# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project aims to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
once a `1.0.0` release is cut. While the version is still in the `0.x` range,
**minor version bumps may include breaking changes** — pin to a specific tag
(not `ref: main`) in your `packages:` block if you need stability.

## [Unreleased]

This section tracks everything on `main` that has not yet been cut into a
tagged release. Once the first tag ships, this entry will be renamed and a
new empty `[Unreleased]` section added above it.

### Added

- **Runtime auth key override** — new `VPN Auth Key Override` text entity
  (password mode) lets you change the Tailscale auth key from HA without
  reflashing. Empty submit reverts to the YAML default. Key is persisted
  in NVS across reboots. New `VPN Auth Key Source` text sensor shows
  `Default (YAML)` or `Override (YYYY-MM-DD HH:MM)` with the timestamp.
- **VPN Auto-Rollback binary sensor** — shows whether turning off VPN would
  trigger the 60 s dead-man's-switch rollback (i.e. HA is connected via
  Tailscale).
- **Consistent "Unknown" sensor states** — when VPN is disconnected, all
  dynamic text sensors, numeric sensors, HA route/IP sensors, and the key
  expiry warning binary sensor now show HA-native "Unknown" instead of blank
  or stale values. Static config values (Control Plane, Login Server, Peers
  Max) and lifetime counters (Connect Count) remain visible.
- **VPN switch OFF/ON reliability** — fixed use-after-free in microlink's
  zombie coord task that caused VPN to auto-reconnect ~18 s after switch OFF.
  Stop/destroy now runs on a background FreeRTOS task to avoid blocking
  loopTask and triggering WDT. Added atomic pointer guard (`s_active_ml`)
  in callbacks and sequencing gate (`s_stop_in_progress`) to prevent
  resource contention during OFF/ON cycles.
- **Pre-publish sensor clearing** — when HA is connected via Tailscale and
  VPN is turned off, sensor clear values are published before the tunnel
  dies (while API is still alive), so HA sees the state change.
- **HA API sensors accuracy** — HA API Connected, Connection Route, and
  Connection IP now correctly reflect real-time API state during VPN
  shutdown (detect Tailscale route and show disconnected).
### Removed

- **`example-dev-tailscale.yaml`** and **`example-dev-headscale.yaml`** —
  consolidated into `example-dev.yaml` (use the commented `login_server`
  line to switch between Tailscale SaaS and Headscale).

### Fixed

- **Microlink logs missing in package builds** — ESPHome defaults
  `CONFIG_LOG_MAXIMUM_LEVEL` to ERROR, which compiles out all ESP_LOGI/ESP_LOGW
  at the preprocessor level. The component now sets
  `CONFIG_LOG_MAXIMUM_LEVEL_INFO` automatically so the debug log switch works
  in all builds (including HA dashboard package installs), not just dev builds.
- **State-aware VPN Setup Hint** — when disconnected, the setup hint sensor now
  shows the current microlink state (Connecting, Registering, Reconnecting,
  Error) instead of a generic "Waiting for VPN..." message, giving users
  actionable feedback when auth_key is wrong or network is unreachable.

### Changed

- **VPN Connect Count** — renamed from "VPN Connections" for clarity.
- **VPN Debug Log switch** — runtime-togglable switch (persisted across
  reboots via NVS) replaces the old `debug_log` YAML option. When OFF
  (default), all microlink INFO logs are suppressed to WARN for a quiet
  serial console; when ON, full diagnostic output is restored. Requires
  `CONFIG_LOG_TAG_LEVEL_IMPL_LINKED_LIST` sdkconfig (auto-set by the
  component).
- **WireGuard printf noise eliminated** — converted raw `printf("[WG_...")`
  calls in the WireGuard lwIP layer to `WG_DEBUG()` macro, which compiles
  to a no-op when `WG_DEBUG_LOGGING=0` (default).
- **Setup Hint URLs** — the VPN Setup Hint sensor now includes clickable
  GitHub README links: key expiry warning points to `#disable-key-expiry`,
  wifi use_address hint points to `#wifi-use-address`. Key expiry warning
  takes priority over the use_address hint.
- **Package YAML updated** — added all missing entities (HA API Connected,
  VPN Auto-Rollback, VPN Hostname, HA API Connection IP, VPN Control Plane,
  VPN Login Server, VPN Network, VPN Connect Count, VPN Debug Log switch)
  and fixed stale key names (`tailscale_enabled` → `vpn_enabled`,
  `tailscale_hostname` → `vpn_hostname`, `tailnet_name` → `network_name`).

