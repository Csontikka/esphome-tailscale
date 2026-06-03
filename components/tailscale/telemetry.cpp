/* Anonymous telemetry — see telemetry.h for the full design + payload spec.
 * SPDX-License-Identifier: MIT */

#include "telemetry.h"

#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps — PSRAM stack */
#include "freertos/semphr.h"

#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
#include "esp_core_dump.h"
#endif

namespace esphome {
namespace tailscale {

static const char *const TAG = "tailscale.telemetry";

#define TLM_NVS_NAMESPACE   "tailscale"      /* shared with the component */
#define TLM_NVS_BOOT_CNT    "tm_boot"

/* Grace before the first send: lets microlink finish its initial DERP+STUN
 * TLS handshakes so their transient mbedTLS buffers don't collide with ours. */
#define TLM_FIRST_SEND_DELAY_S   180u
#define TLM_HEARTBEAT_PERIOD_S   86400u

static const uint32_t TLM_RETRY_BACKOFF_S[] = {30, 60, 120, 300, 600};
#define TLM_RETRY_BACKOFF_N (sizeof(TLM_RETRY_BACKOFF_S) / sizeof(TLM_RETRY_BACKOFF_S[0]))

static std::atomic<bool> s_connected{false};
static TaskHandle_t      s_task = nullptr;
static char              s_device_hash[17] = "";   /* 16 hex + NUL */
static uint32_t          s_boot_count = 0;
static char              s_chip_buf[16] = "";
static char              s_crash_sig[200] = "";     /* "" when no pending crash */

static void compute_device_hash() {
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);

  static const char salt[] = "esphome-tailscale-v1";
  uint8_t input[6 + sizeof(salt) - 1];
  memcpy(input, mac, 6);
  memcpy(input + 6, salt, sizeof(salt) - 1);

  uint8_t digest[32];
  mbedtls_sha256(input, sizeof(input), digest, 0);
  for (int i = 0; i < 8; i++) snprintf(s_device_hash + i * 2, 3, "%02x", digest[i]);
  s_device_hash[16] = 0;
}

static const char *chip_model_str() {
  esp_chip_info_t info;
  esp_chip_info(&info);
  const char *m;
  switch (info.model) {
    case CHIP_ESP32:   m = "ESP32"; break;
    case CHIP_ESP32S2: m = "S2";    break;
    case CHIP_ESP32S3: m = "S3";    break;
    case CHIP_ESP32C3: m = "C3";    break;
    case CHIP_ESP32C6: m = "C6";    break;
    case CHIP_ESP32H2: m = "H2";    break;
    default:           m = "?";     break;
  }
  snprintf(s_chip_buf, sizeof(s_chip_buf), "%sr%d", m, info.revision);
  return s_chip_buf;
}

/* Reads the ESP-IDF core-dump summary (if a dump is pending from the previous
 * panic/WDT), formats an anonymous "task=NAME pc=0xADDR bt=0x..,0x.." line —
 * code addresses + task name only, no stack content — then erases the image so
 * the next panic gets a fresh one. No-op unless coredump-to-flash is enabled. */
static void read_crash_sig() {
  s_crash_sig[0] = 0;
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
  if (esp_core_dump_image_check() != ESP_OK) return;   /* no pending dump */

  esp_core_dump_summary_t *sum =
      (esp_core_dump_summary_t *) heap_caps_malloc(sizeof(*sum), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
    int n = snprintf(s_crash_sig, sizeof(s_crash_sig),
                     "task=%s pc=0x%08" PRIx32,
                     sum->exc_task, (uint32_t) sum->exc_pc);
    for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 6 &&
                         n > 0 && n < (int) sizeof(s_crash_sig) - 14; i++) {
      n += snprintf(s_crash_sig + n, sizeof(s_crash_sig) - n,
                    "%s0x%08" PRIx32, (i == 0) ? " bt=" : ",",
                    (uint32_t) sum->exc_bt_info.bt[i]);
    }
  }
  if (sum) heap_caps_free(sum);
  esp_core_dump_image_erase();
#endif
}

