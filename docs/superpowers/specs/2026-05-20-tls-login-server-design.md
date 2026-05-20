# TLS support in microlink for `login_server`

**Date:** 2026-05-20
**Status:** Approved (brainstorming) — pending implementation plan
**Owner:** jonny190
**Branch:** `tls-login-server`

## Background

The `esphome-tailscale` external component embeds [microlink](https://github.com/CamM2325/microlink), a C implementation of the Tailscale client protocol, into ESPHome firmware. Microlink intentionally omits TLS on the wire because Tailscale's Noise IK protocol provides application-layer confidentiality and integrity. Comments in `ml_coord.c`:

```c
// 2. TCP connect to port 80 (NOT TLS - Noise provides encryption)
// https:// is rejected (TLS is out of scope).
```

In practice this is fine for Tailscale SaaS (port 80 + Noise) and for self-hosted Headscale instances whose operator can expose plain HTTP. It is **not** fine for Headscale instances behind an HTTPS-terminating reverse proxy (Caddy, nginx, Cloudflare tunnel), where the only ingress is `https://...`. The current code rejects such URLs at parse time:

```
E (73241) ml_coord: https:// control plane URL is not supported (TLS unimplemented)
E (73243) ml_coord: Invalid login_server 'https://vpn.daveys.xyz'
```

This design adds optional TLS transport so `https://` `login_server` URLs work end-to-end. The Noise tunnel remains unchanged and continues to provide the actual security; TLS adds defense-in-depth (notably, MITM protection on the initial `/key?v=88` fetch that bootstraps the Noise server public key).

## Goal

A microlink+esphome-tailscale build that:

1. Accepts `https://host[:port]` as a `login_server` value.
2. Performs a TLS 1.2/1.3 handshake against the host, verifying the cert against the ESP-IDF bundled Mozilla CA roots.
3. Completes the existing Noise handshake, registration, and `/machine/map` long-poll over the TLS-wrapped socket.
4. Leaves the plain-HTTP path completely unchanged for SaaS and bare-IP Headscale users.

## Non-goals

- Tailscale SaaS coordinator path — stays plain-HTTP via Noise as today.
- Client certificate / mutual TLS — Headscale doesn't use it.
- HTTP/2 ALPN negotiation on the transport — the Noise tunnel multiplexes HTTP/2 frames internally; the outer transport is opaque bytes from TLS's perspective.
- TLS 1.0/1.1 fallback — `esp_tls` defaults are appropriate.

## Repos & branching

- Fork `Csontikka/esphome-tailscale` → `jonny190/esphome-tailscale`. Microlink is vendored (not a submodule); all C changes live in the single fork.
- Work happens on branch `tls-login-server` off `main`.
- A small precursor commit on the same branch fixes a pre-existing upstream issue (`tailscale.h` did not `#include "esphome/core/defines.h"`), without which the build was already broken on ESPHome ≥ 2026.x. This is independently useful and may be PR'd upstream separately.

## Files changed

| File | Change |
| --- | --- |
| `microlink/components/microlink/src/ml_coord.c` | URL parser accepts `https://`; new `ml_conn_write` / `ml_conn_read` helpers; `do_tcp_connect` and `fetch_server_pubkey` gain TLS branches; `coord_send` / `coord_recv` and the two loose `ml_recv` sites route through the new helpers. |
| `microlink/components/microlink/include/microlink.h` (or internal header) | Add `esp_tls_t *coord_tls` and `bool use_tls` to the connection state. |
| `microlink/components/microlink/CMakeLists.txt` | Add `esp_tls` to `REQUIRES`. |
| `components/tailscale/__init__.py` | Relax the YAML validator to accept `https://`. Add the sdkconfig fragments needed for the certificate bundle. |

Suggested sdkconfig additions (applied via the component's `add_idf_sdkconfig_option` calls):

```
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
```

## Implementation approach

A **connection-layer dispatch**, not parallel code paths. Two new static helpers in `ml_coord.c`:

```c
static int ml_conn_write(microlink_t *ml, const uint8_t *buf, size_t len);
static int ml_conn_read (microlink_t *ml, uint8_t *buf, size_t len);
```

Each inspects `ml->use_tls` and dispatches to either `ml_send`/`ml_recv` (existing raw socket path) or `esp_tls_conn_write`/`esp_tls_conn_read` (TLS path).

Call sites updated to go through these helpers:

- `coord_send` (line 304) — straight wrap.
- `coord_recv` (line 322) — straight wrap.
- Loose `ml_recv(ml->coord_sock, ...)` at lines 598 and 732 — replaced with `ml_conn_read`.

URL parser change (around line 71 in `ml_coord.c`):

```c
} else if (strncasecmp(p, "https://", 8) == 0) {
    ml->use_tls = true;
    p += 8;
    default_port = "443";
    // fall through into the existing host[:port] parsing
}
```

Connection setup (`do_tcp_connect`, `fetch_server_pubkey`) gets a TLS branch:

```c
if (ml->use_tls) {
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .common_name = ml->coord_host,    // SNI + cert verification hostname
    };
    ml->coord_tls = esp_tls_init();
    if (esp_tls_conn_new_sync(ml->coord_host, strlen(ml->coord_host),
                              atoi(ml->coord_port), &cfg, ml->coord_tls) != 1) {
        esp_tls_conn_destroy(ml->coord_tls);
        ml->coord_tls = NULL;
        return -1;
    }
    ml->coord_sock = -1;  // signal "no raw socket"
} else {
    /* existing socket()/connect() path, unchanged */
}
```

(`esp_tls_conn_new_sync` API form depends on the ESP-IDF version shipped with the ESPHome platform — implementation phase will pin the exact signature.)

Teardown adds a symmetric `if (ml->coord_tls) { esp_tls_conn_destroy(ml->coord_tls); ml->coord_tls = NULL; }` next to the existing `close(ml->coord_sock)`.

## CA verification

`esp_crt_bundle_attach` activates ESP-IDF's bundled Mozilla root CA list (~70 KB flash in the FULL variant — hashes only; cert bytes are lazy-loaded during chain validation). This verifies any publicly-trusted cert including the Let's Encrypt / Cloudflare chains serving `vpn.daveys.xyz`. No per-device cert pinning; cert rotations on the Headscale side are transparent.

## Noise / HTTP/2 impact

Zero. The Noise tunnel encrypts and frames application data **inside** the transport stream; the HTTP/2 multiplexing happens **inside** Noise. Once `ml_conn_write` / `ml_conn_read` route through TLS, every layer above (Noise framing, h2 framing, `/machine/register`, `/machine/map`) is byte-for-byte identical to today.

## Error handling

- TLS handshake failure (cert chain, hostname mismatch, timeout): logged via `esp_tls`'s built-in error reporting; returns the same error path as a TCP connect failure, so the existing reconnect state machine retries.
- Partial reads from `esp_tls_conn_read`: same loop pattern as `ml_recv` — the connection wrapper handles `< len` returns transparently.
- `EAGAIN`/`WOULDBLOCK` on non-blocking TLS reads: `esp_tls` returns `ESP_TLS_ERR_SSL_WANT_READ` / `ESP_TLS_ERR_SSL_WANT_WRITE`; treat as "no data, retry" exactly like the existing `select()` paths.
- Double-free safety: `coord_tls` set to NULL after every `esp_tls_conn_destroy`.

## Testing

Three checkpoints, in order:

1. **Compile clean** — no new warnings beyond baseline; the existing `compile` succeeds with the patched component and `https://vpn.daveys.xyz` as the `login_server`.
2. **TLS handshake completes** — device logs show an `esp-tls` line confirming a successful handshake to `vpn.daveys.xyz:443`. We expect ~500 ms-2 s for handshake on first connect.
3. **Device registers in Headscale** — `headscale nodes list` on the VPS shows `esp32-tailscale` with a `100.64.x.x` IP. On-device: `VPN Connected` binary sensor reads `ON`, `VPN IP` text sensor populated, `VPN Setup Hint` clears.

Failure mode triage:

- Stuck at checkpoint 1 → C-level compile errors; iterate locally.
- Stuck at checkpoint 2 → enable `esp-tls` debug logging (`esp_log_level_set("esp-tls", ESP_LOG_DEBUG)`) and re-flash to capture cert chain / SNI / timeout details. If cert chain fails, capture which CA is missing — possible bundle variant mismatch.
- Stuck at checkpoint 3 → TLS is fine but the existing Noise / HTTP/2 path is misbehaving. Compare against the working plain-HTTP path (same code minus the TLS wrap) to isolate.

## Rollout

1. All changes live on `tls-login-server` in the local clone.
2. Once checkpoint 3 passes, push the branch to `jonny190/esphome-tailscale`.
3. Update `/config/esphome-web-18e080.yaml` to point at `https://github.com/jonny190/esphome-tailscale` (ref: `tls-login-server`) instead of the local path.
4. Re-flash via the fork URL to confirm a clean end-to-end build from GitHub.
5. Optionally open a PR upstream once it's been running stably for a few days.

## Open questions

None that block implementation. The exact `esp_tls_conn_new_sync` signature depends on the ESP-IDF version pinned by the current ESPHome / pioarduino release; the implementation plan will verify it against the headers in the build environment before writing the call.
