/* Anonymous telemetry for the ESPHome Tailscale component.
 *
 * Ported from the esp32-tailscale-subnet-router project, trimmed for the
 * ESPHome ESP-as-tailnet-node use case. On by default; opt out in YAML with
 *   tailscale:
 *     disable_telemetry: true
 *
 * What it does:
 *   After the network is up (+ a grace period) a low-priority task POSTs a
 *   small anonymous JSON event to a Cloudflare Worker over HTTPS, then a
 *   heartbeat roughly once a day.
 *
 *     POST <url>   X-Tlm-Key: <key>   Content-Type: application/json
 *     { dh, v, et, ch, up, bc, rr, ps, cn[, cr] }
 *
 * What it sends (the ENTIRE payload):
 *   dh  anonymous device id  — first 8 bytes of SHA-256(WiFi MAC + fixed salt),
 *                              one-way; cannot be reversed to the MAC
 *   v   component version
 *   et  event type           — "boot" | "heartbeat"
 *   ch  chip model + revision ("S3r0")
 *   up  uptime, seconds
 *   bc  total boot count
 *   rr  ESP-IDF reset reason code — the crash signal: 4=PANIC, 5/6/7=WDT,
 *                               9=BROWNOUT, 1=POWERON, 3=SW (OTA/reboot)
 *   ps  PSRAM present?        — 1 | 0
 *   cn  connected to tailnet? — 1 | 0  (just the bool; NO peers / tailnet name)
 *   cr  crash signature       — OPTIONAL, empty unless the build enables
 *                               ESP-IDF core-dump-to-flash (off by default, and
 *                               the component does NOT force a coredump
 *                               partition — that would break OTA for existing
 *                               installs). When enabled: "task=NAME pc=0xADDR
 *                               bt=0x..,0x.." — code addresses only, no stack.
 *                               Without it, crash visibility comes from rr.
 *
 * What it NEVER sends:
 *   raw MAC, WiFi SSID, IP addresses, tailnet name, peer identities, auth key,
 *   or anything entered in the UI. The source IP is not part of the payload;
 *   the receiving Worker stores only coarse geo (country/region), never the IP.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace esphome {
namespace tailscale {

/* Reported in the "v" field. Bump on each release to match the CHANGELOG. */
#define TAILSCALE_TELEMETRY_VERSION "0.4.0"
#define TAILSCALE_TELEMETRY_URL "https://esphome-tailscale-telemetry.csontikka.workers.dev/v1/event"
#define TAILSCALE_TELEMETRY_KEY "860cbe21c497a43c745715857184058d"

/* Start the telemetry sender. No-op if enabled == false or already started.
 * Increments the persistent boot counter and reads any pending crash summary
 * synchronously, then spawns the (PSRAM-stacked) sender task. Call once the
 * network is up. */
void telemetry_init(bool enabled);

/* Keep the reported tailnet-connection bool current (read at send time). */
void telemetry_set_connected(bool connected);

}  // namespace tailscale
}  // namespace esphome
