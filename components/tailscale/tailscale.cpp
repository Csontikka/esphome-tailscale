#include "tailscale.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/wifi/wifi_component.h"
#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/ip_addr.h"
#include "lwip/tcpip.h"

namespace esphome {
namespace tailscale {

static const char *const TAG = "tailscale";
static std::atomic<bool> s_vpn_stopping{false};
static std::atomic<microlink_t *> s_active_ml{nullptr};
static std::atomic<bool> s_stop_in_progress{false};

static void microlink_stop_task(void *arg) {
  auto *ml = static_cast<microlink_t *>(arg);
  microlink_stop(ml);
  vTaskDelay(pdMS_TO_TICKS(5000));
  microlink_destroy(ml);
  s_stop_in_progress.store(false);
  ESP_LOGI(TAG, "Microlink cleanup complete");
  vTaskDelete(nullptr);
}

void TailscaleComponent::setup() {
  ESP_LOGI(TAG, "Initializing Tailscale (MicroLink)...");

  // Runtime PSRAM detection
  size_t psram_size = esp_psram_get_size();
  if (psram_size > 0) {
    this->psram_available_ = true;
    ESP_LOGI(TAG, "PSRAM detected: %u KB - using large buffers", (unsigned)(psram_size / 1024));
  } else {
    this->psram_available_ = false;
    ESP_LOGW(TAG, "No PSRAM - using small buffers (max ~30 peers). Add PSRAM for large tailnets.");
  }

  ESP_LOGI(TAG, "Waiting for WiFi before starting...");
}

void TailscaleComponent::start_microlink_() {
  if (this->ml_ != nullptr)
    return;  // Already started
  if (s_stop_in_progress.load()) {
    ESP_LOGD(TAG, "Waiting for previous instance cleanup...");
    return;
  }
  s_vpn_stopping.store(false);

  microlink_config_t config = {};
  config.auth_key = this->auth_key_.c_str();
  config.device_name = this->hostname_.empty() ? nullptr : this->hostname_.c_str();
  config.max_peers = this->max_peers_;
  config.ctrl_host = this->login_server_.empty() ? nullptr : this->login_server_.c_str();

  // Mask the auth key: show only the prefix so "tskey-auth-..." vs "tskey-client-..."
  // is still distinguishable in logs without leaking the secret portion.
  char masked_key[20] = "NULL";
  if (config.auth_key) {
    size_t klen = strlen(config.auth_key);
    if (klen <= 12) {
      snprintf(masked_key, sizeof(masked_key), "(len=%u)", (unsigned)klen);
    } else {
      snprintf(masked_key, sizeof(masked_key), "%.12s...", config.auth_key);
    }
  }
  ESP_LOGI(TAG, "Calling microlink_init with auth_key=%s device=%s ctrl_host=%s",
    masked_key,
    config.device_name ? config.device_name : "NULL",
    config.ctrl_host ? config.ctrl_host : "(tailscale)");
  this->ml_ = microlink_init(&config);
  ESP_LOGI(TAG, "microlink_init returned: %p", (void*)this->ml_);
  if (this->ml_ == nullptr) {
    ESP_LOGE(TAG, "Failed to initialize MicroLink!");
    this->mark_failed();
    return;
  }

  s_active_ml.store(this->ml_);
  microlink_set_state_callback(this->ml_, TailscaleComponent::state_callback, this);
  microlink_set_peer_callback(this->ml_, TailscaleComponent::peer_callback, this);

  esp_err_t err = microlink_start(this->ml_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start MicroLink: %d", err);
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "Tailscale started after WiFi connected!");
}

void TailscaleComponent::loop() {
  // Start microlink only after WiFi is connected (and user hasn't disabled)
  if (this->ml_ == nullptr && wifi::global_wifi_component->is_connected()) {
    if (this->tailscale_user_enabled_ && !this->vpn_stopping_) {
      this->start_microlink_();
    }
  }

  // Auto-confirm rollback after 30s if HA API is still connected (proof HA can see us)
  bool api_alive = false;
#ifdef USE_API
  api_alive = api::global_api_server != nullptr && api::global_api_server->is_connected();
#endif
  if (this->enable_rollback_pending_ && api_alive && (millis() - this->enable_rollback_ms_ > 30000)) {
    ESP_LOGI(TAG, "Tailscale enable change auto-confirmed (HA API still alive after 30s)");
    this->enable_rollback_pending_ = false;
  }

  // 60s rollback timers for switches (HA unreachable = restore)
  if (this->enable_rollback_pending_ && (millis() - this->enable_rollback_ms_ > 60000)) {
    ESP_LOGW(TAG, "Enable rollback: no confirmation in 60s, restoring previous state");
    this->tailscale_user_enabled_ = this->enable_rollback_value_;
    this->enable_rollback_pending_ = false;
#ifdef USE_SWITCH
    if (this->enable_switch_ != nullptr) {
      this->enable_switch_->publish_state(this->enable_rollback_value_);
    }
#endif
    if (!this->tailscale_user_enabled_ && this->ml_ != nullptr) {
      s_active_ml.store(nullptr);
      microlink_set_state_callback(this->ml_, nullptr, nullptr);
      microlink_set_peer_callback(this->ml_, nullptr, nullptr);
      microlink_t *old_ml = this->ml_;
      this->ml_ = nullptr;
      s_stop_in_progress.store(true);
      xTaskCreatePinnedToCore(microlink_stop_task, "ml_stop", 4096, old_ml, 1, nullptr, 0);
    }
    this->state_changed_ = true;
  }

  if (this->state_changed_) {
    this->state_changed_ = false;
    this->publish_state_();
  }

  // Periodic sensor refresh (uptime, peer counts, HA route)
  if (this->ml_ != nullptr && this->is_connected()) {
    uint32_t now = millis();
    if (now - this->last_sensor_publish_ms_ >= 5000) {
      this->last_sensor_publish_ms_ = now;
      this->publish_state_();
    }
  }

  // Try sending IP notification once HA API is connected
  if (this->ip_notify_pending_) {
    this->send_ip_notification_();
  }

  // Periodic diagnostic logs every 10 minutes (hints, peer warnings, status summary)
  if (this->ml_ != nullptr) {
    constexpr uint32_t HINT_INTERVAL_MS = 600000;  // 10 minutes
    uint32_t now_ms = millis();
    if (now_ms - this->last_hint_ms_ >= HINT_INTERVAL_MS) {
      this->last_hint_ms_ = now_ms;
      if (!this->vpn_ip_str_.empty()) {
        ESP_LOGI(TAG, "Hint: set 'wifi: use_address: \"%s\"' in your ESPHome YAML if device is not visible in Builder",
                 this->vpn_ip_str_.c_str());
      }
      // Peer capacity warnings
      int online = 0, direct = 0, relay = 0;
      int total = this->get_peer_count();
      for (int i = 0; i < total; i++) {
        microlink_peer_info_t info;
        if (microlink_get_peer_info(this->ml_, i, &info) == ESP_OK && info.online) {
          online++;
          if (info.direct_path) direct++; else relay++;
        }
      }
      if (online >= this->max_peers_) {
        ESP_LOGW(TAG, "Peer limit FULL: %d/%d online peers. Increase max_peers or remove unused peers from your tailnet.",
                 online, (int)this->max_peers_);
      } else if (online >= this->max_peers_ - 2) {
        ESP_LOGW(TAG, "Peer limit WARNING: %d/%d online peers. Approaching max_peers limit.",
                 online, (int)this->max_peers_);
      }
      // Periodic status summary
      ESP_LOGI(TAG, "Status: %s | peers %d/%d (direct=%d relay=%d) | heap %uKB | PSRAM %uKB | uptime %us",
               this->is_connected() ? "connected" : "disconnected",
               online, (int)this->max_peers_, direct, relay,
               (unsigned)(esp_get_free_heap_size() / 1024),
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
               (unsigned)(millis() / 1000));
    }
  }

  // Reconnect state machine
  if (this->reconnect_phase_ != RECONNECT_IDLE && this->ml_ != nullptr) {
    uint32_t elapsed = millis() - this->reconnect_start_ms_;
    switch (this->reconnect_phase_) {
      case RECONNECT_REBIND:
        if (this->is_connected()) {
          ESP_LOGI(TAG, "Reconnect: rebind succeeded");
          this->reconnect_phase_ = RECONNECT_IDLE;
        } else if (elapsed > 15000) {
          ESP_LOGW(TAG, "Reconnect: rebind failed after 15s, trying full restart...");
          s_active_ml.store(nullptr);
          microlink_set_state_callback(this->ml_, nullptr, nullptr);
          microlink_set_peer_callback(this->ml_, nullptr, nullptr);
          microlink_t *old_ml = this->ml_;
          this->ml_ = nullptr;
          s_stop_in_progress.store(true);
          xTaskCreatePinnedToCore(microlink_stop_task, "ml_stop", 4096, old_ml, 1, nullptr, 0);
          this->reconnect_phase_ = RECONNECT_FULL;
          this->reconnect_start_ms_ = millis();
          // Will re-init in next loop() via start_microlink_()
        }
        break;
      case RECONNECT_FULL:
        if (this->is_connected()) {
          ESP_LOGI(TAG, "Reconnect: full restart succeeded");
          this->reconnect_phase_ = RECONNECT_IDLE;
        } else if (elapsed > 30000) {
          ESP_LOGE(TAG, "Reconnect: full restart failed after 30s, rebooting device...");
          this->reconnect_phase_ = RECONNECT_REBOOT;
          App.safe_reboot();
        }
        break;
      default:
        break;
    }
  }
}

void TailscaleComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Tailscale:");
  ESP_LOGCONFIG(TAG, "  Hostname: %s", this->hostname_.empty() ? "(auto)" : this->hostname_.c_str());
  ESP_LOGCONFIG(TAG, "  Max Peers: %u", this->max_peers_);
  if (!this->login_server_.empty()) {
    ESP_LOGCONFIG(TAG, "  Login Server: %s", this->login_server_.c_str());
  }
}

void TailscaleComponent::on_shutdown() {
  if (this->ml_ != nullptr) {
    ESP_LOGI(TAG, "Shutting down Tailscale...");
    s_active_ml.store(nullptr);
    s_vpn_stopping.store(true);
    microlink_stop(this->ml_);
    microlink_destroy(this->ml_);
    this->ml_ = nullptr;
  }
}

bool TailscaleComponent::is_connected() const {
  return this->current_state_ == ML_STATE_CONNECTED;
}

std::string TailscaleComponent::get_vpn_ip() const {
  if (this->ml_ == nullptr || !this->is_connected())
    return "";
  uint32_t ip = microlink_get_vpn_ip(this->ml_);
  if (ip == 0)
    return "";
  char buf[16];
  microlink_ip_to_str(ip, buf);
  return std::string(buf);
}

int TailscaleComponent::get_peer_count() const {
  if (this->ml_ == nullptr)
    return 0;
  return microlink_get_peer_count(this->ml_);
}

void TailscaleComponent::state_callback(microlink_t *ml, microlink_state_t state, void *user_data) {
  if (ml != s_active_ml.load() || s_vpn_stopping.load()) return;
  auto *self = static_cast<TailscaleComponent *>(user_data);
  self->current_state_ = state;
  self->state_changed_ = true;

  static const char *state_names[] = {
      "IDLE", "WIFI_WAIT", "CONNECTING", "REGISTERING",
      "CONNECTED", "RECONNECTING", "ERROR"};
  int idx = static_cast<int>(state);
  const char *name = (idx >= 0 && idx < 7) ? state_names[idx] : "UNKNOWN";
  ESP_LOGI(TAG, "State: %s", name);

  if (state == ML_STATE_CONNECTED) {
    self->connection_count_++;
    self->connected_since_ms_ = millis();
    uint32_t ip = microlink_get_vpn_ip(ml);
    char ip_str[16];
    microlink_ip_to_str(ip, ip_str);
    ESP_LOGI(TAG, "Connected! VPN IP: %s", ip_str);

    // Check if use_address needs updating (init mode or IP mismatch)
    self->check_ip_config_(ip_str);
  } else if (state != ML_STATE_CONNECTED) {
    self->connected_since_ms_ = 0;
    self->tailnet_name_.clear();
  }
}

void TailscaleComponent::peer_callback(microlink_t *ml, const microlink_peer_info_t *peer,
                                        void *user_data) {
  if (ml != s_active_ml.load()) return;
  char ip_str[16];
  microlink_ip_to_str(peer->vpn_ip, ip_str);
  ESP_LOGI(TAG, "Peer: %s (%s) online=%d direct=%d",
           peer->hostname, ip_str, peer->online, peer->direct_path);
}

void TailscaleComponent::publish_state_() {
  bool connected = this->is_connected();

#ifdef USE_BINARY_SENSOR
  if (this->connected_sensor_ != nullptr && this->connected_sensor_->state != connected) {
    this->connected_sensor_->publish_state(connected);
  }
  if (!connected && this->key_expiry_warning_sensor_ != nullptr && this->key_expiry_warning_sensor_->has_state()) {
    this->key_expiry_warning_sensor_->invalidate_state();
  }
  {
    bool api_alive = false;
#ifdef USE_API
    api_alive = api::global_api_server != nullptr && api::global_api_server->is_connected();
    if (api_alive && this->vpn_stopping_) {
      std::string route = this->detect_ha_route_();
      if (route.find("Tailscale") != std::string::npos) {
        api_alive = false;
      }
    }
#endif
    if (this->ha_connected_sensor_ != nullptr && this->ha_connected_sensor_->state != api_alive) {
      this->ha_connected_sensor_->publish_state(api_alive);
    }
    if (this->vpn_auto_rollback_sensor_ != nullptr) {
      bool would_rollback = this->enable_rollback_pending_;
      if (!would_rollback && api_alive && connected) {
        std::string route = this->detect_ha_route_();
        would_rollback = route.find("Tailscale") != std::string::npos;
      }
      if (this->vpn_auto_rollback_sensor_->state != would_rollback) {
        this->vpn_auto_rollback_sensor_->publish_state(would_rollback);
      }
    }
  }
#endif

#ifdef USE_TEXT_SENSOR
  // Clear dynamic sensors when disconnected — publish empty + reset has_state_
  // so HA shows "Unknown" instead of blank.
  if (!connected) {
    auto unknown_text = [](text_sensor::TextSensor *s) {
      if (s == nullptr || !s->has_state()) return;
      s->publish_state("");
      s->set_has_state(false);
    };
    unknown_text(this->ip_address_sensor_);
    unknown_text(this->hostname_sensor_);
    unknown_text(this->magicdns_sensor_);
    unknown_text(this->tailnet_name_sensor_);
    unknown_text(this->key_expiry_sensor_);
    unknown_text(this->peer_list_sensor_);
    unknown_text(this->peer_status_sensor_);
    if (this->setup_status_sensor_ != nullptr &&
        this->setup_status_sensor_->state != "Waiting for VPN...") {
      this->setup_status_sensor_->publish_state("Waiting for VPN...");
    }
    this->tailnet_name_.clear();
  } else {
  std::string vpn_ip = this->get_vpn_ip();
  if (this->ip_address_sensor_ != nullptr && this->ip_address_sensor_->state != vpn_ip) {
    this->ip_address_sensor_->publish_state(vpn_ip);
  }
  if (this->hostname_sensor_ != nullptr && this->hostname_sensor_->state != this->hostname_) {
    this->hostname_sensor_->publish_state(this->hostname_);
  }
  if (this->setup_status_sensor_ != nullptr) {
    std::string hint;
    if (vpn_ip.empty()) {
      hint = "Waiting for VPN...";
    } else {
      hint = "wifi use_address: " + vpn_ip;
    }
    if (this->setup_status_sensor_->state != hint) {
      this->setup_status_sensor_->publish_state(hint);
    }
  }
  if (this->magicdns_sensor_ != nullptr && this->ml_ != nullptr) {
    // MagicDNS: find our own entry by matching VPN IP
    uint32_t our_ip = microlink_get_vpn_ip(this->ml_);
    if (our_ip != 0) {
      int count = microlink_get_peer_count(this->ml_);
      for (int i = 0; i < count; i++) {
        microlink_peer_info_t info;
        if (microlink_get_peer_info(this->ml_, i, &info) == ESP_OK) {
          if (info.vpn_ip == our_ip) {
            this->magicdns_sensor_->publish_state(std::string(info.hostname));
            break;
          }
        }
      }
      // If not found in peer list, construct from hostname + tailnet
      if (this->magicdns_sensor_->state.empty() || this->magicdns_sensor_->state == "unknown") {
        // Try to get tailnet domain from any peer's FQDN
        if (count > 0) {
          microlink_peer_info_t info;
          if (microlink_get_peer_info(this->ml_, 0, &info) == ESP_OK) {
            std::string fqdn(info.hostname);
            auto dot = fqdn.find('.');
            if (dot != std::string::npos) {
              std::string domain = fqdn.substr(dot);  // .tailXXXXX.ts.net
              this->magicdns_sensor_->publish_state(this->hostname_ + domain);
            }
          }
        }
      }
    }
  }
  if ((this->key_expiry_sensor_ != nullptr ||
#ifdef USE_BINARY_SENSOR
       this->key_expiry_warning_sensor_ != nullptr ||
#endif
       false) &&
      this->ml_ != nullptr) {
    // The "key expiry" we get from microlink is the NODE key expiry, parsed from
    // the Tailscale control plane's MapResponse.KeyExpiry field. When the user
    // clicks "Disable key expiry" in the Tailscale admin, the control plane sends
    // "0001-01-01T00:00:00Z" (Go's zero time), which the microlink parser turns
    // into a near-zero epoch. We treat anything below a sane baseline as "disabled".
    constexpr int64_t SANE_EPOCH_BASELINE = 1577836800;  // 2020-01-01 UTC
    int64_t expiry = microlink_get_key_expiry(this->ml_);
    bool expiry_enabled = (expiry > SANE_EPOCH_BASELINE);

    // Binary sensor: problem = expiry is enabled (device will eventually drop
    // off the tailnet). OFF = expiry disabled (the recommended safe state).
    bool key_problem = expiry_enabled;
#ifdef USE_BINARY_SENSOR
    if (this->key_expiry_warning_sensor_ != nullptr &&
        this->key_expiry_warning_sensor_->state != key_problem) {
      this->key_expiry_warning_sensor_->publish_state(key_problem);
    }
#endif

    // Text sensor (timestamp device class): ISO 8601 UTC when enabled, empty
    // string when disabled (so HA renders "unknown" which is the correct state
    // for a timestamp that doesn't exist).
    std::string key_expiry_iso;
    if (expiry_enabled) {
      struct tm tm_exp;
      time_t exp_t = (time_t)expiry;
      gmtime_r(&exp_t, &tm_exp);
      char iso_buf[32];
      strftime(iso_buf, sizeof(iso_buf), "%Y-%m-%dT%H:%M:%S+00:00", &tm_exp);
      key_expiry_iso = iso_buf;
    }
    if (this->key_expiry_sensor_ != nullptr &&
        this->key_expiry_sensor_->state != key_expiry_iso) {
      this->key_expiry_sensor_->publish_state(key_expiry_iso);
    }
  }
  if (this->tailnet_name_sensor_ != nullptr && this->ml_ != nullptr && this->tailnet_name_.empty()) {
    // Extract tailnet name from first peer's FQDN (e.g., "host.tailXXXXX.ts.net" -> "tailXXXXX.ts.net")
    int count = microlink_get_peer_count(this->ml_);
    for (int i = 0; i < count; i++) {
      microlink_peer_info_t info;
      if (microlink_get_peer_info(this->ml_, i, &info) == ESP_OK) {
        std::string fqdn(info.hostname);
        auto dot = fqdn.find('.');
        if (dot != std::string::npos) {
          this->tailnet_name_ = fqdn.substr(dot + 1);
          break;
        }
      }
    }
  }
  if (this->tailnet_name_sensor_ != nullptr && !this->tailnet_name_.empty()) {
    if (this->tailnet_name_sensor_->state != this->tailnet_name_) {
      this->tailnet_name_sensor_->publish_state(this->tailnet_name_);
    }
  }
  if (this->peer_list_sensor_ != nullptr && this->ml_ != nullptr) {
    int count = microlink_get_peer_count(this->ml_);
    // Compact format: "name(ip)D|name(ip)R|..." D=direct R=relay
    // Max ~250 chars to fit text_sensor limit
    std::string list;
    for (int i = 0; i < count; i++) {
      microlink_peer_info_t info;
      if (microlink_get_peer_info(this->ml_, i, &info) == ESP_OK && info.online) {
        std::string name(info.hostname);
        auto dot = name.find('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        char ip_str[16];
        microlink_ip_to_str(info.vpn_ip, ip_str);
        std::string entry = name + "(" + ip_str + ")" + (info.direct_path ? "D" : "R");
        if (!list.empty()) list += "|";
        if (list.size() + entry.size() > 240) {
          list += "...";
          break;
        }
        list += entry;
      }
    }
    if (this->peer_list_sensor_->state != list) {
      this->peer_list_sensor_->publish_state(list);
    }
  }
  }  // else (connected)
  if (this->ha_route_sensor_ != nullptr || this->ha_ip_sensor_ != nullptr) {
    std::string ha_ip;
    std::string route;
    if (!this->vpn_stopping_) {
      route = this->detect_ha_route_(&ha_ip);
    }
    if (this->ha_route_sensor_ != nullptr &&
        this->ha_route_sensor_->state != route) {
      this->ha_route_sensor_->publish_state(route);
      if (route.empty()) this->ha_route_sensor_->set_has_state(false);
    }
    if (this->ha_ip_sensor_ != nullptr &&
        this->ha_ip_sensor_->state != ha_ip) {
      this->ha_ip_sensor_->publish_state(ha_ip);
      if (ha_ip.empty()) this->ha_ip_sensor_->set_has_state(false);
    }
  }
  if (this->control_plane_sensor_ != nullptr) {
    std::string cp;
    if (this->login_server_.empty() || this->login_server_.find("tailscale.com") != std::string::npos) {
      cp = "Tailscale";
    } else {
      cp = "Headscale";
    }
    if (this->control_plane_sensor_->state != cp) {
      this->control_plane_sensor_->publish_state(cp);
    }
  }
  if (this->login_server_sensor_ != nullptr) {
    std::string ls = this->login_server_.empty() ? "https://controlplane.tailscale.com" : this->login_server_;
    if (this->login_server_sensor_->state != ls) {
      this->login_server_sensor_->publish_state(ls);
    }
  }
  if (this->memory_mode_sensor_ != nullptr && this->memory_mode_sensor_->state.empty()) {
    // Memory mode never changes - publish once
    size_t psram = esp_psram_get_size();
    if (psram > 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), "PSRAM %uKB", (unsigned)(psram / 1024));
      this->memory_mode_sensor_->publish_state(buf);
    } else {
      this->memory_mode_sensor_->publish_state("Internal RAM");
    }
  }
#endif

#ifdef USE_SENSOR
  auto pub_sensor = [](sensor::Sensor *s, float val) {
    if (s == nullptr) return;
    if (std::isnan(val) && std::isnan(s->state)) return;
    if (s->state != val) s->publish_state(val);
  };
  if (connected) {
    int total = this->get_peer_count();
    int online = 0, direct = 0, derp = 0;
    if (this->ml_ != nullptr) {
      for (int i = 0; i < total; i++) {
        microlink_peer_info_t info;
        if (microlink_get_peer_info(this->ml_, i, &info) == ESP_OK && info.online) {
          online++;
          if (info.direct_path) direct++; else derp++;
        }
      }
    }
    pub_sensor(this->peers_total_sensor_, static_cast<float>(total));
    pub_sensor(this->peers_online_sensor_, static_cast<float>(online));
    pub_sensor(this->peers_direct_sensor_, static_cast<float>(direct));
    pub_sensor(this->peers_derp_sensor_, static_cast<float>(derp));
    float uptime_s = 0;
    if (this->connected_since_ms_ > 0) {
      uptime_s = static_cast<float>((millis() - this->connected_since_ms_) / 1000);
    }
    pub_sensor(this->uptime_sensor_, uptime_s);
  } else {
    float nan = NAN;
    pub_sensor(this->peers_total_sensor_, nan);
    pub_sensor(this->peers_online_sensor_, nan);
    pub_sensor(this->peers_direct_sensor_, nan);
    pub_sensor(this->peers_derp_sensor_, nan);
    pub_sensor(this->uptime_sensor_, nan);
  }
  pub_sensor(this->peers_max_sensor_, static_cast<float>(this->max_peers_));
  pub_sensor(this->connections_sensor_, static_cast<float>(this->connection_count_));
#endif

#ifdef USE_TEXT_SENSOR
  if (this->peer_status_sensor_ != nullptr) {
    if (connected) {
      int online = 0;
      if (this->ml_ != nullptr) {
        int total = this->get_peer_count();
        for (int i = 0; i < total; i++) {
          microlink_peer_info_t info;
          if (microlink_get_peer_info(this->ml_, i, &info) == ESP_OK && info.online) online++;
        }
      }
      std::string status;
      if (online >= this->max_peers_) {
        status = "Full";
      } else if (online >= this->max_peers_ - 2) {
        status = "Warning";
      } else {
        status = "OK";
      }
      if (this->peer_status_sensor_->state != status) {
        this->peer_status_sensor_->publish_state(status);
      }
    } else if (!this->peer_status_sensor_->state.empty()) {
      this->peer_status_sensor_->publish_state("");
    }
  }
#endif
}