static uint32_t load_and_bump_boot_count() {
  uint32_t bc = 0;
  nvs_handle_t h;
  if (nvs_open(TLM_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_get_u32(h, TLM_NVS_BOOT_CNT, &bc);
    bc++;
    nvs_set_u32(h, TLM_NVS_BOOT_CNT, bc);
    nvs_commit(h);
    nvs_close(h);
  }
  return bc;
}

static esp_err_t do_send(const char *event_type) {
  uint64_t uptime_s = esp_timer_get_time() / 1000000ULL;
  int psram = (esp_psram_get_size() > 0) ? 1 : 0;
  int connected = s_connected.load() ? 1 : 0;
  bool with_crash = (strcmp(event_type, "boot") == 0) && s_crash_sig[0];

  char body[512];
  int n;
  if (with_crash) {
    n = snprintf(body, sizeof(body),
        "{\"dh\":\"%s\",\"v\":\"%s\",\"et\":\"%s\",\"ch\":\"%s\",\"up\":%llu,"
        "\"bc\":%u,\"rr\":%d,\"ps\":%d,\"cn\":%d,\"cr\":\"%s\"}",
        s_device_hash, TAILSCALE_TELEMETRY_VERSION, event_type, chip_model_str(),
        (unsigned long long) uptime_s, (unsigned) s_boot_count,
        (int) esp_reset_reason(), psram, connected, s_crash_sig);
  } else {
    n = snprintf(body, sizeof(body),
        "{\"dh\":\"%s\",\"v\":\"%s\",\"et\":\"%s\",\"ch\":\"%s\",\"up\":%llu,"
        "\"bc\":%u,\"rr\":%d,\"ps\":%d,\"cn\":%d}",
        s_device_hash, TAILSCALE_TELEMETRY_VERSION, event_type, chip_model_str(),
        (unsigned long long) uptime_s, (unsigned) s_boot_count,
        (int) esp_reset_reason(), psram, connected);
  }
  if (n < 0 || n >= (int) sizeof(body)) return ESP_ERR_NO_MEM;

  esp_http_client_config_t cfg = {};
  cfg.url               = TAILSCALE_TELEMETRY_URL;
  cfg.method            = HTTP_METHOD_POST;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms        = 15000;
  cfg.keep_alive_enable = false;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) return ESP_ERR_NO_MEM;
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "X-Tlm-Key", TAILSCALE_TELEMETRY_KEY);
  esp_http_client_set_post_field(client, body, n);

  esp_err_t err = esp_http_client_perform(client);
  int code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    ESP_LOGD(TAG, "send %s failed: %s", event_type, esp_err_to_name(err));
    return err;
  }
  if (code < 200 || code >= 300) {
    ESP_LOGD(TAG, "send %s -> HTTP %d", event_type, code);
    return ESP_FAIL;
  }
  ESP_LOGD(TAG, "send %s -> HTTP %d", event_type, code);
  return ESP_OK;
}

static esp_err_t do_send_with_retry(const char *event_type) {
  esp_err_t err = do_send(event_type);
  for (size_t i = 0; err != ESP_OK && i < TLM_RETRY_BACKOFF_N; i++) {
    vTaskDelay(pdMS_TO_TICKS(TLM_RETRY_BACKOFF_S[i] * 1000));
    err = do_send(event_type);
  }
  return err;
}

static void sender_task(void *arg) {
  (void) arg;
  vTaskDelay(pdMS_TO_TICKS(TLM_FIRST_SEND_DELAY_S * 1000));

  bool first = true;
  while (true) {
    do_send_with_retry(first ? "boot" : "heartbeat");
    first = false;

    /* Sleep ~24 h. Compute the tick count in uint64 — pdMS_TO_TICKS on the
     * 32-bit non-SMP kernel overflows for 86_400_000 ms. */
    TickType_t ticks = (TickType_t) ((uint64_t) TLM_HEARTBEAT_PERIOD_S * 1000ULL /
                                     (uint64_t) portTICK_PERIOD_MS);
    vTaskDelay(ticks);
  }
}

void telemetry_init(bool enabled) {
  if (s_task || !enabled) return;

  compute_device_hash();
  s_boot_count = load_and_bump_boot_count();
  read_crash_sig();

  ESP_LOGI(TAG, "anonymous telemetry on (dh=%s boot=%u%s); disable with "
                "'disable_telemetry: true'",
           s_device_hash, (unsigned) s_boot_count, s_crash_sig[0] ? ", crash pending" : "");

  /* PSRAM stack (TCB stays internal): the task only does HTTPS + NVS reads,
   * never SPI-flash writes, so XIP keeps the cache live. ~8 KB. */
  xTaskCreateWithCaps(sender_task, "tlm", 8192, nullptr, 3, &s_task, MALLOC_CAP_SPIRAM);
}

void telemetry_set_connected(bool connected) {
  s_connected.store(connected);
}

}  // namespace tailscale
}  // namespace esphome
