# Local Headscale test harness

A minimal Headscale instance used to **reproduce the current
`login_server` limit** in this component. It is not a working Headscale
endpoint — see *What this proves / what it doesn't* below for the exact
failure signature.

**Not a production setup** — no TLS, default secrets, single container.

## What this proves / what it doesn't

- ✅ The YAML `login_server` value reaches microlink at runtime.
- ✅ DNS resolves, TCP connects on port 80, and the HTTP/1.1 Upgrade
  request makes it into Headscale's `NoiseUpgradeHandler`.
- ❌ The Noise IK handshake then **always fails** with
  `chacha20poly1305: message authentication failed` on the Headscale
  side and `Failed to read Noise msg2 header` on the device side.
- ❌ The ESP never reaches `CONNECTED`, never gets a VPN IP, never
  appears in `headscale nodes list`.

Root cause: `ml_noise_init` in microlink hardcodes Tailscale SaaS's
Noise server public key (`microlink/components/microlink/src/ml_noise.c:232`)
when no remote pubkey is passed. Every Headscale instance generates its
own Noise keypair at first boot, so ChaCha20-Poly1305 can never
authenticate the ciphertext. Making Headscale actually work requires a
microlink patch: fetch the server's Noise pubkey from the Tailscale-
compatible `/key?v=2` HTTP endpoint during setup, then pass the result
into `ml_noise_init`. That change is tracked for a future release.

Additionally, microlink hardcodes TCP port **80** for the coordinator
(`ml_coord.c:230`), so this harness binds Headscale to host port 80 to
match. That is why the compose file maps `80:80`, not `8080:8080`.

## Prerequisites

- Docker (Docker Desktop on Windows/Mac works fine)
- Your workstation must be reachable from the ESP32 over WiFi — i.e., the
  ESP32 and the Docker host must share a LAN segment.
- Nothing else listening on host port 80 (IIS, Apache, local web
  servers). On Windows, check with `netstat -ano | findstr :80`.
- Inbound TCP 80 open in the host firewall. On Windows run (elevated):
  ```
  netsh advfirewall firewall add rule name="Headscale 80" dir=in action=allow protocol=TCP localport=80
  ```

## 1. Set the server URL

Figure out your host's LAN IP (the address the ESP32 will reach). On
Windows `ipconfig` and look at the adapter that is on the same network
as your ESP32. Edit `config/config.yaml` and replace the
`HOST_LAN_IP` placeholder with that address, for example:

```yaml
server_url: http://192.168.1.42
listen_addr: 0.0.0.0:80
```

Headscale bakes `server_url` into preauth keys and the registration
flow, so clients must reach it exactly as written — `localhost` will not
work for the ESP32.

## 2. Start Headscale

From this directory:

```bash
docker compose up -d
docker compose logs -f   # optional: watch it boot
```

First boot creates the SQLite database and keys under the
`headscale-data` volume. You should see:

```
INF listening and serving HTTP on: 0.0.0.0:80
```

## 3. Create a user and a preauth key

```bash
docker compose exec headscale headscale users create esp32
docker compose exec headscale headscale preauthkeys create \
    --user esp32 \
    --reusable \
    --expiration 24h
```

The second command prints a raw hex preauth key (not `tskey-auth-…` —
Headscale uses its own format). Copy it.

## 4. Point the ESPHome device at Headscale

In your device YAML:

```yaml
tailscale:
  auth_key: "<hex preauth key from step 3>"
  hostname: "esp32-test"
  login_server: "192.168.1.42"   # BARE IP — no scheme, no port
```

`login_server` must be a bare hostname or IPv4 address because microlink
does not parse URLs and hardcodes port 80.

Flash, then `esphome logs example-dev.yaml`. You will see:

```
[I][tailscale]: Calling microlink_init with auth_key=abc123def456... ctrl_host=192.168.1.42
[I][microlink]: Control plane from config: 192.168.1.42
[I][ml_coord]: Resolving 192.168.1.42...
[I][ml_coord]: Connecting to 192.168.1.42:80...
[I][ml_coord]: Sending Noise handshake (msg1=101 bytes, b64=...)
[E][ml_coord]: Handshake recv failed: -1 (errno=11)
[E][ml_coord]: Noise handshake failed
```

And on the Headscale side (`docker compose logs`):

```
ERR noise upgrade failed error="noise handshake failed: decrypting machine key: chacha20poly1305: message authentication failed"
```

That pair of log lines is the verification. It proves the plumbing
lands and identifies the exact layer (Noise IK, server pubkey
mismatch) that needs a microlink fix.

## 5. Tear down

```bash
docker compose down          # keep the volume (keys + db persist)
docker compose down -v       # full wipe
```

## Notes

- HTTP only. Real Tailscale clients use the ts2021 Noise protocol over
  plain TCP anyway (the Noise layer provides confidentiality), so TLS
  isn't required end-to-end — but this harness also doesn't attempt
  TLS at all. Do not expose this Headscale to the public internet.
- DERP is disabled in this config; it would not help since we never
  complete the control-plane handshake.
- We test this harness as a reproduction environment, not as a product
  feature. Tailscale SaaS remains the only supported control plane
  for this component.
