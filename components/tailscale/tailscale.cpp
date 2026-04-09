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
  // Start microlink only after WiFi is connected
  if (this->ml_ == nullptr && wifi::global_wifi_component->is_connected()) {
    this->start_microlink_();
  }

  if (this->state_changed_) {
    this->state_changed_ = false;
    this->publish_state_();
  }

  // Try sending IP notification once HA API is connected
  if (this->ip_notify_pending_) {
    this->send_ip_notification_();
  }
}

void TailscaleComponent::update() {
  if (this->ml_ == nullptr)
    return;

  // Force-publish all sensors on polling interval (for web_server SSE)
  this->force_publish_ = true;

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
    uint32_t ip = microlink_get_vpn_ip(ml);
    char ip_str[16];
    microlink_ip_to_str(ip, ip_str);
    ESP_LOGI(TAG, "Connected! VPN IP: %s", ip_str);

    // Check if use_address needs updating (init mode or IP mismatch)
    self->check_ip_config_(ip_str);
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
    std::string status;
    if (vpn_ip.empty()) {
      status = "Waiting for Tailscale...";
    } else if (this->configured_ip_ == "init" || this->configured_ip_.empty()) {
      status = "Set var_tailscale_ip to: " + vpn_ip;
    } else if (this->configured_ip_ != vpn_ip) {
      status = "IP mismatch! Change " + this->configured_ip_ + " to " + vpn_ip;
    } else {
      status = "OK";
    }
    if (force || this->setup_status_sensor_->state != status) {
      this->setup_status_sensor_->publish_state(status);
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
  if (this->peer_count_sensor_ != nullptr) {
    float count = static_cast<float>(this->get_peer_count());
    if (force || this->peer_count_sensor_->state != count) {
      this->peer_count_sensor_->publish_state(count);
    }
  }
#endif
}

void TailscaleComponent::check_ip_config_(const char *vpn_ip) {
  this->vpn_ip_str_ = vpn_ip;
  this->ip_notify_pending_ = true;
  if (this->configured_ip_ == "init" || this->configured_ip_.empty()) {
    ESP_LOGW(TAG, "Set var_tailscale_ip: \"%s\" in your ESPHome config (currently 'init')", vpn_ip);
  } else if (this->configured_ip_ != std::string(vpn_ip)) {
    ESP_LOGW(TAG, "IP mismatch! Change var_tailscale_ip from \"%s\" to \"%s\"", this->configured_ip_.c_str(), vpn_ip);
  }
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
