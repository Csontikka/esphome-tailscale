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
- **Headscale references and claims** throughout the README — this
  project targets the official Tailscale control plane only. Headscale
  is not tested and no compatibility is promised.
- **SonarCloud integration** — workflow file, `sonar-project.properties`,
  README badges, and the `SONAR_TOKEN` repo secret. Replaced by GitHub's
  native CodeQL static analysis.
- **Stale scaffolding files** from early development: `.gitmodules`,
  `include_fix`, broken symlinks, leftover packages directory.
- **Real Tailscale IPs and tailnet names** scrubbed from tracked files,
  comments, and screenshots (git history was rewritten once to remove
  an earlier leak).

### Security

- Added `SECURITY.md` describing the vulnerability reporting channel.
- CodeQL static analysis now runs on every push via `.github/workflows/codeql.yml`.
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
- **Headscale is not supported.** Only the official Tailscale control
  plane is tested.
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

---

<!-- Link references for the Keep a Changelog tooling -->
[Unreleased]: https://github.com/Csontikka/esphome-tailscale/commits/main
