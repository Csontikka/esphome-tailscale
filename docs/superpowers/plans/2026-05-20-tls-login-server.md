# TLS support in microlink for `login_server` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make microlink (the C client embedded in `esphome-tailscale`) accept `https://host[:port]` for `login_server` by wrapping the coordinator TCP socket with `esp_tls` and verifying the cert chain against ESP-IDF's bundled Mozilla CA roots. Plain-HTTP path remains the default and stays byte-identical.

**Architecture:** A small connection-layer dispatch in `ml_coord.c`. New `ml_conn_write`/`ml_conn_read` helpers branch on `use_tls`; `do_tcp_connect` and `fetch_server_pubkey` gain TLS branches that call `esp_tls_conn_new_sync`; URL parser accepts `https://` instead of rejecting it. Noise and HTTP/2 layers above are unchanged because they only see opaque bytes.

**Tech Stack:** C99, ESP-IDF `esp_tls` + `mbedtls` (already in the platform), ESP-IDF's bundled CA roots (`esp_crt_bundle_attach`), ESPHome Python codegen, ESP32-S3 target.

**Environment:** All work happens inside the `esphome` Docker container on `dockerhost01` (`ssh jonny@192.168.5.201`, then `docker exec esphome ...`). The local clone is at `/config/tailscale-local/` on that host. Working branch is `tls-login-server`. The user's test YAML is `/config/esphome-web-18e080.yaml`. Build command is `docker exec esphome /usr/local/bin/esphome compile /config/esphome-web-18e080.yaml`; flash command is `docker exec -it esphome /usr/local/bin/esphome upload /config/esphome-web-18e080.yaml --device 192.168.15.61`.

**Testing model:** True unit testing is impractical here (no host-side test harness for microlink). Each task ends with a concrete verification — usually "compile clean", a `grep` check, a log line, or a Headscale state — and a commit. We re-flash and watch device logs at three checkpoints (Tasks 13, 14, 15).

---

## File Structure

| File | Purpose | Owner |
| --- | --- | --- |
| `microlink/components/microlink/src/ml_coord.c` | Coordinator client — URL parsing, connect, send/recv, Noise framing | Modified |
| `microlink/components/microlink/include/microlink_internal.h` | Per-instance state struct (search will confirm exact path during Task 1) | Modified |
| `microlink/components/microlink/CMakeLists.txt` | Add `esp_tls` to component `REQUIRES` | Modified |
| `components/tailscale/__init__.py` | Python validator + sdkconfig fragments | Modified |
| `docs/superpowers/specs/2026-05-20-tls-login-server-design.md` | Approved design spec (already committed) | Reference only |

---

## Task 1: Confirm the build environment — esp_tls API, struct layout, line numbers

**Files (read-only):**
- Inspect: `/config/.esphome/build/esphome-web-18e080/managed_components/espressif__esp-tls/esp_tls.h` (or equivalent in the IDF tree)
- Inspect: `/config/tailscale-local/microlink/components/microlink/src/ml_coord.c`
- Inspect: `/config/tailscale-local/microlink/components/microlink/include/` (any header that declares `microlink_t`)
- Inspect: `/config/tailscale-local/microlink/components/microlink/CMakeLists.txt`

The spec was written from a snapshot read of these files; line numbers and exact API forms may have shifted. This task captures ground truth before any code is written.

- [ ] **Step 1: Locate `esp_tls.h` for the pinned IDF version and capture the sync-connect API signature**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'find / -name esp_tls.h -path \"*build*\" 2>/dev/null | head -3; echo ---; find / -name esp_tls.h 2>/dev/null | head -3'"
```

Then read whichever path is closest to the build artefacts and grep for the prototype:

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'grep -n \"esp_tls_conn_new_sync\\|esp_tls_init\\|esp_tls_conn_destroy\\|esp_tls_conn_read\\|esp_tls_conn_write\\|crt_bundle_attach\" <path-from-previous-step>'"
```

Record the exact signatures in a scratch note — they get used verbatim in Tasks 9 and 10.

- [ ] **Step 2: Find the `microlink_t` definition and the `coord_sock`/`coord_host`/`coord_port` fields**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'grep -rn \"struct microlink\\|coord_sock\\|coord_host\\|coord_port\\|use_tls\" /config/tailscale-local/microlink/components/microlink/ 2>/dev/null'"
```

Capture: the file + line of the struct definition, and the current type of `coord_sock` (int? something else?). Note whether `coord_host`/`coord_port` are stored as `char[]`, `char *`, or computed each connect.

- [ ] **Step 3: Re-locate the four hot spots in `ml_coord.c` whose line numbers the spec quotes**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'grep -n \"https://\\|do_tcp_connect\\|fetch_server_pubkey\\|coord_send\\|coord_recv\\|ml_recv(ml->coord_sock\" /config/tailscale-local/microlink/components/microlink/src/ml_coord.c'"
```

Save the current line numbers for: the `https://` rejection branch, `coord_send`, `coord_recv`, `do_tcp_connect`, `fetch_server_pubkey`, the two raw `ml_recv(ml->coord_sock, ...)` calls. Subsequent tasks reference these by purpose, not by line, but having the numbers handy speeds up review.

