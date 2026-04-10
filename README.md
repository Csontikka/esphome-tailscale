# ESPHome Tailscale

> Native Tailscale VPN on ESP32-S3 as a plug-and-play ESPHome external component.
> Your ESP joins your tailnet as a real Tailscale node — no subnet router, no reverse proxy, no middleman.

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
<!-- IMAGE: Simple architecture diagram. Left: Home (HA, phone) inside a "Tailnet" box with a cloud icon. Right: Remote site (ESP32-S3 with the tailscale component) also inside the Tailnet box. Arrow "direct WireGuard (no subnet router)". Below: Tailscale control plane cloud. -->

---

## Features

- **Pure Tailscale node** — the ESP joins your tailnet directly, no subnet router needed on either side.
- **Works with official Tailscale & Headscale** — configurable `login_server`.
- **Home Assistant native** — exposes 20+ entities out of the box: connection status, VPN IP, hostname, peer counts, auth key expiry, uptime, MagicDNS name, peer list, memory mode, HA connection route, reboot/reconnect buttons, enable switch.
- **HA Connection Route sensor** — tells you *how* HA is currently reaching the device: `Tailscale Direct`, `Tailscale DERP`, or `Local`. Great for debugging connectivity.
- **Auth key expiry sensor** — surfaces the real expiry timestamp from the control plane, so you can set up a HA automation that warns you before your key dies.
- **PSRAM-aware** — auto-detects PSRAM and scales internal buffers (supports large tailnets with 50+ peers).
- **Self-healing reconnect** — three-phase recovery (rebind → full restart → reboot) when the tailnet link goes stale.
- **Auto `use_address` hint** — tells you exactly which line to add to your YAML so HA finds the device over Tailscale after first boot.
- **Package-based install** — one `packages:` line in your YAML and all entities appear.

---

## Requirements

### Hardware

- **ESP32-S3** with **PSRAM** (recommended: 8 MB Octal PSRAM).
- **16 MB flash** recommended (8 MB can work but leaves little OTA headroom).
- Examples that are known to work:
  - ESP32-S3-DevKitC-1 (8 MB PSRAM, 16 MB flash) — the reference board
  - ESP32-S3-N16R8

> **Why ESP32-S3 / PSRAM?** The Tailscale control protocol and WireGuard crypto state together need more RAM than a plain ESP32 has. Without PSRAM the component falls back to small buffers and caps around 30 peers — fine for small tailnets, rough for larger ones.

### Software

- **ESPHome 2026.3.1** or newer
- **ESP-IDF framework** (not Arduino) — this is enforced automatically by the package
- **Home Assistant** with the ESPHome integration enabled
- A **Tailscale account** (or a Headscale instance)

---

## Quick Start

### 1. Create a Tailscale auth key

