# ESPHome Tailscale

![GitHub release (latest by date)](https://img.shields.io/github/v/release/Csontikka/esphome-tailscale?style=plastic)
[![ESPHome External Component](https://img.shields.io/badge/ESPHome-external%20component-black?style=plastic&logo=esphome&logoColor=white)](https://esphome.io/components/external_components.html)
[![MIT License](https://img.shields.io/badge/License-MIT-yellow.svg?style=plastic)](https://github.com/Csontikka/esphome-tailscale/blob/main/LICENSE)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-donate-yellow.svg?style=plastic)](https://buymeacoffee.com/csontikka)

> [!NOTE]
> **📦 First public release** — works on our hardware, but you're among the first to try it. Bug reports and feedback are welcome!

> Native Tailscale VPN on **ESP32** as a plug-and-play ESPHome external component.
> Your ESP joins your tailnet as a real Tailscale node — no subnet router, no reverse proxy, no middleman.
>
> *Active testing & development currently happens on **ESP32-S3 with PSRAM**. Other ESP32 variants may work but are not yet verified — see [Requirements](#requirements).*

![ESPHome Tailscale Hero](docs/images/hero.png)
<!-- IMAGE: Home Assistant dashboard screenshot showing the Tailscale card with connected state, IP, peers, route, uptime. This is the "sales pitch" image at the top. -->

---

## Why

Home Assistant users often run their HA instance at home behind a NAT, but they want to reach remote sensors — a weekend cabin, a parent's house, a workshop — without poking holes in someone else's firewall.

The traditional answer is **subnet routers**: put a Tailscale node on the remote network and route the ESP's LAN IP through it. This works, but it:

- needs an always-on machine at the remote site,
- adds a hop,
- depends on subnet-routing ACLs,
- and turns simple "device lives in my tailnet" into "device lives behind a gateway I have to maintain".

**This component removes the middleman.** The ESP32 itself becomes a Tailscale node with its own `100.x.x.x` address, showing up in your `tailscale status` list like any laptop or phone. Home Assistant connects to it directly over the tailnet — LAN, mobile data, anywhere — with the same rock-solid `api:` + `ota:` + `web_server:` stack you already trust.

![Architecture Diagram](docs/images/architecture.png)
<!-- IMAGE: Simple architecture diagram. Left: Home (HA, phone) inside a "Tailnet" box with a cloud icon. Right: Remote site (ESP32 with the tailscale component) also inside the Tailnet box. Arrow "direct WireGuard (no subnet router)". Below: Tailscale control plane cloud. -->

---

## Features

- **Pure Tailscale node** — the ESP joins your tailnet directly, no subnet router needed on either side.
- **Works with official Tailscale and Headscale** — connects to Tailscale SaaS out of the box; set `login_server` to point at a self-hosted [Headscale](https://github.com/juanfont/headscale) instance instead.
- **Home Assistant native** — exposes a full set of Home Assistant entities out of the box: connection status, VPN IP, hostname, peer counts, node key expiry, uptime, MagicDNS name, peer status, memory mode, HA connection route, runtime auth key override, reboot/reconnect buttons, enable switch.
- **HA API Connection Route sensor** — tells you *how* HA is currently reaching the device: `Tailscale Direct`, `Tailscale DERP`, or `Local`. Great for debugging connectivity.
- **Node key expiry sensor + warning** — surfaces the node key expiry timestamp from the Tailscale control plane plus a `problem` binary sensor that turns off the moment you click "Disable key expiry".
- **Runtime auth key override** — change the Tailscale auth key from HA without reflashing. Persisted in NVS across reboots.
- **PSRAM-aware** — auto-detects PSRAM and scales internal buffers (supports large tailnets with 50+ peers).
- **Self-healing reconnect** — three-phase recovery (rebind → full restart → reboot) when the tailnet link goes stale.
- **Auto `use_address` hint** — tells you exactly which line to add to your YAML so HA finds the device over Tailscale after first boot.
- **Package-based install** — one `packages:` line in your YAML and all entities appear.

---

## Requirements

### Hardware

- An **ESP32** board with **PSRAM** (recommended: 8 MB Octal PSRAM).
- **At least 4 MB flash** — enough for the bootloader plus two OTA slots of the ~1 MB firmware. 8 MB or more is only useful if you want to stack other large ESPHome components next to Tailscale.

> **Current testing target:** active development and flashing is being done on **ESP32-S3**. Other ESP32 variants (classic ESP32, ESP32-C3, ESP32-C6, ESP32-P4, …) may work through the upstream [microlink](https://github.com/CamM2325/microlink) library, but they are **not yet verified** by this project. If you get the component running on a non-S3 chip, please open an issue / PR so we can list it here.

Boards currently verified:

- **ESP32-S3-DevKitC-1** (8 MB PSRAM, 16 MB flash) — the reference / test board
- **ESP32-S3-N16R8**

> **Why PSRAM?** The Tailscale control protocol and WireGuard crypto state together need more RAM than a plain ESP32 has. Without PSRAM the component falls back to small buffers and caps around 30 peers — fine for small tailnets, rough for larger ones.

### Software

- **ESPHome 2026.3.1** or newer
- **ESP-IDF framework** (not Arduino) — this is enforced automatically by the package
- **Home Assistant** with the ESPHome integration enabled
- A **Tailscale account** (or a self-hosted [Headscale](https://github.com/juanfont/headscale) instance)

---

## Quick Start

### 1. Create a Tailscale auth key

Log in to the [Tailscale admin console](https://login.tailscale.com/admin/settings/keys) and go to **Settings → Personal Settings → Keys**. Click **Generate auth key...**.

![Tailscale Keys Page](docs/images/tailscale-keys-page.png)

Then fill in the dialog:

![Tailscale Auth Key Creation](docs/images/tailscale-auth-key-create.png)

**Recommended settings:**

| Option | Value | Why |
| --- | --- | --- |
| **Description** | `esphome-<devicename>` | So you can identify it later |
| **Reusable** | ✅ On | Lets you re-flash without regenerating a key |
| **Ephemeral** | ❌ Off | Ephemeral nodes get garbage-collected when offline, bad for ESPs that sleep or reboot |
| **Pre-approved** | ✅ On *(if your tailnet uses device approval)* | So the ESP can join without a manual click |
| **Tags** | `tag:esphome` *(optional)* | Useful for ACL targeting |
| **Expiration** | 90 days *(max)* | Tailscale caps this — see **Disable key expiry** below to make the resulting node key permanent |

Copy the key (starts with `tskey-auth-...`) — you'll paste it into your ESPHome secrets in a moment.

<a id="disable-key-expiry"></a>

> ### 🔑 Auth key vs. node key — important
>
> Tailscale has **two** different kinds of key, and they're easy to confuse:
>
> - **Auth key** (`tskey-auth-...`) — a *one-time ticket* that lets a new device register itself with your tailnet. The ESP only uses this on its very first boot, to prove to the control plane "I'm allowed to join". **After the first successful registration the auth key is no longer needed for normal operation** — the device has received its own private **node key** and talks to the tailnet with that from then on. Tailscale still shows the auth key in the admin console with a 90-day max expiry, but this does **not** kick the device off the tailnet when it expires — it only stops *new* devices from being able to register with the same key.
> - **Node key** — the per-device long-term identity, generated and stored on the ESP itself (in NVS). This is the key that actually keeps the device authenticated on the tailnet. **This one *does* have its own expiry**, and by default Tailscale expires node keys after ~180 days, after which the device is kicked off the tailnet and needs to be re-authenticated (which means re-flashing or re-running the auth flow — ugly on an unattended sensor at a remote cabin).
>
> **What this means in practice:**
>
> 1. Generate an auth key, flash the ESP with it, let the device register.
> 2. **Then go to the Tailscale admin → Machines → your ESP → ⋯ → "Disable key expiry".** This tells Tailscale "this specific device's *node* key never expires". Now the ESP stays on the tailnet forever, regardless of whether the original auth key expires.
> 3. **Reboot the ESP** (press the Reboot button in HA or power-cycle it). The updated expiry status is only fetched from the control plane during a fresh login — a simple reconnect is not enough. After the reboot, the **VPN Node Key Expiry Warning** sensor should turn off and the **VPN Setup Hint** will switch from the key-expiry URL to the `use_address` hint.
> 4. After that, the auth key is basically discardable — if it expires, nothing happens to the already-registered device. You only need a new auth key if you want to flash *another* ESP, or re-register this one from scratch (e.g. after `esptool erase_flash`).
>
> If you skip step 2, everything will *look* fine for months, and then one day the device silently drops off the tailnet and you'll have no idea why. **Disable node key expiry. Always. For every ESP you flash.**

### 2. Install the component

Create a minimal ESPHome YAML:

```yaml
esphome:
  name: esp32-tailscale
  friendly_name: ESP32 Tailscale

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

packages:
  tailscale:
    url: https://github.com/Csontikka/esphome-tailscale
    ref: main
    files: [packages/tailscale/tailscale.yaml]
    refresh: 0s

tailscale:
  auth_key: !secret tailscale_auth_key
  hostname: "esp32-tailscale"

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

logger:
  level: DEBUG

api:

# Optional — only for the initial bring-up test.
# web_server exposes a tiny HTTP page at http://<device-ip>/ that shows
# the live state of all entities. Handy to verify "yes, Tailscale is up
# and I can reach the device over its 100.x.y.z address from my browser".
# Once Home Assistant is connected over the API, you don't need this —
# feel free to remove the block, it just uses flash and RAM for nothing.
web_server:
  port: 80

ota:
  platform: esphome
```

And add to your `secrets.yaml`:

```yaml
wifi_ssid: "YourWiFi"
wifi_password: "YourWiFiPassword"
tailscale_auth_key: "tskey-auth-xxxxxxxxxxxxxxxxxxxxxxxxxxx"
```

### 3. First flash

The device isn't on the tailnet yet, so OTA can't reach it. You need to get the firmware onto the device at least once before Tailscale can take over. Pick whichever method is least painful for you — they all produce the same result.

#### Option A — ESPHome Web (recommended for most users)

No CLI, no USB driver hunts, no Python install. Just a Chrome/Edge browser tab.

1. In your ESPHome dashboard (or your local ESPHome install) **compile** the device YAML to produce a binary. From the HA ESPHome add-on this is the "Install → Manual download (modern format)" button.
2. Plug the ESP32 into a computer via USB.
3. Open [web.esphome.io](https://web.esphome.io/) in Chrome or Edge.
4. Click **Connect**, pick the serial port, then **Install** → upload the `.bin` file from step 1.
5. When it's done, click **Use device** → watch the live logs right in the browser.

![ESPHome Web Flasher](docs/images/esphome-web-flasher.png)
<!-- IMAGE: web.esphome.io page mid-upload, showing Connect / Install dialog. -->

> **Why this is nice:** zero toolchain on your local machine, works on Windows/macOS/Linux/Chromebook identically, and the web flasher is maintained by the ESPHome team directly.

#### Option B — Home Assistant ESPHome add-on (if your ESP is plugged into the HA host)

If you're running the ESPHome add-on inside Home Assistant *and* the ESP32 is physically plugged into your HA machine, you can flash it straight from the HA UI with no separate tools:

1. Open the **ESPHome dashboard** in HA (Settings → Add-ons → ESPHome → Open Web UI).
2. Click **New device** → **Continue** → paste your YAML, or create from template.
3. Click **Install** → **Plug into the computer running ESPHome Dashboard**.
4. Pick the serial device (e.g. `/dev/ttyUSB0`) and hit **Install**.

From the second flash onwards the dashboard switches automatically to OTA over the tailnet IP, so you don't need to keep the USB cable plugged in.

#### Option C — USB with the ESPHome CLI (what I use for development)

Classic, fully scriptable, and the fastest feedback loop if you're iterating on the YAML.

```bash
esphome run your-device.yaml --device /dev/ttyUSB0
```

On Windows the device usually comes up as `COM3`, `COM4`, etc. Use `--device COM3` accordingly.

#### What to look for on first boot

Whichever method you picked, once the firmware is running connect to the serial log (web flasher, HA dashboard logs, or `esphome logs your-device.yaml`) and you should see:

```
[I][tailscale]: Initializing Tailscale (MicroLink)...
[I][tailscale]: PSRAM detected: 8192 KB - using large buffers
[I][tailscale]: Waiting for WiFi before starting...
[I][wifi]: WiFi Connected!
[I][tailscale]: Calling microlink_init with auth_key=tskey-auth-... device=esp32-tailscale
[I][tailscale]: State: CONNECTING
[I][tailscale]: State: REGISTERING
[I][tailscale]: State: CONNECTED
[I][tailscale]: Connected! VPN IP: 100.xx.yy.zz
[I][tailscale]: Set wifi use_address: "100.xx.yy.zz" in your ESPHome YAML
```

<a id="wifi-use-address"></a>

### 4. Pin `use_address` to the Tailscale IP

After the first successful connection, **copy the `100.x.y.z` IP** from the log and add it to your YAML:

```yaml
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  use_address: "100.xx.yy.zz"   # ← your Tailscale IP
```

Re-flash once more (still USB if the device is in front of you; from here on OTA will work).

> **Why this matters:** ESPHome's API and OTA clients need a single address to talk to the device. By pointing `use_address` at the Tailscale `100.x` IP, both LAN-side and remote Home Assistant instances reach the device through the tailnet — no port forwarding, no mDNS trickery, and it survives the device moving between WiFi networks.

> [!TIP]
> **Deploying to a remote site?** A typical use case is building the device at home and then moving it somewhere else — a cabin, a parent's house, a workshop. Configure multiple WiFi networks so the ESP can connect at both locations:
>
> ```yaml
> wifi:
>   networks:
>     - ssid: !secret wifi_ssid_home
>       password: !secret wifi_password_home
>       priority: 2
>     - ssid: !secret wifi_ssid_remote
>       password: !secret wifi_password_remote
>       priority: 1
>   use_address: "100.xx.yy.zz"   # ← your Tailscale IP (same everywhere)
> ```
>
> The `use_address` is your Tailscale `100.x` IP — it **does not change** when the device moves between WiFi networks. Set it once, deploy anywhere. Home Assistant will reach the device through the tailnet regardless of which WiFi it's connected to.

### 5. Disable key expiry on the new node

By default Tailscale expires every node key after **180 days** (the tailnet-wide default; an admin can shorten it to anywhere between 1 and 180 days in **Settings → Device management → Device approval**). For an unattended ESP, you want the node key to be **permanent** — which is a per-device flag, not a tailnet setting.

1. Open the [Tailscale Machines page](https://login.tailscale.com/admin/machines).
2. Find the new `esp32-tailscale` entry.
3. Click the `⋯` menu → **Disable key expiry**.

![Tailscale Disable Key Expiry](docs/images/tailscale-disable-key-expiry.png)
<!-- IMAGE: Tailscale admin → Machines → row for the ESP32 → menu open, "Disable key expiry" highlighted. Also show the "Expires" column turning into "Disabled". -->

In Home Assistant you'll see the `VPN Node Key Expiry` timestamp sensor display as **Unknown** (because the timestamp no longer exists — that's the standard HA rendering for an empty `timestamp` sensor) and the `VPN Node Key Expiry Warning` binary sensor display as **OK** (the `problem` device class renders `off` as "OK" with a check icon). That `Unknown` + `OK` pair is the recommended steady state for an unattended node — nothing is missing, nothing is wrong, the key simply never expires.

| Disabled (recommended) | Enabled (needs attention) |
| :---: | :---: |
| ![Key expiry OK](docs/images/ha-key-expiry-ok.png) | ![Key expiry Problem](docs/images/ha-key-expiry-problem.png) |
| `Unknown` + `OK` — key never expires, device stays on the tailnet indefinitely. | `In 6 months` + `Problem` — the node key will expire and the device will drop off the tailnet unless you click **Disable key expiry**. |

### 6. Add to Home Assistant

Go to **Settings → Devices & Services → ESPHome → Add Device** and enter the Tailscale IP (`100.xx.yy.zz`) from step 4. HA will discover the device and offer to add all entities.

![Home Assistant ESPHome Integration Add](docs/images/ha-integration-add.png)
<!-- IMAGE: HA ESPHome integration "add device" dialog with the 100.x IP typed in. -->

![Home Assistant Device Page](docs/images/ha-device-page.png)
<!-- IMAGE: The HA device page showing all the Tailscale entities in the sensor / switch / button cards. -->

Done — the ESP is now a first-class citizen of your tailnet and your Home Assistant.

---

## Entity Reference

All entities are created automatically when you include the package.

### Binary sensors

| Entity | Description |
| --- | --- |
| **VPN Connected** | `on` when the Tailscale state machine reports `CONNECTED` (WireGuard tunnel is up and the control plane has handshaken). Device class: `connectivity`. |
| **VPN Node Key Expiry Warning** | `on` when the node's key expiry is **enabled** (Tailscale SaaS: HA shows this as "Problem" — click "Disable key expiry" in the admin). `off` when expiry is disabled or not applicable (HA shows "OK"). On Headscale, node keys do not expire by default so this stays OFF. |
| **HA API Connected** | `on` when at least one Home Assistant API client is connected. Device class: `connectivity`. |
| **VPN Auto-Rollback** | `on` when HA is connected via Tailscale and turning off the VPN switch would trigger the 60 s dead-man's-switch rollback. `off` when HA is on LAN — the switch can be toggled freely. |

### Text sensors (diagnostic)

| Entity | Description |
| --- | --- |
| **VPN IP** | The `100.x.y.z` address assigned to this node. Empty until connected. |
| **VPN Hostname** | The hostname this node registered with, e.g. `esp32-tailscale`. |
| **VPN MagicDNS** | The FQDN, e.g. `esp32-tailscale.tailXXXXX.ts.net` (Tailscale SaaS) or `esp32-tailscale.headscale.local` (Headscale with `magic_dns: true`). Unknown if MagicDNS is not configured on the control plane. |
| **VPN Network** | The tailnet domain, e.g. `tailXXXXX.ts.net` (Tailscale SaaS) or the Headscale `base_domain`. Unknown if not provided by the control plane. |
| **Device Memory** | Reports `PSRAM <size>KB` or `Internal RAM` so you can confirm PSRAM was detected. |
| **VPN Setup Hint** | Human-readable next-action hint, e.g. `wifi use_address: 100.x.y.z`. Shows auth-source-aware failure messages after 60 s, node key expiry deadline date when applicable, and clickable GitHub README links. |
| **VPN Peer Status** | `OK` / `Warning` / `Full` based on how close you are to the `max_peers` limit. |
| **VPN Node Key Expiry** | ISO-8601 timestamp of the node's key expiry (`device_class: timestamp`). Shows **Unknown** when key expiry is disabled (Tailscale SaaS: click "Disable key expiry") or not applicable (Headscale: no expiry by default). Unknown is the correct, healthy state. See the **VPN Node Key Expiry Warning** binary sensor for the simple on/off view. |
| **HA API Connection Route** | How the *currently-connected* HA instance is reaching the device: `Tailscale Direct`, `Tailscale DERP`, `Local`, or `Unknown`. Updates live. |
| **HA API Connection IP** | The IP address of the currently-connected HA API client. Deduplicated when multiple clients connect. |
| **VPN Control Plane** | Friendly name of the active control plane: `Tailscale` when using SaaS, `Headscale` when `login_server` points elsewhere. Static — set at boot. |
| **VPN Login Server** | The raw `login_server` YAML value (empty for Tailscale SaaS). Static — set at boot. |
| **VPN Auth Key Source** | Shows `Default (YAML)` when using the built-in auth key, or `Override (YYYY-MM-DD HH:MM)` with the timestamp when a runtime override is active. |

### Sensors

| Entity | Description |
| --- | --- |
| **VPN Peers Online** | How many peers in the tailnet are currently reachable. |
| **VPN Peers Direct** | How many of those are on a direct WireGuard path (NAT traversal succeeded). |
| **VPN Peers DERP** | How many are going through Tailscale's DERP relays. Ideally zero. |
| **VPN Peers Max** | Your configured `max_peers` value. |
| **VPN Uptime** | Seconds since the `CONNECTED` state was entered. Resets on reconnect. Publishes every 5 s during the first 5 minutes, then every 60 s. |
| **VPN Connect Count** | Lifetime count of successful VPN connections since boot. Does not reset on reconnect. |

### Text inputs

| Entity | Description |
| --- | --- |
| **VPN Auth Key Override** | Password-mode text input. Submit a new auth key (`tskey-auth-...` for Tailscale SaaS, `hskey-auth-...` for Headscale) to replace the YAML default at runtime — no reflash needed. The key is persisted in NVS across reboots. Submit an empty value to revert to the YAML default. See **VPN Auth Key Source** for which key is active. Max 128 characters. |

### Switches

| Entity | Description |
| --- | --- |
| **VPN Enabled** | Stops the Tailscale stack when turned off. See [Enable switch caveats](#enable-switch-caveats) below. |
| **VPN Debug Log** | Toggles verbose microlink/VPN logging (INFO ↔ WARN). State persists across reboots via NVS. Only affects VPN-related log tags — ESPHome's own logs are unchanged. |

> **Note:** The debug log switch controls the *runtime* log level of the VPN components. Your ESPHome `logger: level:` setting still acts as a global ceiling — if you set it to `ERROR`, VPN debug logs won't appear even with the switch ON. The default `DEBUG` or `INFO` level works fine.

### Buttons

| Entity | Description |
| --- | --- |
| **VPN Reconnect** | Triggers the three-phase reconnect state machine (rebind → full restart → reboot). Useful if you suspect the tunnel is wedged. |
| **Reboot** | Standard ESPHome restart button. |

---

## Configuration options

All options go under the `tailscale:` block:

```yaml
tailscale:
  auth_key: !secret tailscale_auth_key   # required
  hostname: "esp32-tailscale"            # optional, default empty → control plane auto-assigns
  max_peers: 16                          # optional, default 16, range 1–64
  login_server: ""                       # optional, empty → Tailscale SaaS; set for Headscale / self-hosted
```

| Option | Default | Description |
| --- | --- | --- |
| `auth_key` | *(required)* | Tailscale auth key (`tskey-auth-...`). Use `!secret`. |
| `hostname` | `""` | Name the node registers as. Empty → Tailscale picks one. |
| `max_peers` | `16` | Maximum number of peers to track. Raise if your tailnet has more than 16 nodes *and* you have PSRAM. |
| `login_server` | `""` | Custom control-plane host. Empty uses the official Tailscale SaaS coordinator. Set to a Headscale (or other Tailscale-compatible) coordinator to point the node elsewhere. Accepts a bare hostname, an IP, `host:port`, or a full `http://host[:port]` URL; `https://` is rejected. Authentication and initial registration work end-to-end against Headscale 0.23.0; see *Custom control plane (Headscale)* under Deployment Notes for the current caveats. Leave empty for Tailscale SaaS. |

> **No `update_interval`.** The component is fully event-driven: sensors publish only when the underlying state actually changes. There is no polling loop to tune — and nothing to reduce CPU/network cost by raising.

> **What about STUN / DISCO?** The Tailscale stack always runs **STUN** (to discover how your NAT maps outbound UDP) and **DISCO** (Tailscale's peer discovery / path-probing protocol) — they're essential for getting direct peer-to-peer connections. They can't be turned off in this component because microlink runs them unconditionally; they have no config knob.

---

## How it works

### The short version

The ESP32 runs [microlink](https://github.com/CamM2325/microlink), a C implementation of the Tailscale client protocol. microlink handles:

- control-plane registration (HTTPS to the Tailscale coordinator),
- WireGuard key exchange and tunnel setup,
- peer discovery (disco protocol),
- NAT traversal via STUN,
- fallback to DERP relays when direct paths fail,
- and lwIP integration so the ESP's network stack sees the `100.x` address as a normal interface.

This ESPHome component wraps microlink in a `PollingComponent`, feeds it your auth key, exposes the state it reports as Home Assistant entities, and integrates the lifecycle with the rest of ESPHome (WiFi wait, OTA hooks, reboot, etc.).

### Direct vs DERP

Tailscale tries to make every peer-to-peer connection a **direct** UDP path. When that fails (strict NAT, UDP-blocked networks, etc.) it falls back to **DERP**: relays operated by Tailscale that tunnel traffic for you. DERP is encrypted end-to-end — the relays only see ciphertext — but they add latency.

The `VPN Peers Direct` and `VPN Peers DERP` sensors tell you at a glance how your peers are connected. The `HA API Connection Route` sensor tells you specifically which path Home Assistant is using *right now*.

> **Note:** There is **no DERP switch**. microlink always needs DERP available as a fallback (it hardcodes this internally). Turning DERP off would only disrupt the tunnel without actually disabling DERP, so the switch was removed.

### Memory modes

On boot the component queries `esp_psram_get_size()` and reports one of two modes:

- **`PSRAM <size>KB`** — large buffers, full peer list support, up to 64 peers.
- **`Internal RAM`** — small buffers, ~30-peer effective limit, works but not recommended.

Check the `Device Memory` sensor after first boot to confirm.

### HA Connection Route

This is the clever bit. When an ESPHome API client connects, the component walks the lwIP TCP PCB list, finds the pcb that owns the API connection, extracts the remote IP, and classifies it:

- If the remote IP is in the Tailscale CGNAT range (`100.64.0.0/10`), it looks up the peer in the microlink peer table and reports `Tailscale Direct` or `Tailscale DERP` based on whether the peer has a direct path.
- Otherwise it reports `Local`.

This gives you a live view of "is HA Core actually talking to me over Tailscale, or is it taking a LAN shortcut?" — invaluable when you're debugging a "why is my dashboard slow?" moment.

---

## Home Assistant integration

Once the device is added via the ESPHome integration, all entities show up under the device page. Here are a few useful things you can build on top:

### Dashboard card

```yaml
type: entities
title: Tailscale ESP32
entities:
  - entity: binary_sensor.esp32_tailscale_vpn_connected
    name: Connected
  - entity: sensor.esp32_tailscale_vpn_uptime
    name: Uptime
  - entity: text_sensor.esp32_tailscale_vpn_ip
    name: VPN IP
  - entity: text_sensor.esp32_tailscale_vpn_magicdns
    name: MagicDNS
  - entity: text_sensor.esp32_tailscale_ha_api_connection_route
    name: HA Route
  - entity: sensor.esp32_tailscale_vpn_peers_online
    name: Peers online
  - entity: sensor.esp32_tailscale_vpn_peers_direct
    name: Peers direct
  - entity: sensor.esp32_tailscale_vpn_peers_derp
    name: Peers DERP
  - entity: binary_sensor.esp32_tailscale_vpn_node_key_expiry_warning
    name: Key expiry warning
  - entity: text_sensor.esp32_tailscale_vpn_node_key_expiry
    name: Key expires
  - type: buttons
    entities:
      - entity: button.esp32_tailscale_vpn_reconnect
        name: Reconnect
      - entity: button.esp32_tailscale_reboot
        name: Reboot
```

### Automation: warn if key expiry is still enabled

If you forget to disable key expiry on a new node, this automation checks every hour and warns you about **any** ESP device that still has key expiry enabled. Works automatically with multiple devices — no need to list entity IDs by hand.

```yaml
alias: Tailscale node key expiry check
trigger:
  - platform: time_pattern
    hours: "/1"
action:
  - variables:
      warnings: >
        {{ states.binary_sensor
           | selectattr('entity_id', 'search', '_vpn_node_key_expiry_warning$')
           | selectattr('state', 'eq', 'on')
           | map(attribute='entity_id')
           | list }}
  - condition: template
    value_template: "{{ warnings | length > 0 }}"
  - service: notify.mobile_app_phone
    data:
      title: "Tailscale key expiry still on"
      message: >
        {% for entity_id in warnings %}
        {% set device = device_attr(device_id(entity_id), 'name') %}
        {% set expiry_id = entity_id | replace('binary_sensor.', 'text_sensor.') | replace('_warning', '') %}
        - {{ device }}: expires {{ states(expiry_id) }}
        {% endfor %}
        Open the admin console and click "Disable key expiry" for each device.
```

---

## Deployment Notes

A collection of behaviors we've found easy to miss until they cause a surprise — and easier to get right on day one. These are not bugs; they are design consequences of how Tailscale and userspace WireGuard interact with a microcontroller endpoint.

### Subnet routers and the ESP node

If your network already has a Tailscale subnet router advertising the LAN CIDR that the ESP lives on, peers on the tailnet now see two paths to the device: one via the subnet router (arriving at the ESP's LAN address, NAT'd), and one direct (arriving at the ESP's own `100.x` tailnet address). The two paths are not equivalent, and Tailscale's route preference decides which one wins.

We recommend:

- Let the ESP join as its own tailnet node — that is the whole point of this component. The direct path is the one you want.
- Do **not** re-advertise the ESP's LAN CIDR through the subnet router purely to reach the device; it creates duplicate routing state and makes `HA API Connection Route` diagnostics ambiguous.
- Keep `accept_routes: false` on the ESP itself unless you specifically need another subnet. With `accept_routes: true`, the node honors advertised routes that can include its own LAN — a self-loop that never completes, and a source of flaky behavior that is hard to spot from the logs.

### Userspace WireGuard: what the node can and cannot do

This component uses microlink, a userspace WireGuard stack. There is no kernel TUN device and no netfilter hook, which constrains what the Tailscale node is allowed to do compared with a full Linux `tailscaled`:

| Feature                  | Supported | Notes |
| ------------------------ | --------- | ----- |
| Join tailnet as leaf     | Yes       | This is the node's role. |
| `accept_routes`          | Yes       | Off by default. |
| Custom `login_server`    | Yes       | Headscale 0.23.0 verified end-to-end: Noise handshake, `/machine/register`, and the streaming MapResponse long-poll all work. See *Custom control plane (Headscale)* below. |
| Advertise subnet routes  | No        | The ESP cannot be a subnet router. |
| Exit node (use or offer) | No        | Neither direction is implemented. |
| Netfilter / ACL rules    | No        | There is no OS-level firewall to hook into. |

If your architecture requires the ESP to route traffic on behalf of other devices, it won't. Place a Linux subnet router alongside it and keep the ESP as a pure endpoint — that is the supported topology.

### Custom control plane (Headscale)

> **TL;DR:** `login_server` supports Headscale end-to-end. The device completes the Noise handshake, registers, gets a tailnet IP, and stays online via the streaming MapResponse long-poll. Verified against Headscale 0.23.0.

By default the component registers against the official Tailscale SaaS coordinator at `controlplane.tailscale.com`. That is the recommended production path.

The `login_server` YAML option replaces the control-plane host microlink talks to, pointing the node at Headscale or any other Tailscale-compatible coordinator:

```yaml
tailscale:
  auth_key: "<preauth key from headscale>"
  hostname: "esp32-tailscale"
  login_server: "http://192.168.1.42:80"   # or bare IP / hostname / host:port
```

Accepted forms:

- Bare hostname or IP: `192.168.1.42`, `headscale.local`
- With explicit port: `192.168.1.42:8080`
- Full URL: `http://192.168.1.42`, `http://192.168.1.42:80`
- `https://…` is **rejected** — TLS is not implemented in this code path.

**What works end-to-end against Headscale 0.23.0:**

- YAML → `tailscale.cpp` setter → `microlink_config_t.ctrl_host` → `ml->ctrl_host`.
- `host:port` parsing and HTTP `Host:` / HTTP/2 `:authority` header construction.
- Noise server public key is fetched at setup time from the Tailscale-compatible `/key?v=88` HTTP endpoint and passed into `ml_noise_init`, so the IK handshake completes against a Headscale-generated keypair.
- `/machine/register` over HTTP/2 — the node receives its tailnet IP from Headscale and appears in `headscale nodes list` (typically `100.64.0.1` on a fresh install).
- The streaming `/machine/map` long-poll stays open and delivers delta `MapResponse` chunks in response to each periodic endpoint update, so the node stays "online" in Headscale between STUN re-probes.

**One more thing to watch for:**

- **Auth keys come from the coordinator that issues them.** A Tailscale SaaS `tskey-auth-...` will not work against Headscale, and vice versa. Generate the key on the same coordinator you will connect to. Headscale / Headplane generates keys with a `hskey-auth-...` prefix (older CLI versions may output raw hex).
- **Switching between Tailscale SaaS and Headscale** (or vice versa) requires both a new `login_server` value in the YAML and a matching auth key. The auth key can be changed at runtime via the **VPN Auth Key Override** entity in HA — no reflash needed for the key itself. However, changing `login_server` requires a YAML edit and reflash.

For the full reproduction — docker-compose, a minimal `config.yaml`, and the CLI commands to create a user and preauth key — see [`contrib/headscale-test/README.md`](contrib/headscale-test/README.md). That directory is intentionally not shipped via the `packages:` distribution.

### Auth key and node key expiry

Two settings in the admin panel (Tailscale Admin Console or Headplane UI) decide whether an unattended ESP stays healthy:

1. **Node key expiry.** On **Tailscale SaaS**, each machine's key expires after 180 days by default. When that happens the device drops off the tailnet until re-authentication. Disable key expiry per device right after first boot: *Machines → device → ⋯ → Disable key expiry*. The `VPN Node Key Expiry Warning` binary sensor will flip off once it is done. On **Headscale**, node keys do not expire by default (the server sends Go's zero time as expiry). No action needed — the `VPN Node Key Expiry` sensor shows Unknown and the warning stays OFF, which is the correct steady state.
2. **Auth key flags.** When generating the key used in `secrets.yaml`, we recommend:
   - **Pre-authorized**, so the node joins without manual approval.
   - **Non-ephemeral**, so the machine entry persists across reboots. Ephemeral nodes are wiped while offline and reappear as new entries every boot, quickly cluttering the admin panel.
   - **Reusable** if the same firmware is flashed to multiple devices; single-use for tighter provisioning control.
   - **Long expiry** on the key itself. The auth key is only consumed at first boot, but a short expiry window can bite if a replacement device sits on a shelf for a few months before deployment.

### First boot vs subsequent boots

At first boot the component reads the auth key, performs the tailnet join, and persists the resulting node key to NVS. Every subsequent boot reuses that node key, which is why the same device appears as the same machine across power cycles.

Three scenarios cause the device to appear as a new machine on the tailnet:

- **`esptool erase_flash`** wipes NVS and forces a fresh join. The old machine entry in the admin panel becomes orphaned and should be deleted manually.
- **Changing `hostname`** in the YAML. From Tailscale's perspective this is a new node; the old entry lingers with the old name until deleted.
- **Deleting the device in the admin console** (Tailscale Admin or Headplane UI / `headscale nodes delete`). If the auth key is **reusable and still valid**, the device will automatically re-register with the same IP — no action needed. If the auth key is **single-use or expired**, the device will fail to connect ("Connection failed" in Setup Hint). To fix this, either enter a new auth key via the **VPN Auth Key Override** entity in HA, or erase NVS (`esptool erase_flash` or ESPHome's "Clean Build Files" + reflash) and provide a fresh auth key in the YAML.

Clean up orphans when renaming or reflashing. It is easy to accumulate ghost machines otherwise.

### Direct connections versus DERP relays

Tailscale prefers direct peer-to-peer UDP connections and falls back to DERP relays when NAT traversal fails. The fallback is transparent but has measurable cost:

- DERP paths add roughly 50–150 ms of latency depending on the nearest relay.
- DERP bandwidth is subject to Tailscale's fair-use caps.
- Sustained high-frequency telemetry over DERP works, but is noticeably slower than direct.

What usually prevents a direct connection:

- **CGNAT or symmetric NAT** on either end — common on mobile carriers and some residential ISPs — makes UDP hole punching fail. The node still works; it will simply always relay.
- **Outbound UDP 41641** must be reachable for the initial handshake. Most consumer networks allow this; some restricted corporate networks do not.
- **Hairpin NAT** on the local router affects whether a peer on the same LAN can reach the ESP via its tailnet IP while both are local.

How to tell which mode is in use: the `HA API Connection Route` text sensor reports `Tailscale Direct` or `Tailscale DERP` for the current HA session, and the serial log emits the same classification per peer.

### Hardware realities

Development and automated validation happen on ESP32-S3 with PSRAM. Other variants are documented as possible by the underlying libraries, but are not currently verified here.

- **PSRAM is strongly recommended.** The WireGuard crypto buffers, microlink's internal state, and the ESPHome runtime together exceed what stock ESP32 internal RAM has to spare for a stable margin. Without PSRAM, `max_peers` above ~30 is not realistic.
- **CPU caps WireGuard throughput at roughly 2–5 Mbit/s** on ESP32-S3. This is ample for sensor telemetry and MQTT payloads, but too slow for image or video streaming through the tailnet.
- **Flash 4 MB minimum, 8 MB or more recommended.** OTA requires enough free flash to hold a second firmware image alongside the running one.

If a particular variant matters to you, running `esphome compile` on the example config is the fastest way to check whether it fits: a failed build names the constraint that was hit.

### ESPHome package cache

When loading the component via `packages:` from a GitHub source, ESPHome caches the fetched content for 24 hours by default. During active development against `main`, this can hide newly pushed changes for a full day.

Three ways around it:

- Add `refresh: always` to the `packages:` block (equivalent to `refresh: 0s`), which bypasses the 24-hour cache and refetches on every compile.
- Click **Clean All Files** in the ESPHome Dashboard top bar — this wipes cached build artifacts and external packages for every configuration, straight from the web UI.
- Delete `.esphome/external_components/` and `.esphome/packages/` manually between compiles.

End users who pin to a released tag or a specific commit do not need any of these workarounds — it only matters when tracking `main` actively.

---

## Troubleshooting

### The device won't connect at all

Check the serial log for the state machine output. You should cycle through `IDLE → WIFI_WAIT → CONNECTING → REGISTERING → CONNECTED`.

| Stuck at | Likely cause | Fix |
| --- | --- | --- |
| `IDLE` | microlink never started | WiFi not connected — check WiFi logs |
| `WIFI_WAIT` | WiFi still joining | Wait or check SSID/password |
| `CONNECTING` | Can't reach control plane | Check DNS and internet connectivity on the LAN |
| `REGISTERING` | Control plane rejected the auth key | Key expired, used on too many devices, or the tailnet has device approval on — check the Tailscale admin (or Headplane UI / `headscale` CLI for Headscale) |
| `ERROR` | microlink crash | See serial log for details; try `VPN Reconnect` button or reboot |

### Auth key expired

Symptom: the log shows `State: ERROR` / `REGISTERING` failing after a fresh flash, the `VPN Connected` binary sensor never turns on, and the admin console (Tailscale or Headplane) shows no new machine. This usually means the pre-authentication key you baked into the firmware has expired or been revoked.

1. Generate a new auth key (Tailscale: [Quick Start step 1](#1-create-a-tailscale-auth-key); Headscale: Headplane UI → "Create pre-auth key" or `headscale preauthkeys create --user <user>`).
2. **Option A — no reflash:** paste the new key into the **VPN Auth Key Override** text entity in HA (Settings → Devices → your ESP → Controls). The device reconnects immediately with the new key, which is persisted in NVS across reboots. The **VPN Auth Key Source** sensor will show `Override (YYYY-MM-DD HH:MM)`.
3. **Option B — reflash:** update `secrets.yaml` and re-flash (OTA if reachable, otherwise USB).
4. **Disable key expiry on the new node** right away so it doesn't happen again. The `VPN Node Key Expiry Warning` binary sensor will flip to `off` once you do.

> **Note:** The `VPN Node Key Expiry` sensor reflects the *node* key expiry (received from the control plane), not the auth key used to register the device. Auth key expiry is never sent to the device, so it can't be monitored from HA. On Headscale, node keys do not expire by default — the sensor shows Unknown and the warning stays OFF, which is normal.

### HA can't reach the device after OTA

Symptom: you pushed an OTA update, the device rebooted, and now HA shows it as unavailable.

Almost always this means `use_address` isn't pinned to the Tailscale IP. ESPHome used LAN mDNS to find the device during OTA, but Home Assistant is configured to reach it at a different address.

**Fix:** set `wifi: use_address: "100.x.y.z"` in the YAML, re-flash once over USB (or if the device comes back briefly after reboot, over OTA), and from then on every connection goes through the tailnet.

### "Peer limit FULL" warnings in the log

Symptom: log lines like `Peer limit FULL: 16/16 online peers. Increase max_peers or remove unused peers from your tailnet.`

Increase `max_peers`:

```yaml
tailscale:
  auth_key: !secret tailscale_auth_key
  max_peers: 32  # or 48, or 64
```

You need PSRAM for anything above ~30.

### `HA API Connection Route` shows `Tailscale (unknown)`

This means HA is connecting from a `100.x` address, but the ESP doesn't have that peer in its peer table yet. Usually a transient state right after startup — wait for the next peer callback and it'll resolve to `Tailscale Direct` or `Tailscale DERP`.

### `HA API Connection Route` shows `Local` when you expect Tailscale

This means Home Assistant (or the ESPHome Builder, etc.) is reaching the device via its LAN IP, not the Tailscale IP. Either:

- `use_address` is still set to a LAN address — fix as above
- HA has cached a LAN address discovered via mDNS — restart the ESPHome integration or clear HA's zeroconf cache

### Builder UI can't find the device over Tailscale

Home Assistant's ESPHome add-on builder uses zeroconf / mDNS, which doesn't cross the tailnet boundary cleanly. Workaround: in the add-on UI add the device manually by its `100.x` IP, or run the ESPHome CLI from a machine that's on your tailnet.

---

## Enable switch caveats

The `VPN Enabled` switch really does stop the microlink stack when turned off — but there's one gotcha:

**If Home Assistant is reaching the ESP *only* through Tailscale** (no LAN path), then turning the switch off will kill HA's own connection to the device. The component has a 60-second dead-man's-switch safety: if HA doesn't re-establish a connection within 60 seconds, the switch rolls back to its previous state automatically.

In practice this means:
- **On LAN:** the switch works as expected. Turn it off, Tailscale goes away, HA still reaches the device over LAN. Stays off.
- **Tailscale-only:** you *can* turn it off but it'll snap back after a minute. If you really want it off in this scenario, you need to reboot the device or re-enable from LAN.

The switch state also **does not persist across reboots** — the device always boots with Tailscale enabled. This is a deliberate safety choice so a bad toggle can't leave the device unreachable forever.

---

## Development

If you want to modify the component:

```bash
git clone https://github.com/Csontikka/esphome-tailscale.git
cd esphome-tailscale
```

The `microlink/` directory is vendored directly into the repository, so no `git submodule` step is needed.

The repository ships two example configurations at the root:

| File | Purpose |
|------|---------|
| [`example.yaml`](example.yaml) | End-user style. Pulls the component and entities from the GitHub `packages:` source — exactly what someone installing this would write. |
| [`example-dev.yaml`](example-dev.yaml) | Development style. Uses `external_components: - source: components` so every compile picks up the working-tree copy of the code, with the entity definitions inlined. This is the file to use when editing `components/tailscale/`. |

Build and flash the dev example:

```bash
esphome run example-dev.yaml --device COM3
```

The `components/tailscale/` directory is the external component proper; `microlink/` is a vendored copy of [CamM2325/microlink](https://github.com/CamM2325/microlink) (via the [Csontikka/microlink](https://github.com/Csontikka/microlink) fork) which provides the Tailscale protocol stack. It is checked in directly rather than pulled as a submodule, so a fresh clone is ready to build with no extra steps.

### File layout

```
esphome-tailscale/
├── components/tailscale/      # The ESPHome external component
│   ├── __init__.py            # Config schema + codegen (C++ setters, build flags, CMake patch)
│   ├── binary_sensor.py       # Binary sensor platform
│   ├── sensor.py              # Sensor platform
│   ├── text_sensor.py         # Text sensor platform
│   ├── switch.py              # Switch platform
│   ├── text.py                # Text input platform (auth key override)
│   ├── button.py              # Button platform
│   ├── tailscale.h            # C++ component header
│   └── tailscale.cpp          # C++ component implementation
├── packages/tailscale/
│   └── tailscale.yaml         # The end-user package (external_components + all entities)
├── docs/
│   └── images/                # Screenshots and diagrams used in the README
├── .github/
│   ├── workflows/
│   │   ├── validate.yml               # ESPHome config validation CI
│   │   ├── check-microlink-update.yml # Alerts when the vendored microlink has a new upstream release
│   │   └── codeql.yml                 # GitHub CodeQL static analysis
│   └── dependabot.yml                 # Automated dependency update config
├── scripts/
│   ├── capture_web_esphome.py  # Screenshot capture helper for ESPHome web UI
│   ├── mask_screenshots.py     # Redacts sensitive info from screenshots
│   └── svg_to_png.py           # Converts SVG diagrams to PNG for the docs
├── contrib/
│   └── headscale-test/         # Local Headscale docker-compose harness for login_server development
├── microlink/                 # Vendored copy of the Tailscale protocol implementation
├── example.yaml               # End-user reference config that uses the GitHub package
├── example-dev.yaml           # Development config using the local checkout and inline entities
├── CHANGELOG.md               # Release notes and breaking-change log (Keep a Changelog format)
├── SECURITY.md                # Security policy and vulnerability reporting
├── LICENSE                    # MIT License
├── .gitignore                 # Git ignore rules
└── README.md                  # This file
```

---

## FAQ

**Q: Do I need a subnet router?**
No. That's the whole point. The ESP is its own Tailscale node.

**Q: Do I need Tailscale Funnel?**
No. Funnel publishes services to the public internet — that's unrelated. Everything here is private, tailnet-only.

**Q: Can I use this on a plain ESP32 (not S3)?**
Active testing happens on ESP32-S3 with PSRAM — that's the only chip this project currently verifies. The underlying microlink library claims support for other ESP32 variants, but they are **not yet verified here**. PSRAM is strongly recommended regardless, because the Tailscale stack is too heavy for stock ESP32 RAM alone.

**Q: Will this work over cellular / LTE / a hotspot?**
Yes, as long as outbound UDP and HTTPS are allowed. DERP (TCP 443) is used as a fallback so even heavily-firewalled networks usually work.

**Q: How much flash does it use?**
The compiled firmware (including the full Tailscale stack) is around **1 MB**. A stock **4 MB** flash chip is plenty — it holds the bootloader, two OTA slots for that ~1 MB image, and still has room left for a small SPIFFS/LittleFS partition. 8 MB / 16 MB boards only matter if you plan to stack other large components next to Tailscale.

**Q: Can I run multiple ESPs on the same auth key?**
Yes, if the auth key is marked **Reusable** in the admin console (Tailscale Admin or Headplane UI). Each ESP will get its own `100.x` address.

**Q: My tailnet has ACLs. Do I need to grant the ESP access to HA?**
Yes, Home Assistant needs to be allowed to reach the ESP's API port (default `6053`). If you use tags, tag the ESP when generating the auth key (e.g. `tag:esphome`) and write an ACL rule allowing your HA host (or your whole tailnet) to reach `tag:esphome:*`.

**Q: Can I use this with Headscale (self-hosted)?**
Yes. Set `login_server` to your Headscale URL and use a preauth key from the Headplane UI or `headscale preauthkeys create`. See [Custom control plane (Headscale)](#custom-control-plane-headscale) for details. Verified end-to-end against Headscale 0.23.0.

**Q: Do I need to disable key expiry on Headscale too?**
No. Headscale does not expire node keys by default — the **VPN Node Key Expiry Warning** sensor stays OFF and the **VPN Node Key Expiry** sensor shows Unknown, which is the correct, healthy state. You only need to worry about key expiry on Tailscale SaaS.

**Q: Can I switch between Tailscale SaaS and Headscale?**
Yes, but it requires a YAML edit (`login_server`) and a reflash. The auth key can be changed at runtime via the **VPN Auth Key Override** entity — no reflash needed for the key itself. The old machine entry on the previous control plane becomes orphaned and should be deleted.

**Q: Can I ping the ESP from another tailnet node?**
Yes. Once connected, it responds to ICMP on its `100.x` address like any other Tailscale node. Great for sanity checks.

---

## Credits

This component is **just the glue** between ESPHome and a third-party Tailscale protocol stack. All the hard work on the wire protocol, WireGuard crypto, disco/STUN, and DERP is done by upstream projects:

- **[microlink](https://github.com/CamM2325/microlink)** by **Cameron Malone** ([@CamM2325](https://github.com/CamM2325)) — MIT-licensed, clean-room implementation of the Tailscale protocol for embedded devices. This is the library that actually speaks Tailscale. Included here as a vendored copy under `microlink/`.
- **[WireGuard](https://www.wireguard.com/)** — the underlying VPN protocol, designed by **Jason A. Donenfeld**. "WireGuard" is a registered trademark of Jason A. Donenfeld.
- **X25519** — elliptic curve code derived from public-domain work by **Daniel J. Bernstein**.
- **[ESPHome](https://esphome.io/)** — the framework this plugs into.
- **[Tailscale](https://tailscale.com/)** — the mesh network we're joining as a peer.

### Trademark & non-affiliation notice

> **This project is not affiliated with, sponsored by, or endorsed by Tailscale Inc., Jason A. Donenfeld, or the WireGuard project.**
>
> "Tailscale" is a trademark of Tailscale Inc. "WireGuard" is a registered trademark of Jason A. Donenfeld. Both names are used here only to describe interoperability with the respective services and protocols. No Tailscale source code is included, copied, or redistributed in this repository — the protocol layer is provided by the separate, independently-maintained microlink library.

---

## License

This project (the ESPHome wrapper — everything under `components/`, `packages/`, `example.yaml`, `README.md`, etc.) is released under the **MIT License**. See [`LICENSE`](LICENSE) for the full text and the list of third-party notices.

Bundled / required components keep their own licenses:

| Component | License | Copyright |
|-----------|---------|-----------|
| [microlink](https://github.com/CamM2325/microlink) (vendored) | MIT | © 2025-2026 Cameron Malone |
| WireGuard protocol impl (inside microlink) | MIT (based on public spec) | WireGuard™ — Jason A. Donenfeld |
| X25519 (inside microlink) | Public domain | Daniel J. Bernstein |

See [`microlink/LICENSE`](microlink/LICENSE) and [`microlink/x25519-license.txt`](microlink/x25519-license.txt) for the full upstream texts.

**DISCLAIMER:** This is an independent community effort for educational and interoperability purposes. The authors make no guarantees about security, correctness, stability, or compatibility with official Tailscale software. **Use at your own risk.**

---

## Support

Found a bug or have an idea? [Open an issue](https://github.com/Csontikka/esphome-tailscale/issues) — feedback and feature requests are welcome!

If you find this component useful, consider [buying me a coffee](https://buymeacoffee.com/csontikka) ☕