- [ ] **Step 4: Confirm `esp_tls` component is reachable from microlink's build**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cat /config/tailscale-local/microlink/components/microlink/CMakeLists.txt'"
```

Expected: an `idf_component_register` call with `REQUIRES` and/or `PRIV_REQUIRES`. Note which lists are present — Task 12 will add `esp_tls` to whichever exists, or add `REQUIRES` if neither does.

- [ ] **Step 5: Commit nothing**

This task only gathers ground truth. No file changes, no commit. Proceed to Task 2 with the notes from Steps 1-4 in hand.

---

## Task 2: Relax ESPHome Python validator to accept `https://` URLs

**Files:**
- Modify: `/config/tailscale-local/components/tailscale/__init__.py`

The component's Python config parses `login_server` and may currently reject `https://`. We let it through; the C side handles the actual scheme.

- [ ] **Step 1: Read the current validator**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'grep -n \"login_server\\|https://\\|http://\" /config/tailscale-local/components/tailscale/__init__.py'"
```

Identify the validator (likely a `cv.string` with a custom validator function, or `cv.url`).

- [ ] **Step 2: If the validator rejects `https://`, remove that branch**

Concrete pattern to look for:

```python
def validate_login_server(value):
    if value.startswith("https://"):
        raise cv.Invalid("https:// is not supported (TLS unimplemented)")
    ...
```

Replace with: accept the value, but still reject anything that isn't a recognised scheme / host / `host:port`. If the validator currently doesn't reject `https://` at all (the C side rejects at runtime), skip the edit and note that in the commit message.

- [ ] **Step 3: Compile-check the YAML accepts `https://`**

The user's YAML at `/config/esphome-web-18e080.yaml` already has `login_server: "https://vpn.daveys.xyz"`. Run:

```bash
ssh jonny@192.168.5.201 "docker exec esphome /usr/local/bin/esphome config /config/esphome-web-18e080.yaml 2>&1 | grep -A1 login_server"
```

Expected: no validation error; the resolved config shows `login_server: https://vpn.daveys.xyz`.

- [ ] **Step 4: Commit**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git add components/tailscale/__init__.py && git -c user.name=jonny190 -c user.email=jonny190@gmail.com commit -m \"feat(tailscale): accept https:// in login_server validator\"'"
```

If Step 2 was a no-op, skip the commit and add a note that nothing needed changing.

---

## Task 3: Enable ESP-IDF bundled CA certificates via sdkconfig fragments

**Files:**
- Modify: `/config/tailscale-local/components/tailscale/__init__.py`

ESPHome components inject sdkconfig fragments via `esp32.add_idf_sdkconfig_option(...)` in their codegen. We add the two flags the design calls out so the bundled CAs are available at link time.

- [ ] **Step 1: Locate existing sdkconfig calls in the file**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'grep -n \"add_idf_sdkconfig_option\\|sdkconfig\" /config/tailscale-local/components/tailscale/__init__.py'"
```

Find the function (usually `async def to_code(config):`) that already adds component-level sdkconfig settings — this is where the new lines go.

- [ ] **Step 2: Add the two sdkconfig options inside `to_code`**

Add immediately after the existing `add_idf_sdkconfig_option` calls (or at the top of `to_code` if none exist):

```python
esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL", True)
```

If `esp32` isn't imported at the top of the file, add `from esphome.components import esp32` to the imports.

- [ ] **Step 3: Verify the codegen picks it up**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'rm -rf /config/.esphome/build/esphome-web-18e080 && /usr/local/bin/esphome config /config/esphome-web-18e080.yaml >/dev/null && grep -E \"CERTIFICATE_BUNDLE\" /config/.esphome/build/esphome-web-18e080/sdkconfig.defaults 2>/dev/null'"
```

Expected: both lines present. If `sdkconfig.defaults` doesn't exist yet, try `sdkconfig` or the file referenced by `find /config/.esphome/build/esphome-web-18e080 -name 'sdkconfig*'`.

- [ ] **Step 4: Commit**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git add components/tailscale/__init__.py && git -c user.name=jonny190 -c user.email=jonny190@gmail.com commit -m \"feat(tailscale): enable mbedTLS CA bundle for TLS login_server\"'"
```

---

## Task 4: Add `use_tls` and `coord_tls` fields to the microlink state struct

**Files:**
- Modify: the header identified in Task 1 Step 2 (placeholder name: `microlink/components/microlink/include/microlink.h` or `..._internal.h`)

- [ ] **Step 1: Add the include for `esp_tls.h`**

Near the existing system includes in the same header:

```c
#include "esp_tls.h"
```

- [ ] **Step 2: Add two fields to the `microlink_t` struct (or whatever the state struct is named — Task 1 confirmed the actual name)**

Adjacent to the existing `coord_sock` field:

```c
    bool      use_tls;          /* true if login_server URL is https:// */
    esp_tls_t *coord_tls;       /* NULL when use_tls is false */
```