- **Tailscale VPN on ESP32** as a drop-in ESPHome external component. The
  device joins your tailnet as a real Tailscale node — no subnet router,
  reverse proxy, or middleman. Built on the
  [microlink](https://github.com/CamM2325/microlink) protocol implementation.
- **Home Assistant entity surface** exposing the live state of the tunnel:
  - **Binary sensors:** `connected`, `key_expiry_warning`
  - **Text sensors:** `ip_address`, `hostname`, `memory_mode`, `setup_status`,
    `peer_status`, `magicdns`, `tailnet_name`, `key_expiry`,
    `ha_connection_route`, `ha_connection_ip`
  - **Numeric sensors:** `peers_online`, `peers_direct`, `peers_derp`,
    `peers_max`, `uptime`
  - **Buttons:** `reconnect` (multi-phase rebind → full restart → safe reboot)
  - **Switches:** `tailscale_enabled` (with 60 s dead-man's-switch rollback
    that restores the previous state if Home Assistant can no longer reach
    the device after the change)
- **Runtime PSRAM detection** — large buffers are enabled automatically when
  PSRAM is present, falling back to small buffers (~30 peers) otherwise.
- **Automatic `wifi: use_address` handling** — the component detects whether
  the configured address matches the actual Tailscale VPN IP at runtime and
  logs a hint if they diverge, so you don't have to hardcode it on first boot.
- **HA API connection route detection** — walks lwIP's TCP pcb table to show
  whether Home Assistant is reaching the device via LAN or via Tailscale, and
  exposes the HA-side IP as a separate sensor.
- **Reconnect state machine** with three escalating phases (rebind, full
  microlink restart, safe reboot) triggered by the `reconnect` button or
  automatic state transitions.
- **Peer capacity warnings** — periodic log lines warn when online peers
  approach or exceed `max_peers`.
- **Periodic diagnostic log summary** every 10 minutes: connection state,
  peer counts by type (direct vs DERP), heap, PSRAM, uptime.
- **SNTP time sync** included in the package so key-expiry timestamps render
  correctly in Home Assistant.
- **Headscale support via `login_server`** — the YAML option points the
  node at a custom control plane (Headscale, or any other Tailscale-
  compatible coordinator) instead of Tailscale SaaS. Empty keeps the
  default. The value may be a bare hostname, an IP, `host:port`, or a
  full `http://host[:port]` URL; `https://` is rejected. The setter
  reaches microlink via a new public `ctrl_host` field on
  `microlink_config_t` (vendored microlink fork change); config-
  supplied values take priority over the NVS-persisted override
  microlink already supported. Authentication, node registration, and
  the streaming MapResponse long-poll against a Headscale 0.23.0 instance
  are verified end-to-end (see *Confirmed working* below). Tailscale
  SaaS remains the default.
- **`contrib/headscale-test/`** — docker-compose harness, minimal
  `config.yaml`, and step-by-step README for standing up a local
  Headscale instance against which the component's auth and register
  flow can be reproduced end-to-end. Not shipped via `packages:`.
- **Packages-based distribution** — end users can drop a one-line
  `packages:` import into their YAML (see `example.yaml`) instead of hand-
  wiring every entity.
- **`example.yaml`** — end-user reference config using the GitHub package.
- **`example-dev.yaml`** — self-contained development config that points at
  the local component checkout and inlines all entity definitions, for
  contributors iterating on the component itself.
- **Comprehensive README** with quick-start, entity reference, configuration
  table, hardware requirements, troubleshooting, how-it-works, credits,
  and a dedicated *Deployment Notes* section covering real-world lessons:
  subnet routers vs userspace WireGuard, auth-key vs node-key expiry, NAT
  traversal realities, the ESPHome package cache footgun, and hardware
  expectations.
- **Screenshots** throughout the README: Home Assistant dashboard, device
  page, auth-key dialog, key-expiry states, web flasher.
- **`SECURITY.md`** describing the vulnerability reporting process.
- **`LICENSE`** (MIT) with proper attribution for microlink, WireGuard,
  Tailscale, and X25519.
- **GitHub Actions workflows:**
  - `validate.yml` — ESPHome config validation on every push and PR.
  - `check-microlink-update.yml` — alerts when the vendored microlink copy
    falls behind its upstream release.
  - `codeql.yml` — GitHub CodeQL static analysis for the Python codegen layer.
- **Dependabot** configuration for automated dependency updates.

### Changed

- **⚠ Breaking — removed the `update_interval` config option.** The
  component is now fully event-driven: sensors publish only when the
  underlying state actually changes, driven by microlink callbacks,
  reconnect transitions, and switch changes. If you had `update_interval:`
  set under `tailscale:`, delete that line — it is now a schema error.
  Diagnostic log cadence (10 min) is now independent of any polling
  interval.
- **⚠ Breaking — auth-key sensors renamed to `key_*`.** The entities
  previously named `auth_key_*` now live under `key_*` (e.g.,
  `key_expiry`, `key_expiry_warning`). This matches the underlying
  Tailscale concept: what the sensor exposes is the *node key* lifecycle,
  not the original auth key. If you had automations referencing
  `sensor.*_auth_key_*`, update them to `sensor.*_key_*`.
- **⚠ Breaking — `Tailscale Peers Total` sensor removed.** It was
  redundant with `Tailscale Peers Online`. Remove any automations that
  referenced it.
- **⚠ Breaking — `Tailscale DERP` switch removed.** Toggling DERP at
  runtime never worked reliably and added confusing state. The reboot
  button added in the same commit provides a cleaner recovery path.
- The default **node-key lifetime is now 180 days** (previously 90 days)
  to match Tailscale's own default.
- **Event-driven sensor publishing** replaced the earlier 30 s polled
  force-publish behaviour. Prior iterations used polling to keep the
  web-server SSE stream alive, but the underlying state-change paths now
  cover every case, and the polling path was pure noise.
- **Thread safety reworked** for the microlink ↔ ESPHome boundary:
  shared state uses `std::atomic`, and all lwIP netif operations are
  wrapped in `LOCK_TCPIP_CORE`.
- `Setup Status` sensor renamed to `Setup Hint` to better reflect its
  purpose (a hint about how to configure `wifi: use_address`).
- The component now uses the
  [Csontikka/microlink](https://github.com/Csontikka/microlink) fork as
  its tracked upstream, vendored as a direct copy into the repo rather
  than a git submodule.
- `example.yaml` now pulls the component + entities via `packages:`
  from GitHub so the build output exactly matches what end users get.
- README badge renames, screenshots refreshed, key-expiry documentation
  rewritten to clarify the relationship between auth keys and node keys.
- Hardware guidance generalised from "ESP32-S3 only" to "ESP32 with a
  currently-tested-on-S3 note," with an explicit acknowledgment that
  only ESP32-S3 + PSRAM has been verified end-to-end.

### Fixed

- **`web_server` + microlink ringbuf assert resolved.** Enabling
  ESPHome's `web_server:` block would reliably crash the device at
  ~50 s uptime with
  `assert failed: prvSendItemDoneNoSplit ringbuf.c:367
  ((pxCurHeader->uxItemFlags & rbITEM_WRITTEN) == 0)`, hit from
  `xRingbufferSendComplete` → `TaskLogBuffer::send_message_thread_safe`
  → `Logger::log_vprintf_non_main_thread_` → `esp_log_va` on the
  `ml_derp_tx_task` path. Root cause: ESPHome's non-main-thread log
  buffer (TaskLogBuffer) races between microlink's high-rate
  `esp_log` calls from `ml_derp_tx_task` / `ml_wg_mgr` and the
  `/events` SSE subscriber that `web_server` adds to the log
  consumer list, corrupting the ringbuf item header. Workaround in
  `example-dev.yaml`: `logger: task_log_buffer_size: 0` disables
  TaskLogBuffer entirely so `esp_log` routes straight to UART. The
  trade-off is that non-main-thread logs no longer reach the HA API
  `/events` stream or the web_server `/events` stream — UART
  (921 600 baud) remains the source of truth for microlink diagnostics.
  Verified 372 s clean with full WireGuard traffic + `web_server: port: 80`
  + 37 tailnet peers. The underlying TaskLogBuffer ringbuf race is an
  ESPHome-core issue and deserves an upstream report separately.
- **~40 s boot-time crash storm resolved.** Under stock settings the
  device would reliably reboot between ~35-45 s uptime with one of
  three symptoms: `task_wdt: loopTask (CPU 1)` watchdog reset,
  `sys_mutex_unlock: failed to give the mutex sys_arch.c:79` from the
  Logger → API log-forward path, or
  `assert failed: lwip_netconn_do_writemore api_msg.c:1738 (offset <
  len)` from `sent_tcp` on the tcpip_thread. Root cause: ESPHome's
  logger calls `uart_write_bytes` on the ESP-IDF UART driver, which
  is installed with `tx_buffer_size=0` and therefore blocks the
  calling task on the hardware-FIFO semaphore whenever the FIFO is
  full. At the default 115 200 baud (~11.5 KB/s) the microlink
  INFO-level log bursts around peer setup / initial MapResponse
  saturate the FIFO, `loopTask` stalls inside `uart_write_bytes`
  for multiple seconds, and the watchdog fires. With
  `LWIP_TCPIP_CORE_LOCKING=y` the stalled `loopTask` was also
  holding the lwIP core lock during the stall, which in turn
  exposed a latent race between `lwip_netconn_do_write`'s
  partial-write UNLOCK/sem_wait/LOCK dance (api_msg.c:1913-1919)
  and the `sent_tcp` callback re-entering `do_writemore` on
  tcpip_thread — the `api_msg.c:1738` assert and the stray mutex
  give failures. The fix has four parts, all in `example-dev.yaml`
  (and any user YAML targeting the same profile) plus one
  microlink change:
  - `logger: baud_rate: 921600` — eight times the throughput of
    the 115 200 default so `uart_write_bytes` drains the FIFO fast
    enough to never block under realistic log volume.
  - `sdkconfig_options: CONFIG_LWIP_TCPIP_CORE_LOCKING: n` — switch
    lwIP from core-locking to the mbox-based api message path so
    the partial-write race window cannot re-appear even if
    `loopTask` stalls briefly for any reason.
  - `sdkconfig_options: CONFIG_LWIP_TCPIP_TASK_STACK_SIZE: "6144"`
    — raise tcpip_thread's stack above the ESP-IDF default 3 072
    so API log-streaming and WireGuard callbacks have headroom
    and cannot silently corrupt adjacent heap objects.
  - `ml_udp.c:201`: `ml_udp_rx` priority lowered from
    `configMAX_PRIORITIES - 2` (23 on a stock build) to `5`. At
    priority 23 pinned to CPU 1 the task could preempt ESPHome's
    priority-1 `loopTask` on the same core; at priority 5 it
    sits in the same tier as other microlink worker tasks and
    can no longer starve the main loop. Not strictly required
    after the baud-rate fix but removes an entire future
    starvation class.
  Verification: 179 s continuous INFO-level stability run against
  the Tailscale SaaS control plane (`controlplane.tailscale.com`)
  with full microlink WireGuard traffic — zero reboots, zero
  asserts, zero `task_wdt` hits. The fix is on the logger UART
  path so it is control-plane-independent; a Headscale endurance
  re-run under the same profile is pending.
- **Headscale initial peer fetch works against non-streaming `serve()`.**
  Headscale v0.28's non-streaming `serve()` path does not write a
  `MapResponse` body for `OmitPeers=false`, so the old two-phase
  `MapRequest` flow (a `Stream=false` peer fetch on stream 3 followed
  by a `Stream=true` long-poll on stream 5) silently hung on stream 3
  and never populated peers. `do_fetch_peers` now sends a single
  `Stream=true` `MapRequest` on stream 5 and reads the initial
  `MapResponse` as the first length-prefixed chunk of the long-poll
  body. The parser was rewritten to use the deterministic 4-byte
  little-endian length prefix instead of the old "scan the first few
  bytes for `{`" heuristic, and to track `h2_parsed` incrementally
  so each Noise frame is parsed once. `do_start_long_poll` is gone —
  the single long-poll is the same connection the initial fetch used.
- **Bulk peer ingest no longer starves IDLE.** On first `MapResponse`
  with ~14 peers, `process_peer_updates` used to drain the entire
  queue in a tight loop. Each `ML_PEER_ADD` does a synchronous NVS
  flash write (~200 ms with NVS cache disabled) plus WireGuard peer
  setup plus a NaCl `box_beforenm` x25519 scalar-mult, so draining
  14 peers back-to-back blocked the `ml_wg_mgr` task for ~3 s and
  tripped `task_wdt` against the IDLE task on its core. Fix: the
  dispatcher now processes **at most one `ML_PEER_ADD` per call**
  and returns so the outer loop's `vTaskDelay(10)` yields. Cheap
  ops (`REMOVE`, `UPDATE_ENDPOINT`) still drain fully per tick.
- **`ml_wg_mgr` moved to CPU 0.** Previously pinned to CPU 1, which
  is also where ESPHome's `loopTask` runs. The WireGuard handshake
  init path calls `x25519` scalar-mult twice (~500 ms each on refc),
  and peer init calls it once more; concentrating all of that on
  CPU 1 starved `loopTask` long enough to trip `task_wdt` on the
  initial MapResponse burst. Moving `ml_wg_mgr` to CPU 0 leaves
  `loopTask` alone on CPU 1.
- **DISCO ping/pong encryption cost reduced ~500 ms → sub-ms.**
  `add_peer` now precomputes the per-peer NaCl `box_beforenm`
  shared secret once at peer-add time and caches it on the peer
  struct. Subsequent `disco_build_ping`, `disco_build_pong`,
  `disco_send_call_me_maybe`, and `process_disco_packet` use
  `box_afternm`/`box_open_afternm`, skipping the x25519 scalar
  multiply on every DISCO packet. Large tailnets that used to
  stutter during periodic DISCO pings now run smoothly.
- **lwIP thread safety** — replaced `ip_input` with `tcpip_input` in the
  WireGuard data path and added `LOCK_TCPIP_CORE` around netif
  operations, eliminating a class of crashes under traffic.
- **Reply routing** — peer reply traffic now tracks per-peer source IPs
  instead of relying on a single `last_rx` fallback, which broke when
  multiple peers were talking to the device simultaneously.
- **`wifi: use_address` injection** — now emitted via `RawExpression`
  C++ code so the ESPHome codegen can safely override the WiFi
  component's configured address at the right point in the init order.
- **`web_server` SSE crashes** — publish sensors only on value change,
  raised `LWIP_MAX_SOCKETS` to 24 to avoid httpd accept errors under the
  additional SSE load.
- **Peers Max sensor** now uses `PEER_SCHEMA` with `accuracy_decimals=0`
  so Home Assistant renders it as an integer count.
- **DERP/Enable switches** — full microlink restart on toggle, switch UI
  rollback if the change fails, HA-API auto-confirm after 30 s if the
  device remained reachable.
- **HA route byte order** — corrected a little-endian / big-endian mixup
  in the HA connection detection logic.
- **Setup Status / Setup Hint** — compares the configured IP with the
  actual VPN IP, not a stale copy.
- **Node-key expiry detection** — values below a sane epoch baseline
  (`2020-01-01 UTC`) are treated as "expiry disabled" (the Tailscale
  control plane sends Go's zero time when an admin disables expiry for
  a node). The `key_expiry` text sensor renders as empty in that state
  so Home Assistant shows "unknown," which is the correct state for a
  timestamp that does not exist. Both "Unknown + OK" and "valid
  timestamp + Warning" are explicitly documented as correct pairs.
- **Auth key no longer logged in plaintext.** Previously the full
  Tailscale auth key was emitted at INFO level inside
  `start_microlink_()`, which meant it landed in the serial console,
  the HA log stream, and any remote log collector the user had wired
  up. The log line now masks the key to its first 12 characters plus
  ellipsis, which is enough to distinguish `tskey-auth-` from
  `tskey-client-` and similar variants during debugging without
  exposing the secret portion.
- **Headscale Noise handshake** — microlink now fetches the server's
  Noise static public key from the Tailscale-compatible `/key?v=88`
  HTTP endpoint at setup time and passes it into `ml_noise_init` as
  the remote static key, replacing the previous behavior of always
  using Tailscale SaaS's hardcoded pubkey. Applies only when
  `login_server` is set; the SaaS path is unchanged. Implemented in
  `microlink/components/microlink/src/ml_coord.c` as a new
  `fetch_server_pubkey()` helper that parses the JSON response,
  extracts the `publicKey` field, strips the `mkey:` prefix, and
  hex-decodes the 32 bytes into a per-instance buffer on
  `microlink_t`.
- **`login_server` URL parsing.** The microlink control-plane host is
  now parsed into host + port + HTTP-Host-header components instead
  of being passed verbatim. Accepts bare hostname, `host:port`,
  `http://host`, and `http://host:port`; the HTTP/1.1 `Host:` header
  and HTTP/2 `:authority` pseudo-header are constructed correctly
  (bare host for port 80, `host:port` otherwise). `https://` is
  rejected because TLS is not implemented in this path. Previously
  the TCP path was hardcoded to port 80 and `ctrl_host` was copied
  raw into the HTTP Host header, which Headscale rejected as
  `400 Bad Request: malformed Host header` for any URL-form value.

### Removed

- **`update_interval` config option** — replaced by the fully
  event-driven publish path (see the Breaking entry above).
- **`Tailscale Peers Total` sensor** — redundant with `Peers Online`.
- **`Tailscale DERP` switch** — runtime toggling was unreliable.
- **Dead `enable_stun` / `enable_disco` knobs** — microlink runs both
  unconditionally and has no way to disable them; the options never
  actually did anything.
- **`tailscale_ip` explicit config parameter** — replaced by runtime
  detection that reads the VPN IP directly from microlink and compares
  it against the WiFi component's `use_address`.
- **Stale "Headscale is not supported" disclaimers** throughout the
  README have been removed in favor of the new Headscale section
  that describes the verified end-to-end auth, register, and
  streaming long-poll paths.
- **SonarCloud integration** — workflow file, `sonar-project.properties`,
  README badges, and the `SONAR_TOKEN` repo secret. Replaced by GitHub's
  native CodeQL static analysis.
- **Stale scaffolding files** from early development: `.gitmodules`,
  `include_fix`, broken symlinks, leftover packages directory.
- **Real Tailscale IPs and tailnet names** scrubbed from tracked files,
  comments, and screenshots (git history was rewritten once to remove
  an earlier leak).

### Security

- **Tailscale auth key is no longer logged in plaintext** on boot
  (`tailscale.cpp:45-47` in earlier revisions). See the Fixed section
  for the full explanation of the fix. This is the reason the v0.1.0
  release is now safe to cut — the prior behavior was a real
  secret-leak path into every log surface the ESPHome runtime
  touches.
- Added `SECURITY.md` describing the vulnerability reporting channel.
- CodeQL static analysis now runs on every push via `.github/workflows/codeql.yml`.
- The CI `validate.yml` workflow now also runs a full `esphome compile`
  of `example.yaml` on every push and pull request, catching C++-level
  breakage in the component or the vendored microlink before it lands
  on `main` and reaches users via `packages: ref: main`. Prior CI only
  ran `esphome config`, which validated YAML schema but never invoked
  the toolchain.
- Git history was rewritten (and force-pushed once, with branch
  protection temporarily relaxed for that single push) to remove a
  previously committed tailnet name and an unmasked device-page
  screenshot. No secrets were in the leaked content, but the cleanup was
  done to keep the public repo free of personal identifiers.

### Known limitations

These are not bugs — they are the current boundaries of what has been
verified. Treat them as the honest answer to "can I rely on this for X?"

- **Only ESP32-S3 with PSRAM is verified end-to-end.** Other ESP32
  variants (classic ESP32, C3, C6, P4) may work via microlink, but are
  not tested by this project. If you try it on a different chip, please
  open an issue with your results.
- **Node-key auto-renewal at 180 days is not yet verified.** The
  component exposes the current expiry timestamp via the `key_expiry`
  sensor and warns via `key_expiry_warning`, but whether microlink
  renews the node key without a device reboot has not been confirmed in
  a long-running deployment. Plan to reflash / reboot the device at
  least once every 180 days until this is verified.
- **Subnet routes and exit-node functionality** are intentionally
  out of scope for this release. The ESP is a *node* on your tailnet,
  not a gateway.
- **No automated tests** beyond the ESPHome config validation CI. The
  component has been tested manually and in a live deployment.

### Confirmed working

- **OTA updates over the Tailscale IP** — flashing the device via its
  `100.x.x.x` tailnet address (while the LAN path is unavailable) has
  been verified end-to-end.
- **Headscale authentication, registration, and streaming long-poll.**
  Against a local Headscale 0.23.0 instance (see `contrib/headscale-test/`
  and the Proxmox dev LXC), the device completes the Noise IK handshake,
  registers via `/machine/register`, and the streaming `/machine/map`
  long-poll on HTTP/2 stream 5 stays open and delivers delta
  `MapResponse` chunks on every periodic endpoint update. Verified with
  both bare-IP (`login_server: "192.168.1.157"`) and URL
  (`login_server: "http://192.168.1.157:80"`) forms. `headscale nodes list`
  shows the node present with IP `100.64.0.1` and online.

---

<!-- Link references for the Keep a Changelog tooling -->
[Unreleased]: https://github.com/Csontikka/esphome-tailscale/commits/main
