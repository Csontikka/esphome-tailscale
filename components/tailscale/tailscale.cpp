#include "tailscale.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"

namespace esphome {
namespace tailscale {

static const char *const TAG = "tailscale";

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

  microlink_config_t config = {};
  config.auth_key = this->auth_key_.c_str();
  config.device_name = this->hostname_.empty() ? nullptr : this->hostname_.c_str();
  config.enable_derp = this->enable_derp_;
  config.enable_stun = this->enable_stun_;
  config.enable_disco = this->enable_disco_;
  config.max_peers = this->max_peers_;

  ESP_LOGI(TAG, "Calling microlink_init with auth_key=%s device=%s",
    config.auth_key ? config.auth_key : "NULL",
    config.device_name ? config.device_name : "NULL");
  this->ml_ = microlink_init(&config);
  ESP_LOGI(TAG, "microlink_init returned: %p", (void*)this->ml_);
  if (this->ml_ == nullptr) {
    ESP_LOGE(TAG, "Failed to initialize MicroLink!");
    this->mark_failed();
    return;
  }

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
  if (this->ml_ == nullptr && this->tailscale_user_enabled_ && wifi::global_wifi_component->is_connected()) {
    this->start_microlink_();
  }

  // 60s rollback timers for switches
  if (this->derp_rollback_pending_ && (millis() - this->derp_rollback_ms_ > 60000)) {
    ESP_LOGW(TAG, "DERP rollback: no confirmation in 60s, restoring previous state");
    this->enable_derp_ = this->derp_rollback_value_;
    this->derp_rollback_pending_ = false;
    if (this->ml_ != nullptr) this->request_reconnect();
  }
  if (this->enable_rollback_pending_ && (millis() - this->enable_rollback_ms_ > 60000)) {
    ESP_LOGW(TAG, "Enable rollback: no confirmation in 60s, restoring previous state");
    this->tailscale_user_enabled_ = this->enable_rollback_value_;
    this->enable_rollback_pending_ = false;
    if (!this->tailscale_user_enabled_ && this->ml_ != nullptr) {
      microlink_stop(this->ml_);
      microlink_destroy(this->ml_);
      this->ml_ = nullptr;
    }
    this->state_changed_ = true;
  }

  if (this->state_changed_) {
    this->state_changed_ = false;
    this->publish_state_();
  }

