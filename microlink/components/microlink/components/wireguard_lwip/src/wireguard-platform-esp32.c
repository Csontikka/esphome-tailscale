/**
 * @file wireguard-platform-esp32.c
 * @brief ESP32 platform implementation for wireguard-lwip
 */

#include "wireguard-platform.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sys.h"
#include <string.h>

/* ============================================================================
 * Time Functions
 * ========================================================================== */

uint32_t wireguard_sys_now() {
    // Use lwIP's built-in time function
    return sys_now();
}

/* WireGuard handshake init carries a TAI64N timestamp the peer compares
 * against stored greatest_timestamp; anything <= stored is treated as
 * replay and DROPped. ESP boot uptime restarts from 0 each reboot, so
 * unless we add a monotonic offset the peer rejects every handshake we
 * send until its replay-state expires.
 *
 * The base offset is supplied by the higher layer (microlink) via
 * wireguard_set_tai64n_base_seconds() because that layer already owns
 * NVS access and can decide whether to use SNTP wall clock or a
 * persisted per-boot counter. */
static uint64_t s_tai_base_seconds = 0;

void wireguard_set_tai64n_base_seconds(uint64_t base_seconds) {
    s_tai_base_seconds = base_seconds;
}

void wireguard_tai64n_now(uint8_t *output) {
    uint64_t now_us = esp_timer_get_time();
    uint64_t seconds = s_tai_base_seconds + (now_us / 1000000ULL);
    uint32_t nanoseconds = (now_us % 1000000ULL) * 1000;
    static uint64_t last_log = 0;
    if (seconds - last_log > 10) {
        printf("[TAI64N] base=%llu now=%llu\n",
               (unsigned long long)s_tai_base_seconds,
               (unsigned long long)seconds);
        last_log = seconds;
    }

    /* TAI64 base: 2^62 + Unix time (TAI is +10s from UTC at epoch). */
    seconds += 0x400000000000000AULL;

    output[0] = (seconds >> 56) & 0xFF;
    output[1] = (seconds >> 48) & 0xFF;
    output[2] = (seconds >> 40) & 0xFF;
    output[3] = (seconds >> 32) & 0xFF;
    output[4] = (seconds >> 24) & 0xFF;
    output[5] = (seconds >> 16) & 0xFF;
    output[6] = (seconds >> 8) & 0xFF;
    output[7] = seconds & 0xFF;

    output[8]  = (nanoseconds >> 24) & 0xFF;
    output[9]  = (nanoseconds >> 16) & 0xFF;
    output[10] = (nanoseconds >> 8) & 0xFF;
    output[11] = nanoseconds & 0xFF;
}

/* ============================================================================
 * Random Number Generation
 * ========================================================================== */

void wireguard_random_bytes(void *bytes, size_t size) {
    // Use ESP32 hardware RNG
    esp_fill_random(bytes, size);
}

/* ============================================================================
 * Load Management
 * ========================================================================== */

bool wireguard_is_under_load() {
    // For now, always return false (not under load)
    // Could be enhanced to check:
    // - Free heap memory
    // - CPU usage
    // - Number of active connections
    return false;
}