void TailscaleComponent::set_tailscale_enabled(bool enabled) {
  ESP_LOGI(TAG, "Tailscale %s requested", enabled ? "enable" : "disable");
  this->enable_rollback_value_ = this->tailscale_user_enabled_;
  this->tailscale_user_enabled_ = enabled;
  bool api_was_connected = false;
#ifdef USE_API
  api_was_connected = api::global_api_server != nullptr && api::global_api_server->is_connected();
#endif
  if (api_was_connected) {
    this->enable_rollback_pending_ = true;
    this->enable_rollback_ms_ = millis();
    ESP_LOGI(TAG, "HA API connected — arming 60s rollback safety");
  } else {
    this->enable_rollback_pending_ = false;
    ESP_LOGI(TAG, "No HA API connection — no rollback armed");
  }
  if (!enabled && this->ml_ != nullptr) {
    this->vpn_stopping_ = true;
    s_vpn_stopping.store(true);
    this->current_state_ = ML_STATE_IDLE;
    this->connected_since_ms_ = 0;
    this->publish_state_();
    ESP_LOGI(TAG, "Stopping Tailscale in 300ms...");
    // Deferred teardown so the switch state change can flush through the API first
    // (otherwise HA sees the TCP tear down before the state update arrives and its
    //  optimistic UI snaps the switch back to the previous value).
    this->set_timeout("tailscale_stop", 300, [this]() {
      if (this->ml_ != nullptr) {
        ESP_LOGI(TAG, "Stopping Tailscale now");
        s_active_ml.store(nullptr);
        microlink_set_state_callback(this->ml_, nullptr, nullptr);
        microlink_set_peer_callback(this->ml_, nullptr, nullptr);
        microlink_t *old_ml = this->ml_;
        this->ml_ = nullptr;
        this->vpn_stopping_ = false;
        this->current_state_ = ML_STATE_IDLE;
        this->connected_since_ms_ = 0;
        this->state_changed_ = true;
        s_stop_in_progress.store(true);
        xTaskCreatePinnedToCore(microlink_stop_task, "ml_stop", 4096, old_ml, 1, nullptr, 0);
      }
    });
  } else if (enabled && this->ml_ == nullptr) {
    this->vpn_stopping_ = false;
    s_vpn_stopping.store(false);
    ESP_LOGI(TAG, "Re-enabling Tailscale...");
    // Will start in next loop() iteration
  }
}

