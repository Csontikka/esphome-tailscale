# Local Headscale test harness

A minimal Headscale instance for verifying the ESPHome Tailscale component
against a self-hosted control plane. **Not a production setup** — no TLS,
default secrets, single container.

The ESP32 flashed with this project's component joins this Headscale over
plain HTTP on the same LAN, using the `login_server` YAML option.

## Prerequisites

- Docker (Docker Desktop on Windows/Mac works fine)
- Your workstation must be reachable from the ESP32 over WiFi — i.e., the
  ESP32 and the Docker host must share a LAN segment.
- Inbound TCP 8080 open in the host firewall. On Windows run (elevated):
  ```
  netsh advfirewall firewall add rule name="Headscale 8080" dir=in action=allow protocol=TCP localport=8080
  ```

## 1. Set the server URL

Figure out your host's LAN IP (the one the ESP32 will see):

- **Windows:** `ipconfig` → look for the Wi-Fi adapter IPv4 address.
- **macOS/Linux:** `ip addr` or `ifconfig`.

Edit `config/config.yaml` and replace `HOST_LAN_IP` with that address, e.g.:

```yaml
server_url: http://192.168.1.42:8080
```

Headscale bakes this URL into preauth keys and the device-registration
flow, so clients must reach it exactly as written — `localhost` will not
work for the ESP32.

## 2. Start Headscale

From this directory:

```bash
docker compose up -d
docker compose logs -f   # optional: watch it boot
```

First boot creates the SQLite database and keys under the `headscale-data`
volume.

## 3. Create a user and a preauth key

```bash
docker compose exec headscale headscale users create esp32
docker compose exec headscale headscale preauthkeys create \
    --user esp32 \
    --reusable \
    --expiration 24h
```

The second command prints a key starting with `hskey-auth-` or similar —
copy it. That is what you paste into your ESPHome `secrets.yaml` as
`tailscale_auth_key`.

## 4. Point the ESPHome device at Headscale

In your device YAML (or `example-dev.yaml` for local development):

```yaml
tailscale:
  auth_key: !secret tailscale_auth_key
  hostname: "esp32-test"
  login_server: "http://192.168.1.42:8080"   # same HOST_LAN_IP as above
```

Flash, then `esphome logs example-dev.yaml`. On success you should see
something like:

```
[I][tailscale]: Calling microlink_init with auth_key=hskey-auth... ctrl_host=http://192.168.1.42:8080
[I][microlink]: Control plane from config: http://192.168.1.42:8080
```

followed by the normal registration / CONNECTED state transitions.

Back on the host you can verify the device joined:

```bash
docker compose exec headscale headscale nodes list
```

## 5. Tear down

```bash
docker compose down          # keep the volume (keys + db persist)
docker compose down -v       # full wipe
```

## Known rough edges

- HTTP only. Real Tailscale clients require HTTPS; microlink is more
  permissive and accepts plain HTTP for this testing path. Do not expose
  this Headscale to the public internet.
- DERP is disabled in this config. Direct UDP between the ESP32 and
  tailnet peers should still work on the same LAN.
- No exit nodes, no subnet routes — matches what the ESPHome component
  itself supports.
- We test this harness against Tailscale SaaS as the primary target and
  use Headscale here only to prove the `login_server` knob works
  end-to-end. Don't treat it as an endorsement of Headscale for
  production ESP32 deployments.