  // Try sending IP notification once HA API is connected
  if (this->ip_notify_pending_) {
    this->send_ip_notification_();
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
          microlink_stop(this->ml_);
          microlink_destroy(this->ml_);
          this->ml_ = nullptr;
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

void TailscaleComponent::update() {
  if (this->ml_ == nullptr)
    return;

  // Force-publish all sensors on polling interval (for web_server SSE)
  this->force_publish_ = true;

  // Periodic log hints every 10 minutes (30s polling * 20 = 10min)
  this->hint_counter_++;
  if (this->hint_counter_ >= 20) {
    this->hint_counter_ = 0;
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

  this->publish_state_();
}

void TailscaleComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Tailscale:");
  ESP_LOGCONFIG(TAG, "  Hostname: %s", this->hostname_.empty() ? "(auto)" : this->hostname_.c_str());
  ESP_LOGCONFIG(TAG, "  DERP: %s", YESNO(this->enable_derp_));
  ESP_LOGCONFIG(TAG, "  STUN: %s", YESNO(this->enable_stun_));
  ESP_LOGCONFIG(TAG, "  DISCO: %s", YESNO(this->enable_disco_));
  ESP_LOGCONFIG(TAG, "  Max Peers: %u", this->max_peers_);
  if (!this->login_server_.empty()) {
    ESP_LOGCONFIG(TAG, "  Login Server: %s", this->login_server_.c_str());
  }
}

void TailscaleComponent::on_shutdown() {
  if (this->ml_ != nullptr) {
    ESP_LOGI(TAG, "Shutting down Tailscale...");
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
  char ip_str[16];
  microlink_ip_to_str(peer->vpn_ip, ip_str);
  ESP_LOGI(TAG, "Peer: %s (%s) online=%d direct=%d",
           peer->hostname, ip_str, peer->online, peer->direct_path);
}

void TailscaleComponent::publish_state_() {
  bool connected = this->is_connected();

  bool force = this->force_publish_;
  this->force_publish_ = false;

#ifdef USE_BINARY_SENSOR
  if (this->connected_sensor_ != nullptr && (force || this->connected_sensor_->state != connected)) {
    this->connected_sensor_->publish_state(connected);
  }
#endif

#ifdef USE_TEXT_SENSOR
  std::string vpn_ip = this->get_vpn_ip();
  if (this->ip_address_sensor_ != nullptr && (force || this->ip_address_sensor_->state != vpn_ip)) {
    this->ip_address_sensor_->publish_state(vpn_ip);
  }
  if (this->hostname_sensor_ != nullptr && (force || this->hostname_sensor_->state != this->hostname_)) {
    this->hostname_sensor_->publish_state(this->hostname_);
  }
  if (this->setup_status_sensor_ != nullptr) {
    std::string hint;
    if (vpn_ip.empty()) {
      hint = "Waiting for Tailscale...";
    } else {
      hint = "wifi use_address: " + vpn_ip;
    }
    if (force || this->setup_status_sensor_->state != hint) {
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
              std::string domain = fqdn.substr(dot);  // .tail13ca7.ts.net
              this->magicdns_sensor_->publish_state(this->hostname_ + domain);
            }
          }
        }
      }
    }
  }
  if (this->auth_key_status_sensor_ != nullptr) {
    std::string key_status = connected ? "Active" : "Unknown";
    if (force || this->auth_key_status_sensor_->state != key_status) {
      this->auth_key_status_sensor_->publish_state(key_status);
    }
  }
  if (this->tailnet_name_sensor_ != nullptr && this->ml_ != nullptr && this->tailnet_name_.empty()) {
    // Extract tailnet name from first peer's FQDN (e.g., "host.tail13ca7.ts.net" -> "tail13ca7.ts.net")
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
    if (force || this->tailnet_name_sensor_->state != this->tailnet_name_) {
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
    if (force || this->peer_list_sensor_->state != list) {
      this->peer_list_sensor_->publish_state(list);
    }
  }
  if (this->memory_mode_sensor_ != nullptr && (force || this->memory_mode_sensor_->state.empty())) {
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
  // Count peers by type
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

  auto pub_sensor = [force](sensor::Sensor *s, float val) {
    if (s != nullptr && (force || s->state != val)) s->publish_state(val);
  };
  pub_sensor(this->peers_total_sensor_, static_cast<float>(total));
  pub_sensor(this->peers_online_sensor_, static_cast<float>(online));
  pub_sensor(this->peers_direct_sensor_, static_cast<float>(direct));
  pub_sensor(this->peers_derp_sensor_, static_cast<float>(derp));
  pub_sensor(this->peers_max_sensor_, static_cast<float>(this->max_peers_));
  if (this->uptime_sensor_ != nullptr) {
    float uptime_s = 0;
    if (this->connected_since_ms_ > 0) {
      uptime_s = static_cast<float>((millis() - this->connected_since_ms_) / 1000);
    }
    if (force || this->uptime_sensor_->state != uptime_s) {
      this->uptime_sensor_->publish_state(uptime_s);
    }
  }
#endif

#ifdef USE_TEXT_SENSOR
  if (this->peer_status_sensor_ != nullptr) {
    std::string status;
    if (online >= this->max_peers_) {
      status = "Full";
    } else if (online >= this->max_peers_ - 2) {
      status = "Warning";
    } else {
      status = "OK";
    }
    if (force || this->peer_status_sensor_->state != status) {
      this->peer_status_sensor_->publish_state(status);
    }
  }
#endif
}

void TailscaleComponent::set_derp_enabled(bool enabled) {
  ESP_LOGI(TAG, "DERP %s requested", enabled ? "enable" : "disable");
  this->derp_rollback_value_ = this->enable_derp_;
  this->enable_derp_ = enabled;
  this->derp_rollback_pending_ = true;
  this->derp_rollback_ms_ = millis();
  // Apply immediately via reconnect
  if (this->ml_ != nullptr) {
    this->request_reconnect();
  }
}

void TailscaleComponent::set_tailscale_enabled(bool enabled) {
  ESP_LOGI(TAG, "Tailscale %s requested", enabled ? "enable" : "disable");
  this->enable_rollback_value_ = this->tailscale_user_enabled_;
  this->tailscale_user_enabled_ = enabled;
  this->enable_rollback_pending_ = true;
  this->enable_rollback_ms_ = millis();
  if (!enabled && this->ml_ != nullptr) {
    ESP_LOGI(TAG, "Stopping Tailscale...");
    microlink_stop(this->ml_);
    microlink_destroy(this->ml_);
    this->ml_ = nullptr;
    this->current_state_ = ML_STATE_IDLE;
    this->state_changed_ = true;
  } else if (enabled && this->ml_ == nullptr) {
    ESP_LOGI(TAG, "Re-enabling Tailscale...");
    // Will start in next loop() iteration
  }
}

void TailscaleComponent::confirm_derp_rollback() {
  if (this->derp_rollback_pending_) {
    ESP_LOGI(TAG, "DERP change confirmed, rollback cancelled");
    this->derp_rollback_pending_ = false;
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
    microlink_stop(this->ml_);
    microlink_destroy(this->ml_);
    this->ml_ = nullptr;
    this->reconnect_phase_ = RECONNECT_FULL;
    this->reconnect_start_ms_ = millis();
  }
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