void TailscaleComponent::confirm_enable_rollback() {
  if (this->enable_rollback_pending_) {
    ESP_LOGI(TAG, "Tailscale enable/disable change confirmed, rollback cancelled");
    this->enable_rollback_pending_ = false;
  }
}

void TailscaleComponent::request_reconnect() {
  if (this->reconnect_phase_ != RECONNECT_IDLE) {
    ESP_LOGW(TAG, "Reconnect already in progress (phase %d)", this->reconnect_phase_);
    return;
  }
  if (this->ml_ == nullptr) {
    ESP_LOGW(TAG, "Cannot reconnect: microlink not initialized");
    return;
  }
  ESP_LOGI(TAG, "Reconnect requested: starting phase 1 (rebind)...");
  this->reconnect_phase_ = RECONNECT_REBIND;
  this->reconnect_start_ms_ = millis();
  esp_err_t err = microlink_rebind(this->ml_);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Rebind call failed (%d), escalating to full restart...", err);
    s_active_ml.store(nullptr);
    microlink_set_state_callback(this->ml_, nullptr, nullptr);
    microlink_set_peer_callback(this->ml_, nullptr, nullptr);
    microlink_t *old_ml = this->ml_;
    this->ml_ = nullptr;
    s_stop_in_progress.store(true);
    xTaskCreatePinnedToCore(microlink_stop_task, "ml_stop", 4096, old_ml, 1, nullptr, 0);
    this->reconnect_phase_ = RECONNECT_FULL;
    this->reconnect_start_ms_ = millis();
  }
}

