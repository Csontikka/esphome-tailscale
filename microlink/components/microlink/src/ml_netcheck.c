/**
 * @file ml_netcheck.c
 * @brief DERP region latency probing (a la Tailscale Go netcheck).
 *
 * Sends a STUN binding request to one node per known DERP region in
 * parallel on a single UDP socket, collects RTT via the binding
 * response (matched on transaction ID), and returns the region_id
 * with the lowest measured RTT.
 *
 * Reference: tailscale/net/netcheck/netcheck.go
 *
 * SPDX-License-Identifier: MIT
 */
#include "microlink_internal.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <errno.h>
#include <string.h>

static const char *TAG = "ml_netcheck";

#define NETCHECK_TIMEOUT_MS    25000 /* Total budget — covers a 19-second STUN retry seen in ml_stun */
#define NETCHECK_RETRY_GAP_MS  5000  /* Re-send to non-responded regions every 5 s, mimics tailscale Go */
#define NETCHECK_MAX_RETRIES   3     /* Initial send + up to 3 retries = 4 attempts/region */
#define NETCHECK_PROBE_GAP_MS  50    /* Spread sendto() to avoid local NAT/UDP burst loss */
#define NETCHECK_MAX_PROBES    ML_MAX_DERP_REGIONS

/* Build a 40-byte STUN Binding Request with SOFTWARE + FINGERPRINT.
 * SOFTWARE MUST be "tailnode" — the Tailscale DERP STUN servers reject
 * requests with any other SOFTWARE value (verified empirically against
 * derp4i, derp9d; only "tailnode" gets a reply). Matches the payload in
 * ml_stun.c. Returns total length (always 40). */
static size_t build_stun_binding(uint8_t *out, uint8_t *txid_out) {
    out[0] = 0x00; out[1] = 0x01;            /* Binding Request */
    out[2] = 0x00; out[3] = 20;              /* Length: 12B SOFTWARE-TLV + 8B FINGERPRINT-TLV = 20 */
    out[4] = 0x21; out[5] = 0x12; out[6] = 0xA4; out[7] = 0x42;  /* Magic Cookie */
    esp_fill_random(txid_out, 12);
    memcpy(out + 8, txid_out, 12);

    /* SOFTWARE attribute: type 0x8022, len 8, "tailnode" */
    out[20] = 0x80; out[21] = 0x22;
    out[22] = 0x00; out[23] = 0x08;
    memcpy(out + 24, "tailnode", 8);

    /* FINGERPRINT attribute: type 0x8028, len 4, CRC32(prev)^0x5354554E */
    out[32] = 0x80; out[33] = 0x28;
    out[34] = 0x00; out[35] = 0x04;
    uint32_t fp = esp_crc32_le(0, out, 36) ^ 0x5354554EUL;
    out[36] = (fp >> 24) & 0xFF;
    out[37] = (fp >> 16) & 0xFF;
    out[38] = (fp >>  8) & 0xFF;
    out[39] =  fp        & 0xFF;
    return 40;
}

static uint32_t resolve_v4(const char *hostname) {
    if (!hostname || !hostname[0]) return 0;
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(hostname, NULL, &hints, &res) != 0 || !res) return 0;
    uint32_t ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return ip;
}