Log in to the [Tailscale admin console](https://login.tailscale.com/admin/settings/keys) and create a new auth key.

![Tailscale Auth Key Creation](docs/images/tailscale-auth-key-create.png)
<!-- IMAGE: Tailscale admin console → Settings → Keys → Generate auth key... dialog. Show the checkboxes: Reusable, Ephemeral OFF, Pre-approved, Tags (optional). -->

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
  tailscale: github://Csontikka/esphome-tailscale/packages/tailscale/tailscale.yaml@main

tailscale:
  auth_key: !secret tailscale_auth_key
  hostname: "esp32-tailscale"

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

logger:
  level: DEBUG

api:

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

![HA ESPHome Add-on Install](docs/images/ha-addon-install.png)
<!-- IMAGE: HA ESPHome add-on "Install" dialog showing the three flash target options. -->

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

![Serial Log First Boot](docs/images/serial-log-first-boot.png)
<!-- IMAGE: A screenshot or nicely-formatted terminal screen showing the first-boot sequence above. -->

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

### 5. Disable key expiry on the new node

By default Tailscale expires every node key after 90 days (or whatever cap your tailnet has). For an unattended ESP, you want the node key to be **permanent**.

1. Open the [Tailscale Machines page](https://login.tailscale.com/admin/machines).
2. Find the new `esp32-tailscale` entry.
3. Click the `⋯` menu → **Disable key expiry**.

![Tailscale Disable Key Expiry](docs/images/tailscale-disable-key-expiry.png)
<!-- IMAGE: Tailscale admin → Machines → row for the ESP32 → menu open, "Disable key expiry" highlighted. Also show the "Expires" column turning into "Disabled". -->

The `Tailscale Auth Key Expiry` sensor on the device will flip to an empty/unknown value, and the `Tailscale Auth Key Status` sensor will report `OK` indefinitely.

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
| **Tailscale Connected** | `on` when the Tailscale state machine reports `CONNECTED` (WireGuard tunnel is up and the control plane has handshaken). |

### Text sensors (diagnostic)

| Entity | Description |
| --- | --- |
| **Tailscale IP** | The `100.x.y.z` address assigned to this node. Empty until connected. |
| **Tailscale Hostname** | The hostname this node registered with, e.g. `esp32-tailscale`. |
| **Tailscale MagicDNS** | The FQDN, e.g. `esp32-tailscale.tailXXXXX.ts.net`. |
| **Tailscale Tailnet** | Just the tailnet domain portion, e.g. `tailXXXXX.ts.net`. |
| **Tailscale Memory** | Reports `PSRAM <size>KB` or `Internal RAM` so you can confirm PSRAM was detected. |
| **Tailscale Setup Hint** | Human-readable next-action hint, e.g. `wifi use_address: 100.x.y.z`. Use this in a HA automation to remind yourself after first flash. |
| **Tailscale Peer Status** | `OK` / `Warning` / `Full` based on how close you are to the `max_peers` limit. |
| **Tailscale Auth Key Status** | `OK`, `Warning` (<7 days to expiry), `Expired`, or `Unknown`. |
| **Tailscale Auth Key Expiry** | ISO-8601 timestamp of the actual key expiry (device_class: `timestamp`). Empty once you disable key expiry on the node. |
| **HA Connection Route** | How the *currently-connected* HA instance is reaching the device: `Tailscale Direct`, `Tailscale DERP`, `Local`, or `Unknown`. Updates live. |

### Sensors

| Entity | Description |
| --- | --- |
| **Tailscale Peers Online** | How many peers in the tailnet are currently reachable. |
| **Tailscale Peers Direct** | How many of those are on a direct WireGuard path (NAT traversal succeeded). |
| **Tailscale Peers DERP** | How many are going through Tailscale's DERP relays. Ideally zero. |
| **Tailscale Peers Max** | Your configured `max_peers` value. |
| **Tailscale Uptime** | Seconds since the `CONNECTED` state was entered. Resets on reconnect. |

### Switches

| Entity | Description |
| --- | --- |
| **Tailscale Enabled** | Stops the Tailscale stack when turned off. See [Enable switch caveats](#enable-switch-caveats) below. |

### Buttons

| Entity | Description |
| --- | --- |
| **Tailscale Reconnect** | Triggers the three-phase reconnect state machine (rebind → full restart → reboot). Useful if you suspect the tunnel is wedged. |
| **Reboot** | Standard ESPHome restart button. |

---

## Configuration options

All options go under the `tailscale:` block:

```yaml
tailscale:
  auth_key: !secret tailscale_auth_key   # required
  hostname: "esp32-tailscale"            # optional, default empty → control plane auto-assigns
  enable_stun: true                      # optional, default true
  enable_disco: true                     # optional, default true
  max_peers: 16                          # optional, default 16, range 1–64
  login_server: "https://headscale.example.com"   # optional, for Headscale users
  update_interval: 30s                   # optional, how often to refresh sensor state
```

| Option | Default | Description |
| --- | --- | --- |
| `auth_key` | *(required)* | Tailscale auth key (`tskey-auth-...`). Use `!secret`. |
| `hostname` | `""` | Name the node registers as. Empty → Tailscale picks one. |
| `enable_stun` | `true` | Enable STUN for NAT traversal. Turn off only if your network blocks STUN and you're OK relying on DERP. |
| `enable_disco` | `true` | Enable the Tailscale "disco" peer discovery protocol. Leave on unless you know what you're doing. |
| `max_peers` | `16` | Maximum number of peers to track. Raise if your tailnet has more than 16 nodes *and* you have PSRAM. |
| `login_server` | `""` | Headscale URL. Leave empty for official Tailscale control plane. |
| `update_interval` | `30s` | How often sensor states are re-published. Does **not** affect the tunnel itself. |

---

## How it works

### The short version

The ESP32 runs [microlink](https://github.com/CamM2325/microlink), a C implementation of the Tailscale client protocol. microlink handles:

- control-plane registration (HTTPS to the Tailscale coordinator or your Headscale),
- WireGuard key exchange and tunnel setup,
- peer discovery (disco protocol),
- NAT traversal via STUN,
- fallback to DERP relays when direct paths fail,
- and lwIP integration so the ESP's network stack sees the `100.x` address as a normal interface.

This ESPHome component wraps microlink in a `PollingComponent`, feeds it your auth key, exposes the state it reports as Home Assistant entities, and integrates the lifecycle with the rest of ESPHome (WiFi wait, OTA hooks, reboot, etc.).

![How It Works Flow](docs/images/how-it-works.png)
<!-- IMAGE: Sequence-style diagram: WiFi connect → microlink_init → HTTPS to control plane → node registration → WireGuard handshake with DERP → disco probes → direct paths where possible → CONNECTED state. Highlight that Tailscale's control plane never sees traffic, only metadata. -->

### Direct vs DERP

Tailscale tries to make every peer-to-peer connection a **direct** UDP path. When that fails (strict NAT, UDP-blocked networks, etc.) it falls back to **DERP**: relays operated by Tailscale that tunnel traffic for you. DERP is encrypted end-to-end — the relays only see ciphertext — but they add latency.

The `Tailscale Peers Direct` and `Tailscale Peers DERP` sensors tell you at a glance how your peers are connected. The `HA Connection Route` sensor tells you specifically which path Home Assistant is using *right now*.

> **Note:** There is **no DERP switch**. microlink always needs DERP available as a fallback (it hardcodes this internally). Turning DERP off would only disrupt the tunnel without actually disabling DERP, so the switch was removed.

### Memory modes

On boot the component queries `esp_psram_get_size()` and reports one of two modes:

- **`PSRAM <size>KB`** — large buffers, full peer list support, up to 64 peers.
- **`Internal RAM`** — small buffers, ~30-peer effective limit, works but not recommended.

Check the `Tailscale Memory` sensor after first boot to confirm.

### HA Connection Route

This is the clever bit. When an ESPHome API client connects, the component walks the lwIP TCP PCB list, finds the pcb that owns the API connection, extracts the remote IP, and classifies it:

- If the remote IP is in the Tailscale CGNAT range (`100.64.0.0/10`), it looks up the peer in the microlink peer table and reports `Tailscale Direct` or `Tailscale DERP` based on whether the peer has a direct path.
- Otherwise it reports `Local`.

This gives you a live view of "is HA Core actually talking to me over Tailscale, or is it taking a LAN shortcut?" — invaluable when you're debugging a "why is my dashboard slow?" moment.

---

## Home Assistant integration

Once the device is added via the ESPHome integration, all entities show up under the device page. Here are a few useful things you can build on top:

### Dashboard card

![Home Assistant Dashboard Card](docs/images/ha-dashboard-card.png)
<!-- IMAGE: Finished Home Assistant dashboard card showing connected state, IP, peers direct/DERP counts, route, uptime, reboot/reconnect buttons, enable switch. -->

```yaml
type: entities
title: Tailscale ESP32
entities:
  - entity: binary_sensor.esp32_tailscale_tailscale_connected
    name: Connected
  - entity: sensor.esp32_tailscale_tailscale_uptime
    name: Uptime
  - entity: text_sensor.esp32_tailscale_tailscale_ip
    name: Tailscale IP
  - entity: text_sensor.esp32_tailscale_tailscale_magicdns
    name: MagicDNS
  - entity: text_sensor.esp32_tailscale_ha_connection_route
    name: HA Route
  - entity: sensor.esp32_tailscale_tailscale_peers_online
    name: Peers online
  - entity: sensor.esp32_tailscale_tailscale_peers_direct
    name: Peers direct
  - entity: sensor.esp32_tailscale_tailscale_peers_derp
    name: Peers DERP
  - entity: text_sensor.esp32_tailscale_tailscale_auth_key_status
    name: Auth key status
  - entity: sensor.esp32_tailscale_tailscale_auth_key_expiry
    name: Key expires
  - type: buttons
    entities:
      - entity: button.esp32_tailscale_tailscale_reconnect
        name: Reconnect
      - entity: button.esp32_tailscale_reboot
        name: Reboot
```

### Automation: warn before the auth key expires

If you forget to disable key expiry, this automation gives you a week's warning.

```yaml
alias: Tailscale auth key expiring soon
trigger:
  - platform: state
    entity_id: text_sensor.esp32_tailscale_tailscale_auth_key_status
    to: "Warning"
action:
  - service: notify.mobile_app_phone
    data:
      title: "Tailscale key expiring"
      message: >
        The Tailscale auth key on {{ states.text_sensor.esp32_tailscale_tailscale_auth_key_status.attributes.friendly_name }}
        expires soon. Open the Tailscale admin console and disable key expiry.
```

### Automation: alert on disconnect

```yaml
alias: Tailscale disconnect
trigger:
  - platform: state
    entity_id: binary_sensor.esp32_tailscale_tailscale_connected
    to: "off"
    for: "00:05:00"
action:
  - service: notify.mobile_app_phone
    data:
      title: "ESP offline"
      message: "ESP32 has been disconnected from Tailscale for 5 minutes."
```

---

## Troubleshooting

### The device won't connect at all

Check the serial log for the state machine output. You should cycle through `IDLE → WIFI_WAIT → CONNECTING → REGISTERING → CONNECTED`.

| Stuck at | Likely cause | Fix |
| --- | --- | --- |
| `IDLE` | microlink never started | WiFi not connected — check WiFi logs |
| `WIFI_WAIT` | WiFi still joining | Wait or check SSID/password |
| `CONNECTING` | Can't reach control plane | Check DNS and internet connectivity on the LAN |
| `REGISTERING` | Control plane rejected the auth key | Key expired, used on too many devices, or the tailnet has device approval on — check the Tailscale admin |
| `ERROR` | microlink crash | See serial log for details; try `Tailscale Reconnect` button or reboot |

### Auth key expired

Symptom: `Tailscale Auth Key Status` sensor shows `Expired`, or the log shows `State: ERROR` after a fresh flash.

1. Generate a new auth key (see [Quick Start step 1](#1-create-a-tailscale-auth-key)).
2. Update `secrets.yaml`.
3. Re-flash (OTA is fine if the device is still reachable; otherwise USB).
4. **Disable key expiry on the new node** right away so it doesn't happen again.

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

### `HA Connection Route` shows `Tailscale (unknown)`

This means HA is connecting from a `100.x` address, but the ESP doesn't have that peer in its peer table yet. Usually a transient state right after startup — wait for the next peer callback and it'll resolve to `Tailscale Direct` or `Tailscale DERP`.

### `HA Connection Route` shows `Local` when you expect Tailscale

This means Home Assistant (or the ESPHome Builder, etc.) is reaching the device via its LAN IP, not the Tailscale IP. Either:

- `use_address` is still set to a LAN address — fix as above
- HA has cached a LAN address discovered via mDNS — restart the ESPHome integration or clear HA's zeroconf cache

### Builder UI can't find the device over Tailscale

Home Assistant's ESPHome add-on builder uses zeroconf / mDNS, which doesn't cross the tailnet boundary cleanly. Workaround: in the add-on UI add the device manually by its `100.x` IP, or run the ESPHome CLI from a machine that's on your tailnet.

---

## Enable switch caveats

The `Tailscale Enabled` switch really does stop the microlink stack when turned off — but there's one gotcha:

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
git submodule update --init --recursive
```

Then point an ESPHome YAML at your local checkout instead of the package:

```yaml
external_components:
  - source: components
```

(This is what `example.yaml` in the repo does.)

Build and flash:

```bash
esphome run example.yaml --device COM3
```

The `components/tailscale/` directory is the external component proper; `microlink/` is a git submodule pointing at [CamM2325/microlink](https://github.com/CamM2325/microlink) which provides the Tailscale protocol stack.

### File layout

```
esphome-tailscale/
├── components/tailscale/      # The ESPHome external component
│   ├── __init__.py            # Config schema + codegen (C++ setters, build flags, CMake patch)
│   ├── binary_sensor.py       # Binary sensor platform
│   ├── sensor.py              # Sensor platform
│   ├── text_sensor.py         # Text sensor platform
│   ├── switch.py              # Switch platform
│   ├── button.py              # Button platform
│   ├── tailscale.h            # C++ component header
│   └── tailscale.cpp          # C++ component implementation
├── packages/tailscale/
│   └── tailscale.yaml         # The end-user package (external_components + all entities)
├── microlink/                 # Git submodule: the Tailscale protocol implementation
├── example.yaml               # Reference config that uses the GitHub package
└── README.md                  # This file
```

---

## FAQ

**Q: Do I need a subnet router?**
No. That's the whole point. The ESP is its own Tailscale node.

**Q: Do I need Tailscale Funnel?**
No. Funnel publishes services to the public internet — that's unrelated. Everything here is private, tailnet-only.

**Q: Does this work with Headscale?**
Yes. Set `login_server: "https://your.headscale.example.com"` in the `tailscale:` block.

**Q: Can I use this on a plain ESP32 (not S3)?**
No. The S3 is required, and PSRAM is strongly recommended. The Tailscale stack is too heavy for stock ESP32 RAM.

**Q: Will this work over cellular / LTE / a hotspot?**
Yes, as long as outbound UDP and HTTPS are allowed. DERP (TCP 443) is used as a fallback so even heavily-firewalled networks usually work.

**Q: How much flash does it use?**
About 1 MB for the firmware including the Tailscale stack. 4 MB flash is tight; 8 MB is fine; 16 MB is comfortable with OTA.

**Q: Can I run multiple ESPs on the same auth key?**
Yes, if the auth key is marked **Reusable** in the Tailscale admin. Each ESP will get its own `100.x` address.

**Q: My tailnet has ACLs. Do I need to grant the ESP access to HA?**
Yes, Home Assistant needs to be allowed to reach the ESP's API port (default `6053`). If you use tags, tag the ESP when generating the auth key (e.g. `tag:esphome`) and write an ACL rule allowing your HA host (or your whole tailnet) to reach `tag:esphome:*`.

**Q: Can I ping the ESP from another tailnet node?**
Yes. Once connected, it responds to ICMP on its `100.x` address like any other Tailscale node. Great for sanity checks.

---

## Credits

- **[microlink](https://github.com/CamM2325/microlink)** by @CamM2325 — the actual Tailscale/WireGuard implementation that makes this possible.
- **[ESPHome](https://esphome.io/)** — the framework this plugs into.
- **[Tailscale](https://tailscale.com/)** — the WireGuard mesh we're joining.

---

## License

This project is released under the MIT License. See `LICENSE` for details.

microlink is included as a git submodule under its own license — see `microlink/LICENSE`.