If the struct is in C with `_Bool` rather than `bool`, include `<stdbool.h>` first (it's already in most translation units, but verify).

- [ ] **Step 3: Initialise the two new fields wherever the struct is zero-initialised today**

Search for the existing zero-init of `coord_sock` (likely `ml->coord_sock = -1;` or `memset(ml, 0, ...)`); if memset zeroes the whole struct the new fields are covered for free. Otherwise add:

```c
ml->use_tls = false;
ml->coord_tls = NULL;
```

next to the existing `coord_sock` init.

- [ ] **Step 4: Compile-check**

```bash
ssh jonny@192.168.5.201 "docker exec esphome /usr/local/bin/esphome compile /config/esphome-web-18e080.yaml 2>&1 | tail -30"
```

Expected: clean compile (we haven't used the new fields yet, so they should just be unused-warning-free additions).

- [ ] **Step 5: Commit**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git add -u microlink/ && git -c user.name=jonny190 -c user.email=jonny190@gmail.com commit -m \"feat(microlink): add use_tls and coord_tls fields to state\"'"
```

---

## Task 5: URL parser — accept `https://` instead of rejecting it

**Files:**
- Modify: `microlink/components/microlink/src/ml_coord.c` (the `https://` rejection branch)

Reference: the design spec quotes the current code:

```c
} else if (strncasecmp(p, "https://", 8) == 0) {
    ESP_LOGE(TAG, "https:// control plane URL is not supported (TLS unimplemented): %s", in);
    // returns / errors out
}
```

- [ ] **Step 1: Find the rejection branch (line confirmed in Task 1 Step 3)**

Read 30 lines around the `https://` token to see the full conditional ladder and how the `http://` branch sets the default port.

- [ ] **Step 2: Replace the rejection with TLS-accepting code**

Replace the `ESP_LOGE` + error-return block with:

```c
} else if (strncasecmp(p, "https://", 8) == 0) {
    ml->use_tls = true;
    p += 8;
    /* Default port for https is 443; the existing host[:port] parser
     * below will override this if the URL contains an explicit ":port". */
    default_port = "443";
}
```

The exact variable name `default_port` will match whatever the `http://` branch uses (Task 1 Step 3 captured the surrounding code). Mirror that branch as closely as possible so the parsing path is identical except for the port default and the `use_tls` flag.

- [ ] **Step 3: Compile-check**

```bash
ssh jonny@192.168.5.201 "docker exec esphome /usr/local/bin/esphome compile /config/esphome-web-18e080.yaml 2>&1 | tail -20"
```

Expected: clean compile. The compiler may warn about `use_tls` being set but never read — that's fine until Task 9 reads it.

- [ ] **Step 4: Smoke-test parsing on the device (optional but cheap)**

If the device is flashed and reachable, watch logs after a reboot:

```bash
ssh jonny@192.168.5.201 "docker exec -it esphome /usr/local/bin/esphome logs /config/esphome-web-18e080.yaml --device 192.168.15.61 2>&1 | head -60"
```

Expected: no more `Invalid login_server 'https://vpn.daveys.xyz'`. The next error will likely be a TCP failure (Tasks 6-10 fix this).

- [ ] **Step 5: Commit**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git add microlink/components/microlink/src/ml_coord.c && git -c user.name=jonny190 -c user.email=jonny190@gmail.com commit -m \"feat(microlink): accept https:// login_server URL (default port 443)\"'"
```

---

## Task 6: Add `ml_conn_write` and `ml_conn_read` helpers

**Files:**
- Modify: `microlink/components/microlink/src/ml_coord.c`

These are the only places that need to know about TLS vs raw socket. Everything above them is unchanged.

- [ ] **Step 1: Add the two static helpers immediately above `coord_send`**

Place near the top of the file (after existing static helpers, before `coord_send`):

```c
static int ml_conn_write(microlink_t *ml, const uint8_t *buf, size_t len) {
    if (ml->use_tls) {
        ssize_t n = esp_tls_conn_write(ml->coord_tls, buf, len);
        if (n < 0) {
            ESP_LOGE(TAG, "ml_conn_write: esp_tls_conn_write failed: %d", (int)n);
            return -1;
        }
        return (int)n;
    }
    return ml_send(ml->coord_sock, (uint8_t *)buf, len, 0);
}

static int ml_conn_read(microlink_t *ml, uint8_t *buf, size_t len) {
    if (ml->use_tls) {
        ssize_t n = esp_tls_conn_read(ml->coord_tls, buf, len);
        if (n < 0) {
            /* esp-tls returns -1 on WANT_READ/WANT_WRITE in non-blocking mode;
             * callers already handle 0 / negative the same as the raw path. */
            return -1;
        }
        return (int)n;
    }
    return ml_recv(ml->coord_sock, buf, len, 0);
}
```

Add `#include "esp_tls.h"` at the top of the file if it isn't already pulled in via the header from Task 4.

- [ ] **Step 2: Compile-check**

```bash
ssh jonny@192.168.5.201 "docker exec esphome /usr/local/bin/esphome compile /config/esphome-web-18e080.yaml 2>&1 | tail -20"
```

Expected: clean. The helpers may warn as unused-functions until Task 7 wires them in.

- [ ] **Step 3: Commit**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git add microlink/components/microlink/src/ml_coord.c && git -c user.name=jonny190 -c user.email=jonny190@gmail.com commit -m \"feat(microlink): add ml_conn_write/read TLS-aware helpers\"'"
```

---

## Task 7: Route `coord_send` / `coord_recv` through the helpers

**Files:**
- Modify: `microlink/components/microlink/src/ml_coord.c`

- [ ] **Step 1: Replace the body of `coord_send` (location captured in Task 1 Step 3)**

Current shape:

```c
static int coord_send(microlink_t *ml, const uint8_t *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = ml_send(ml->coord_sock, data + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}
```

Change the inner `ml_send(ml->coord_sock, ...)` to `ml_conn_write(ml, ...)`:

```c
static int coord_send(microlink_t *ml, const uint8_t *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = ml_conn_write(ml, data + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}
```

Leave the surrounding loop, error returns, and signature exactly as-is.

- [ ] **Step 2: Replace the inner call inside `coord_recv` similarly**

Current:

```c
int n = ml_recv(ml->coord_sock, buf + recvd, len - recvd, 0);
```

Becomes:

```c
int n = ml_conn_read(ml, buf + recvd, len - recvd);
```

- [ ] **Step 3: Compile-check**

```bash
ssh jonny@192.168.5.201 "docker exec esphome /usr/local/bin/esphome compile /config/esphome-web-18e080.yaml 2>&1 | tail -20"
```

Expected: clean. The unused-warning on the helpers from Task 6 goes away.

- [ ] **Step 4: Commit**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git add microlink/components/microlink/src/ml_coord.c && git -c user.name=jonny190 -c user.email=jonny190@gmail.com commit -m \"refactor(microlink): route coord_send/coord_recv through ml_conn helpers\"'"
```

---

## Task 8: Route the two loose `ml_recv(ml->coord_sock, ...)` sites through `ml_conn_read`

**Files:**
- Modify: `microlink/components/microlink/src/ml_coord.c` (the two sites identified in Task 1 Step 3 — spec quotes lines 598 and 732, may shift)

- [ ] **Step 1: Locate the first site (HTTP response read after `coord_send` of the upgrade request)**

Current (approximate):

```c
int total = ml_recv(ml->coord_sock, resp, 2047, 0);
```

Becomes:

```c
int total = ml_conn_read(ml, resp, 2047);
```

- [ ] **Step 2: Locate the second site (extra-data drain)**

Current:

```c
int n = ml_recv(ml->coord_sock, extra_data, 1024, 0);
```

Becomes:

```c
int n = ml_conn_read(ml, extra_data, 1024);
```

- [ ] **Step 3: Search for any other stray sockets calls we missed**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'grep -n \"ml_recv(ml->coord_sock\\|ml_send(ml->coord_sock\" /config/tailscale-local/microlink/components/microlink/src/ml_coord.c'"
```

Expected output: only what's inside `ml_conn_write`/`ml_conn_read` (the fallback path) and `fetch_server_pubkey` (Task 10 handles that one — distinct because it uses its own local socket, not `ml->coord_sock`).

- [ ] **Step 4: Compile-check**

```bash
ssh jonny@192.168.5.201 "docker exec esphome /usr/local/bin/esphome compile /config/esphome-web-18e080.yaml 2>&1 | tail -20"
```

Expected: clean.

- [ ] **Step 5: Commit**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git add microlink/components/microlink/src/ml_coord.c && git -c user.name=jonny190 -c user.email=jonny190@gmail.com commit -m \"refactor(microlink): route loose ml_recv coord_sock calls through ml_conn_read\"'"
```

---

## Task 8b: Add `ml_conn_sockfd` helper + route setsockopt/select sites

**Files:**
- Modify: `microlink/components/microlink/src/ml_coord.c`

Task 1 recon found that `ml_coord.c` references `ml->coord_sock` in 6 `ml_setsockopt` calls (lines 307, 728, 746, 1698, 1805, 2194) and 2 `FD_SET`/`ml_select_fds` calls (2187-2189). With TLS the raw `coord_sock` is `-1`; these sites must route through the underlying socket fd that `esp_tls` is sitting on top of, or they silently no-op (setsockopt) / hit undefined behaviour (`FD_SET(-1, ...)`). The select-peek at line 2187 is on the streaming MapResponse hot path — leaving it broken regresses TLS reconnects.

This task lands a single helper and uses it everywhere a raw fd is needed.

- [ ] **Step 1: Add the helper near `ml_conn_write`/`ml_conn_read`**

```c
static int ml_conn_sockfd(microlink_t *ml) {
    if (ml->use_tls) {
        int fd = -1;
        if (ml->coord_tls && esp_tls_get_conn_sockfd(ml->coord_tls, &fd) == ESP_OK) {
            return fd;
        }
        return -1;
    }
    return ml->coord_sock;
}
```

- [ ] **Step 2: Replace each `ml_setsockopt(ml->coord_sock, ...)` with `ml_setsockopt(ml_conn_sockfd(ml), ...)`**

Six call sites. `grep -n "ml_setsockopt(ml->coord_sock" microlink/components/microlink/src/ml_coord.c` lists them. Mechanical substitution; do not change the rest of each call.

- [ ] **Step 3: Replace the `FD_SET` and `ml_select_fds` sites at lines 2187-2189**

Current:

```c
FD_SET(ml->coord_sock, &readfds);
ml_select_fds(ml->coord_sock + 1, &readfds, NULL, NULL, &tv);
```

Becomes:

```c
int peek_fd = ml_conn_sockfd(ml);
if (peek_fd < 0) {
    /* No usable fd (TLS context vanished). Treat as no-data. */
    sel = 0;
} else {
    FD_SET(peek_fd, &readfds);
    sel = ml_select_fds(peek_fd + 1, &readfds, NULL, NULL, &tv);
}
```

(The exact variable `sel` matches the existing `int sel = ...` on line 2189 — keep its name.)

- [ ] **Step 4: At the same select site, peek TLS-buffered bytes first**

`mbedtls_ssl_read` may have decrypted bytes buffered internally even when the underlying socket reads zero. Before the `select`, ask `esp_tls`:

```c
if (ml->use_tls && ml->coord_tls) {
    size_t avail = esp_tls_get_bytes_avail(ml->coord_tls);
    if (avail > 0) {
        sel = 1;   /* short-circuit: tell the rest of the function data is ready */
        goto have_data;   /* or whatever the existing "data ready" path is */
    }
}
```

Where `have_data:` is whatever label / branch the existing `sel > 0` path falls into. If the surrounding code is structured as `if (sel > 0) { ... }`, the simpler form is to set `sel = 1` before the select branch and skip the `FD_SET`/`select` entirely.

- [ ] **Step 5: Compile-check**

```bash
ssh jonny@192.168.5.201 "docker exec esphome /usr/local/bin/esphome compile /config/esphome-web-18e080.yaml 2>&1 | tail -20"
```

Expected: clean compile.

- [ ] **Step 6: Commit**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git add microlink/components/microlink/src/ml_coord.c && git -c user.name=jonny190 -c user.email=jonny190@gmail.com commit -m \"feat(microlink): route setsockopt/select through TLS-aware sockfd helper\"'"
```

---

## Task 9: Add TLS branch to `do_tcp_connect`

**Files:**
- Modify: `microlink/components/microlink/src/ml_coord.c` (the `do_tcp_connect` function)

This is the moment of truth — we open a real TLS connection.

- [ ] **Step 1: Read the full `do_tcp_connect` function**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'awk \"/^static int do_tcp_connect/,/^}/\" /config/tailscale-local/microlink/components/microlink/src/ml_coord.c | head -80'"
```

Capture how it currently does DNS resolution, socket creation, connect, and stores `ml->coord_sock`. The TLS branch replaces all of that when `use_tls` is true.

- [ ] **Step 2: Add a TLS branch at the top of the function body**

Insert immediately after the function's local-variable declarations, before the existing DNS / socket code:

```c
if (ml->use_tls) {
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
        .non_block         = false,
    };
    esp_tls_t *tls = esp_tls_init();
    if (tls == NULL) {
        ESP_LOGE(TAG, "do_tcp_connect: esp_tls_init failed");
        return -1;
    }
    int port_i = atoi(ml->coord_port);
    int rc = esp_tls_conn_new_sync(ml->coord_host, strlen(ml->coord_host),
                                   port_i, &cfg, tls);
    if (rc != 1) {
        ESP_LOGE(TAG, "do_tcp_connect: TLS handshake to %s:%d failed (rc=%d)",
                 ml->coord_host, port_i, rc);
        esp_tls_conn_destroy(tls);
        return -1;
    }
    ml->coord_tls  = tls;
    ml->coord_sock = -1;  /* signal "no raw socket" */
    ESP_LOGI(TAG, "TLS connected to %s:%d", ml->coord_host, port_i);
    return 0;
}
/* else: existing plain-TCP path below, unchanged */
```

Add the include `#include "esp_crt_bundle.h"` at the top of the file (next to the existing `esp_tls.h` include).

**If Task 1 Step 1 showed a different `esp_tls_conn_new_sync` signature** for this IDF version (e.g. returns `esp_tls_t *` directly, or takes a single URL string), use that form instead — the structure of the branch stays the same, only the call line changes.

- [ ] **Step 3: Compile-check**

```bash
ssh jonny@192.168.5.201 "docker exec esphome /usr/local/bin/esphome compile /config/esphome-web-18e080.yaml 2>&1 | tail -40"
```

Expected: clean compile. If linker complains about `esp_crt_bundle_attach`, double-check Task 3's sdkconfig fragments were picked up — `find ... -name sdkconfig* | xargs grep CERTIFICATE_BUNDLE`.

- [ ] **Step 4: Flash and watch logs for the new TLS path**

```bash
ssh jonny@192.168.5.201 "docker exec -it esphome /usr/local/bin/esphome upload /config/esphome-web-18e080.yaml --device 192.168.15.61 2>&1 | tail -30"
ssh jonny@192.168.5.201 "docker exec -it esphome /usr/local/bin/esphome logs /config/esphome-web-18e080.yaml --device 192.168.15.61 2>&1 | head -100"
```

Expected log lines (within ~30 s of boot):
```
[I][ml_coord]: TLS connected to vpn.daveys.xyz:443
```
If the handshake fails, we get `TLS handshake to vpn.daveys.xyz:443 failed (rc=<errno>)` — feed that into the `esp_tls` error-code table in `esp_tls.h` to diagnose (cert chain, hostname, timeout). The most likely first failure is the CA bundle not being linked — re-verify Task 3.

- [ ] **Step 5: Commit**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git add microlink/components/microlink/src/ml_coord.c && git -c user.name=jonny190 -c user.email=jonny190@gmail.com commit -m \"feat(microlink): TLS handshake in do_tcp_connect when use_tls set\"'"
```

---

## Task 10: Add TLS branch to `fetch_server_pubkey`

**Files:**
- Modify: `microlink/components/microlink/src/ml_coord.c` (the `fetch_server_pubkey` function)

This function fetches `/key?v=88` to learn Headscale's Noise public key. It uses a separate, short-lived socket — not `ml->coord_sock`. Same dispatch pattern, different lifecycle.

- [ ] **Step 1: Read the current function**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'awk \"/static int fetch_server_pubkey/,/^}/\" /config/tailscale-local/microlink/components/microlink/src/ml_coord.c | head -120'"
```

Note: this function uses its OWN local `int sock` and does its own send/recv. We replicate the same dispatch pattern but with a local `esp_tls_t *` instead.

- [ ] **Step 2: Add a TLS branch at the top of the function body**

Insert before the existing DNS/socket setup:

```c
if (ml->use_tls) {
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
        .non_block         = false,
    };
    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        ESP_LOGE(TAG, "fetch_server_pubkey: esp_tls_init failed");
        return -1;
    }
    int port_i = atoi(ml->coord_port);
    if (esp_tls_conn_new_sync(ml->coord_host, strlen(ml->coord_host),
                              port_i, &cfg, tls) != 1) {
        ESP_LOGE(TAG, "fetch_server_pubkey: TLS handshake failed");
        esp_tls_conn_destroy(tls);
        return -1;
    }

    /* Build and send the same HTTP request the plain path uses */
    char req[256];
    int req_len = snprintf(req, sizeof(req),
        "GET /key?v=88 HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
        ml->coord_host);
    if (esp_tls_conn_write(tls, (uint8_t *)req, req_len) != req_len) {
        ESP_LOGE(TAG, "fetch_server_pubkey: TLS write failed");
        esp_tls_conn_destroy(tls);
        return -1;
    }

    /* Read response into the existing local response buffer.
     * Mirror the plain path's read loop verbatim, but call esp_tls_conn_read. */
    char resp[2048];
    int total = 0;
    while (total < (int)sizeof(resp) - 1) {
        int n = esp_tls_conn_read(tls, (uint8_t *)resp + total,
                                  sizeof(resp) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    esp_tls_conn_destroy(tls);

    if (total <= 0) {
        ESP_LOGE(TAG, "fetch_server_pubkey: empty TLS response");
        return -1;
    }
    resp[total] = '\0';

    /* Hand off to the same response-parsing code the plain path uses below.
     * The parser is currently inline in this function — refactor to call
     * a small static parser if it's not already factored out. */
    return parse_pubkey_response(resp, total, /* out params */);
}
/* else: existing plain-TCP path below, unchanged */
```

**Important:** the existing function probably has the response parsing inline. If so, **before** Step 2, extract the parsing into a static helper `parse_pubkey_response()` so both branches share it — that keeps DRY. If extraction would explode the diff, the alternative is to fall through to the existing parser block by setting local `sock = -1` and the response buffer, but that's fragile. Prefer extraction.

- [ ] **Step 3: Compile-check**

```bash
ssh jonny@192.168.5.201 "docker exec esphome /usr/local/bin/esphome compile /config/esphome-web-18e080.yaml 2>&1 | tail -20"
```

Expected: clean.

- [ ] **Step 4: Flash + watch logs through the pubkey fetch**

```bash
ssh jonny@192.168.5.201 "docker exec -it esphome /usr/local/bin/esphome upload /config/esphome-web-18e080.yaml --device 192.168.15.61 2>&1 | tail -10"
ssh jonny@192.168.5.201 "docker exec -it esphome /usr/local/bin/esphome logs /config/esphome-web-18e080.yaml --device 192.168.15.61 2>&1 | head -120"
```

Expected: after wifi connect and SNTP sync, microlink logs the Noise pubkey fetch (look for `fetch_server_pubkey` or `Noise server public key`). No more `https:// unsupported` errors. The next failure (if any) is the registration handshake, which Tasks 6-9 already prepared for.

- [ ] **Step 5: Commit**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git add microlink/components/microlink/src/ml_coord.c && git -c user.name=jonny190 -c user.email=jonny190@gmail.com commit -m \"feat(microlink): TLS branch in fetch_server_pubkey\"'"
```

---

## Task 11: TLS-aware cleanup in disconnect paths

**Files:**
- Modify: `microlink/components/microlink/src/ml_coord.c`

Wherever the existing code calls `close(ml->coord_sock)` or `shutdown(ml->coord_sock, ...)`, we also need to tear down `coord_tls`. Otherwise we leak the TLS context on every reconnect.

- [ ] **Step 1: Find every `close` / `shutdown` site against `coord_sock`**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'grep -n \"close(ml->coord_sock\\|shutdown(ml->coord_sock\" /config/tailscale-local/microlink/components/microlink/src/ml_coord.c'"
```

- [ ] **Step 2: At each site, add a TLS teardown immediately above the existing close**

Pattern:

```c
if (ml->coord_tls) {
    esp_tls_conn_destroy(ml->coord_tls);
    ml->coord_tls = NULL;
}
if (ml->coord_sock >= 0) {
    close(ml->coord_sock);
    ml->coord_sock = -1;
}
```

Replace the existing `close(ml->coord_sock)` with the dual block at each site. If the existing code already guards `coord_sock >= 0`, leave that guard intact. Setting fields to `NULL`/`-1` after teardown is essential — the reconnect loop checks them.

- [ ] **Step 3: Compile-check**

```bash
ssh jonny@192.168.5.201 "docker exec esphome /usr/local/bin/esphome compile /config/esphome-web-18e080.yaml 2>&1 | tail -20"
```

- [ ] **Step 4: Reconnect test on device**

Disconnect Wi-Fi for ~10 seconds (toggle in the AP's admin, or yank the device's antenna). Watch logs:

```bash
ssh jonny@192.168.5.201 "docker exec -it esphome /usr/local/bin/esphome logs /config/esphome-web-18e080.yaml --device 192.168.15.61 2>&1 | head -200"
```

Expected: after WiFi recovers, we see a second `TLS connected to ...` log line (the reconnect). Free heap (search for `Free heap` log lines or the ESPHome "Free Heap Size" sensor) should be roughly stable across reconnects, not monotonically falling.

- [ ] **Step 5: Commit**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git add microlink/components/microlink/src/ml_coord.c && git -c user.name=jonny190 -c user.email=jonny190@gmail.com commit -m \"fix(microlink): destroy TLS context on coord disconnect to avoid leak\"'"
```

---

## Task 12: Declare `esp_tls` as a microlink CMake dependency

**Files:**
- Modify: `microlink/components/microlink/CMakeLists.txt`

Without this, the linker can resolve the symbols (because some other component pulls them in transitively) but the build is fragile across IDF updates.

- [ ] **Step 1: Inspect the current `idf_component_register` block**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cat /config/tailscale-local/microlink/components/microlink/CMakeLists.txt'"
```

- [ ] **Step 2: Add `esp_tls` to `REQUIRES`**

If the file currently has `REQUIRES foo bar baz`, change it to `REQUIRES foo bar baz esp_tls`. If it has no `REQUIRES`, add one:

```cmake
REQUIRES esp_tls mbedtls
```

(mbedtls is usually pulled in transitively but listing it makes the dependency explicit.)

- [ ] **Step 3: Clean-build to confirm CMake re-resolves dependencies**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'rm -rf /config/.esphome/build/esphome-web-18e080 && /usr/local/bin/esphome compile /config/esphome-web-18e080.yaml 2>&1 | tail -15'"
```

Expected: `Successfully compiled program.`

- [ ] **Step 4: Commit**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git add microlink/components/microlink/CMakeLists.txt && git -c user.name=jonny190 -c user.email=jonny190@gmail.com commit -m \"build(microlink): declare esp_tls/mbedtls in REQUIRES\"'"
```

---

## Task 13: Checkpoint 1 — full clean build from a wiped cache

**Files:** none — this is a verification gate.

- [ ] **Step 1: Wipe ALL build caches**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'rm -rf /config/.esphome/build/esphome-web-18e080 && rm -rf /config/.esphome/external_components/* 2>/dev/null; ls /config/.esphome/external_components/ 2>/dev/null'"
```

(The external_components wipe is paranoia — the local-path source from Task 0 should mean nothing's there anyway.)

- [ ] **Step 2: Compile from scratch**

```bash
ssh jonny@192.168.5.201 "docker exec esphome /usr/local/bin/esphome compile /config/esphome-web-18e080.yaml 2>&1 | tail -50"
```

Expected: `Successfully compiled program.`, total compile time roughly comparable to previous (Task 1 baseline was 213 s — TLS adds maybe 10-20 s of mbedtls compile).

- [ ] **Step 3: Verify CA bundle made it into the binary**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'grep CERTIFICATE_BUNDLE /config/.esphome/build/esphome-web-18e080/sdkconfig'"
```

Expected: both options set to `y`.

- [ ] **Step 4: Note binary size for the record**

Capture the "Flash used" line from the build output. Compare with the pre-TLS build (was 57.5%). Expect ~5-8% increase. If it's >20% something is wrong (probably the FULL bundle pulling in more than it should — drop to non-FULL variant).

- [ ] **Step 5: No commit needed; this is a gate.**

---

## Task 14: Checkpoint 2 — TLS handshake succeeds against `vpn.daveys.xyz`

**Files:** none — verification gate.

- [ ] **Step 1: Flash the device**

```bash
ssh jonny@192.168.5.201 "docker exec -it esphome /usr/local/bin/esphome upload /config/esphome-web-18e080.yaml --device 192.168.15.61 2>&1 | tail -15"
```

- [ ] **Step 2: Stream logs from boot for ~2 minutes**

```bash
ssh jonny@192.168.5.201 "timeout 120 docker exec -it esphome /usr/local/bin/esphome logs /config/esphome-web-18e080.yaml --device 192.168.15.61 2>&1"
```

- [ ] **Step 3: Confirm the expected sequence**

Look for, in order:
1. `WiFi:` connected
2. `SNTP:` time synchronized (TLS cert validation needs a real clock)
3. `[I][ml_coord]: TLS connected to vpn.daveys.xyz:443` (or equivalent — wording matches the `ESP_LOGI` from Task 9)
4. `[I][ml_coord]: Noise server public key:` (or similar — pubkey fetched OK)

Failure modes:
- No #3 → handshake failing. Enable `esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);` (temporarily, in component setup) and re-flash to capture cert error details.
- #3 but no #4 → pubkey fetch path broke (Task 10). Re-check the response read loop.

- [ ] **Step 4: No commit needed; this is a gate.**

---

## Task 15: Checkpoint 3 — node registers in Headscale

**Files:** none — verification gate.

- [ ] **Step 1: From the VPS, list Headscale nodes**

```bash
ssh admin@82.26.193.37 "docker exec headscale headscale nodes list 2>/dev/null || sudo headscale nodes list"
```

(Adjust to however the user's Headscale is invoked — container vs systemd binary. Memory says it's hosted on the VPS but not which form.)

Expected: a row with `esp32-tailscale` and a `100.64.x.x` IP, status `online`.

- [ ] **Step 2: From the device side, confirm the ESPHome sensors**

In Home Assistant (or via ESPHome dashboard webUI on `192.168.15.61`), check:
- `VPN Connected` → `ON`
- `VPN IP` → `100.64.x.x`
- `VPN Setup Hint` → empty or "Connected"
- `VPN MagicDNS` → populated if Headscale has DNS configured

- [ ] **Step 3: Quick connectivity smoke test**

From the VPS (or another tailnet peer):

```bash
ping -c 4 <esp32-tailscale-vpn-ip>
```

Expected: replies.

- [ ] **Step 4: No commit needed; this is the final gate.**

If all three steps pass, the implementation is functionally complete. Move to Task 16.

---

## Task 16: Push branch to `jonny190/esphome-tailscale`

**Files:** none — git operation.

- [ ] **Step 1: Confirm or create the GitHub fork**

The user needs to fork `Csontikka/esphome-tailscale` to `jonny190/esphome-tailscale` via the GitHub UI if they haven't already. (Forking via `gh` from the container is fine if `gh` and a token are present; otherwise hand it back to the user.)

```bash
curl -s -o /dev/null -w "%{http_code}\n" https://github.com/jonny190/esphome-tailscale
```

Expected: `200` (fork exists) or `404` (needs creation).

- [ ] **Step 2: Add the fork as `fork` remote and push**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git remote add fork https://github.com/jonny190/esphome-tailscale.git 2>/dev/null; git remote -v'"
```

Then push, supplying credentials interactively (GitHub PAT in HTTPS form, or set up SSH key inside container). If the container has neither, do the push from outside the container instead:

```bash
ssh jonny@192.168.5.201 "cd /tmp && git clone /config/tailscale-local tailscale-push && cd tailscale-push && git remote set-url origin https://github.com/jonny190/esphome-tailscale.git && git push -u origin tls-login-server"
```

(Adjust auth as needed — the user knows their environment.)

- [ ] **Step 3: Verify the branch landed**

```bash
curl -s -o /dev/null -w "%{http_code}\n" https://github.com/jonny190/esphome-tailscale/tree/tls-login-server
```

Expected: `200`.

- [ ] **Step 4: No commit; the push IS the deliverable.**

---

## Task 17: Repoint `esphome-web-18e080.yaml` at the fork

**Files:**
- Modify: `/config/esphome-web-18e080.yaml` (in the container)

Restore the YAML to the standard `packages:` form, pointing at the fork instead of the local clone.

- [ ] **Step 1: Edit the YAML**

Replace the local-include line:

```yaml
packages:
  tailscale: !include /config/tailscale-local/packages/tailscale/tailscale.yaml
```

with:

```yaml
packages:
  tailscale:
    url: https://github.com/jonny190/esphome-tailscale
    ref: tls-login-server
    files: [packages/tailscale/tailscale.yaml]
    refresh: 1d
```

(`refresh: 1d` rather than `0s` so cache isn't thrashed.)

- [ ] **Step 2: Also restore the fork's `packages/tailscale/tailscale.yaml`**

In the fork repo, the package's internal `external_components` block was rewritten to `type: local` during early debugging. It needs to be `type: git` again, pointing at the same fork:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jonny190/esphome-tailscale.git
      ref: tls-login-server
    components: [tailscale]
    refresh: 1d
```

Commit + push that change as well.

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'cd /config/tailscale-local && git add packages/tailscale/tailscale.yaml && git -c user.name=jonny190 -c user.email=jonny190@gmail.com commit -m \"chore: point package external_components at fork\" && git push fork tls-login-server'"
```

- [ ] **Step 3: Wipe cache and rebuild end-to-end from GitHub**

```bash
ssh jonny@192.168.5.201 "docker exec esphome bash -c 'rm -rf /config/.esphome/build/esphome-web-18e080 /config/.esphome/external_components/* && /usr/local/bin/esphome compile /config/esphome-web-18e080.yaml 2>&1 | tail -20'"
```

Expected: clean compile, the component pulled from GitHub, identical binary behaviour to the local-clone build.

- [ ] **Step 4: Flash and re-verify Checkpoint 3**

Repeat Task 15.

---

## Task 18: Optional — open PR upstream

**Files:** none — GitHub UI.

Once it's been running stably for a few days, open a PR from `jonny190/esphome-tailscale:tls-login-server` to `Csontikka/esphome-tailscale:main`. Reference the design spec in the PR body. The defines.h fix commit is independently valuable and could be split into its own PR if the maintainer prefers atomic changes.

- [ ] **Step 1: User decision — file PR or keep as private fork.**

(No code change. Sentinel for completion.)

---

## Self-review notes

- **Spec coverage:** every section of the spec maps to a task — URL parsing (T5), conn helpers (T6-8), connect (T9), pubkey (T10), cleanup (T11), CMake (T12), CA bundle (T3), Python validator (T2), state struct (T4). Testing checkpoints (T13-15) match the spec's three. Rollout (T16-17). One thing the spec mentions briefly but no task touches: the spec's "Out of scope" callouts — those are intentionally not tasks.
- **Placeholder scan:** no TBDs in this plan. All code blocks contain the actual code to write. Task 1's "capture API signature" is exploration, not a placeholder.
- **Type consistency:** `ml->use_tls` (bool), `ml->coord_tls` (`esp_tls_t *`), `ml->coord_sock` (int), `ml->coord_host` (string), `ml->coord_port` (string, parsed via `atoi` at use site). Helper names: `ml_conn_read`/`ml_conn_write` used consistently from T6 onward.