uint16_t ml_netcheck_pick_best_derp(microlink_t *ml) {
    if (!ml || ml->derp_region_count == 0) return 0;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket() failed: %d", errno);
        return 0;
    }

    /* Bind the UDP socket to the WiFi STA IP so probes egress the upstream
     * link, not the WireGuard tunnel. Critical when exit-node mode has set
     * netif_default to the WG netif — otherwise probes get filtered by the
     * WG AllowedIPs check and never reach the public STUN servers. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            struct sockaddr_in local = {0};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = ip_info.ip.addr;
            local.sin_port = 0;  /* ephemeral */
            if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
                ESP_LOGW(TAG, "bind to STA %lu.%lu.%lu.%lu failed: %d",
                         (unsigned long)(ip_info.ip.addr & 0xFF),
                         (unsigned long)((ip_info.ip.addr >> 8) & 0xFF),
                         (unsigned long)((ip_info.ip.addr >> 16) & 0xFF),
                         (unsigned long)((ip_info.ip.addr >> 24) & 0xFF),
                         errno);
            } else {
                ESP_LOGI(TAG, "bound netcheck socket to STA %lu.%lu.%lu.%lu",
                         (unsigned long)(ip_info.ip.addr & 0xFF),
                         (unsigned long)((ip_info.ip.addr >> 8) & 0xFF),
                         (unsigned long)((ip_info.ip.addr >> 16) & 0xFF),
                         (unsigned long)((ip_info.ip.addr >> 24) & 0xFF));
            }
        }
    }

    struct probe {
        uint8_t  txid[12];      /* current (last-sent) txid */
        uint64_t send_us;       /* last send time, for RTT math */
        uint16_t region_id;
        uint32_t dest_ip;       /* cached for retries */
        uint16_t dest_port;
        uint32_t rtt_us;
        bool     got_response;
    } probes[NETCHECK_MAX_PROBES];
    int probe_count = 0;

    int regions = ml->derp_region_count < NETCHECK_MAX_PROBES ?
                  ml->derp_region_count : NETCHECK_MAX_PROBES;
    for (int i = 0; i < regions; i++) {
        ml_derp_region_t *r = &ml->derp_regions[i];
        if (r->region_id == 0) continue;
        const char *host = NULL;
        uint16_t stun_port = 3478;
        for (int j = 0; j < r->node_count; j++) {
            if (r->nodes[j].hostname[0]) {
                host = r->nodes[j].hostname;
                if (r->nodes[j].stun_port > 0) stun_port = r->nodes[j].stun_port;
                break;
            }
        }
        if (!host) continue;
        uint32_t ip = resolve_v4(host);
        if (ip == 0) continue;

        uint8_t pkt[40];
        size_t pkt_len = build_stun_binding(pkt, probes[probe_count].txid);

        struct sockaddr_in dest = {0};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(stun_port);
        dest.sin_addr.s_addr = ip;

        probes[probe_count].region_id = r->region_id;
        probes[probe_count].dest_ip = ip;
        probes[probe_count].dest_port = stun_port;
        probes[probe_count].rtt_us = 0;
        probes[probe_count].got_response = false;
        probes[probe_count].send_us = esp_timer_get_time();

        if (sendto(sock, pkt, pkt_len, 0, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
            ESP_LOGW(TAG, "send to region %d (%s) failed: %d", r->region_id, host, errno);
            continue;
        }
        probe_count++;
        vTaskDelay(pdMS_TO_TICKS(NETCHECK_PROBE_GAP_MS));
    }
    ESP_LOGI(TAG, "sent %d STUN probes, retrying every %d ms up to %d ms total",
             probe_count, NETCHECK_RETRY_GAP_MS, NETCHECK_TIMEOUT_MS);

    uint64_t deadline = esp_timer_get_time() + (uint64_t)NETCHECK_TIMEOUT_MS * 1000ULL;
    uint64_t next_retry_at = esp_timer_get_time() + (uint64_t)NETCHECK_RETRY_GAP_MS * 1000ULL;
    int retries_done = 0;

    while (esp_timer_get_time() < deadline) {
        uint64_t now = esp_timer_get_time();

        /* Retry sweep: re-send to any region we haven't heard from yet.
         * Regenerates the txid so the new attempt is the one we match
         * against — old/late responses are silently dropped. */
        if (retries_done < NETCHECK_MAX_RETRIES && now >= next_retry_at) {
            int retried = 0;
            for (int i = 0; i < probe_count; i++) {
                if (probes[i].got_response) continue;
                uint8_t pkt[40];
                size_t pkt_len = build_stun_binding(pkt, probes[i].txid);
                struct sockaddr_in dest = {0};
                dest.sin_family = AF_INET;
                dest.sin_port = htons(probes[i].dest_port);
                dest.sin_addr.s_addr = probes[i].dest_ip;
                if (sendto(sock, pkt, pkt_len, 0, (struct sockaddr *)&dest, sizeof(dest)) >= 0) {
                    probes[i].send_us = esp_timer_get_time();
                    retried++;
                }
                vTaskDelay(pdMS_TO_TICKS(NETCHECK_PROBE_GAP_MS));
            }
            retries_done++;
            ESP_LOGI(TAG, "retry %d/%d: re-sent %d probes",
                     retries_done, NETCHECK_MAX_RETRIES, retried);
            next_retry_at = esp_timer_get_time() + (uint64_t)NETCHECK_RETRY_GAP_MS * 1000ULL;
        }

        /* Wait for either next retry tick or the deadline, whichever is sooner. */
        uint64_t wait_until = (retries_done < NETCHECK_MAX_RETRIES && next_retry_at < deadline)
                              ? next_retry_at : deadline;
        now = esp_timer_get_time();
        if (wait_until <= now) continue;
        uint64_t remain = wait_until - now;

        fd_set rfds; FD_ZERO(&rfds); FD_SET(sock, &rfds);
        struct timeval tv;
        tv.tv_sec  = remain / 1000000;
        tv.tv_usec = remain % 1000000;
        int rs = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (rs <= 0) continue;   /* timed out before next retry tick; loop & re-check */

        uint8_t buf[256];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &fromlen);
        if (n < 20) continue;
        if (buf[0] != 0x01 || buf[1] != 0x01) continue;  /* not Binding Response */

        uint64_t recv_us = esp_timer_get_time();
        for (int i = 0; i < probe_count; i++) {
            if (probes[i].got_response) continue;
            if (memcmp(buf + 8, probes[i].txid, 12) != 0) continue;
            probes[i].rtt_us = (uint32_t)(recv_us - probes[i].send_us);
            probes[i].got_response = true;
            ESP_LOGI(TAG, "region %d RTT=%lu ms", probes[i].region_id,
                     (unsigned long)(probes[i].rtt_us / 1000));

            /* Walk TLVs for XOR-MAPPED-ADDRESS (type 0x0020). RFC 5389:
             * value = reserved(1) | family(1) | x-port(2) | x-addr(4 or 16),
             * X-bytes XORed with the magic cookie 0x2112A442. */
            if (ml->stun_public_ip == 0) {
                size_t pos = 20;
                while (pos + 4 <= (size_t)n) {
                    uint16_t atype = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
                    uint16_t alen  = ((uint16_t)buf[pos + 2] << 8) | buf[pos + 3];
                    pos += 4;
                    if (pos + alen > (size_t)n) break;
                    if (atype == 0x0020 && alen >= 8 && buf[pos + 1] == 0x01) {
                        uint8_t a = buf[pos + 4] ^ 0x21;
                        uint8_t b = buf[pos + 5] ^ 0x12;
                        uint8_t c = buf[pos + 6] ^ 0xA4;
                        uint8_t d = buf[pos + 7] ^ 0x42;
                        ml->stun_public_ip =
                            ((uint32_t)a << 24) | ((uint32_t)b << 16) |
                            ((uint32_t)c << 8)  |  (uint32_t)d;
                        char ip_str[16];
                        microlink_ip_to_str(ml->stun_public_ip, ip_str);
                        ESP_LOGI(TAG, "STUN public IP: %s (from region %d)",
                                 ip_str, probes[i].region_id);
                        break;
                    }
                    pos += (alen + 3U) & ~3U;
                }
            }
            break;
        }
    }
    close(sock);

    uint32_t best_rtt = UINT32_MAX;
    uint16_t best_region = 0;

    /* Store per-region RTTs in ml->derp_rtt_ms aligned with derp_regions[].
     * Clear first so a region with no response shows 0 in the snapshot. */
    for (int i = 0; i < ML_MAX_DERP_REGIONS; i++) ml->derp_rtt_ms[i] = 0;
    for (int i = 0; i < probe_count; i++) {
        if (!probes[i].got_response) continue;
        if (probes[i].rtt_us < best_rtt) {
            best_rtt = probes[i].rtt_us;
            best_region = probes[i].region_id;
        }
        for (int r = 0; r < ml->derp_region_count; r++) {
            if (ml->derp_regions[r].region_id == probes[i].region_id) {
                uint32_t ms = probes[i].rtt_us / 1000;
                ml->derp_rtt_ms[r] = ms > 0xFFFF ? 0xFFFF : (uint16_t)ms;
                break;
            }
        }
    }
    if (best_region == 0) {
        ESP_LOGW(TAG, "no DERP region responded — falling back to default");
    } else {
        ESP_LOGI(TAG, "best DERP = region %d (%lu ms)",
                 best_region, (unsigned long)(best_rtt / 1000));
    }
    return best_region;
}