std::string TailscaleComponent::detect_ha_route_(std::string *out_ip) {
  if (out_ip != nullptr) out_ip->clear();
#ifdef USE_API
  if (api::global_api_server == nullptr || !api::global_api_server->is_connected()) {
    return "";
  }
  uint16_t api_port = api::global_api_server->get_port();
  // Snapshot active TCP pcbs into a small local array under LOCK_TCPIP_CORE —
  // tcp_active_pcbs is mutated by tcpip_thread, so walking it from loopTask
  // without the core lock races with the stack and has caused
  // `sys_mutex_unlock: failed to give the mutex` asserts in netconn writes
  // (heap/pcb corruption making a later lwip_write land on a stale mutex).
  struct pcb_snapshot {
    uint8_t b0, b1, b2, b3;
  };
  constexpr size_t kMaxSnap = 8;
  pcb_snapshot snap[kMaxSnap];
  size_t snap_n = 0;
  LOCK_TCPIP_CORE();
  for (struct tcp_pcb *pcb = tcp_active_pcbs; pcb != nullptr && snap_n < kMaxSnap;
       pcb = pcb->next) {
    if (pcb->local_port != api_port) continue;
    if (pcb->state != ESTABLISHED) continue;
    if (!IP_IS_V4_VAL(pcb->remote_ip)) continue;
    uint32_t remote = ip_addr_get_ip4_u32(&pcb->remote_ip);  // network order
    snap[snap_n++] = {
        (uint8_t)(remote & 0xFF),
        (uint8_t)((remote >> 8) & 0xFF),
        (uint8_t)((remote >> 16) & 0xFF),
        (uint8_t)((remote >> 24) & 0xFF),
    };
  }
  UNLOCK_TCPIP_CORE();

  // Classify + look up peers OUTSIDE the core lock — microlink_get_peer_info
  // is not bounded-latency and must not be called while holding tcpip.
  std::string route;
  std::string all_ips;
  bool ts_pcb_found = false;
  std::string ts_route;
  std::string nonts_route;
  for (size_t i = 0; i < snap_n; ++i) {
    uint8_t b0 = snap[i].b0, b1 = snap[i].b1, b2 = snap[i].b2, b3 = snap[i].b3;
    uint32_t ml_ip = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) |
                     ((uint32_t)b2 << 8) | (uint32_t)b3;
    char ip_buf[16];
    snprintf(ip_buf, sizeof(ip_buf), "%u.%u.%u.%u", b0, b1, b2, b3);
    if (!all_ips.empty()) all_ips += ", ";
    all_ips += ip_buf;
    bool in_tailscale = (b0 == 100) && (b1 >= 64) && (b1 <= 127);
    if (in_tailscale) {
      ts_pcb_found = true;
      if (this->ml_ != nullptr) {
        int count = microlink_get_peer_count(this->ml_);
        bool found = false;
        for (int pi = 0; pi < count; pi++) {
          microlink_peer_info_t info;
          if (microlink_get_peer_info(this->ml_, pi, &info) == ESP_OK) {
            if (info.vpn_ip == ml_ip) {
              ts_route = info.direct_path ? "Tailscale Direct" : "Tailscale DERP";
              found = true;
              break;
            }
          }
        }
        if (!found) ts_route = "Tailscale (unknown)";
      } else {
        ts_route = "Tailscale (unknown)";
      }
    } else if (nonts_route.empty()) {
      nonts_route = "Local";
    }
  }
  // Prefer Tailscale route if any TS pcb was found (that's the "real" HA Core path
  // when user configured via 100.x Tailscale IP). Otherwise fall back.
  if (ts_pcb_found) {
    route = ts_route;
  } else if (!nonts_route.empty()) {
    route = nonts_route;
  }
  if (out_ip != nullptr) *out_ip = all_ips;
  return route;
#else
  return "";
#endif
}

void TailscaleComponent::check_ip_config_(const char *vpn_ip) {
  this->vpn_ip_str_ = vpn_ip;
  this->ip_notify_pending_ = true;
  ESP_LOGI(TAG, "Set wifi use_address: \"%s\" in your ESPHome YAML", vpn_ip);
}

void TailscaleComponent::send_ip_notification_() {
  if (!this->ip_notify_pending_ || this->vpn_ip_str_.empty())
    return;
  // Notification handled by publishing to the IP text sensor
  // The HA automation in the package watches for changes
  this->ip_notify_pending_ = false;
}

}  // namespace tailscale
}  // namespace esphome
