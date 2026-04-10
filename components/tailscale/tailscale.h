#pragma once

#include <atomic>
#include "esphome/core/component.h"
#include "esphome/core/log.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

#include "microlink.h"

namespace esphome {
namespace tailscale {

class TailscaleComponent : public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Setters called from codegen
  void set_auth_key(const std::string &key) { this->auth_key_ = key; }
  void set_hostname(const std::string &hostname) { this->hostname_ = hostname; }
  void set_enable_stun(bool enable) { this->enable_stun_ = enable; }
  void set_enable_disco(bool enable) { this->enable_disco_ = enable; }
  void set_max_peers(uint8_t max) { this->max_peers_ = max; }
  void set_login_server(const std::string &server) { this->login_server_ = server; }

#ifdef USE_BINARY_SENSOR
  void set_connected_binary_sensor(binary_sensor::BinarySensor *sensor) {
    this->connected_sensor_ = sensor;
  }
#endif
#ifdef USE_TEXT_SENSOR
  void set_ip_address_text_sensor(text_sensor::TextSensor *sensor) {
    this->ip_address_sensor_ = sensor;
  }
  void set_hostname_text_sensor(text_sensor::TextSensor *sensor) {
    this->hostname_sensor_ = sensor;
  }
  void set_memory_mode_text_sensor(text_sensor::TextSensor *sensor) {
    this->memory_mode_sensor_ = sensor;
  }
  void set_peer_status_text_sensor(text_sensor::TextSensor *sensor) {
    this->peer_status_sensor_ = sensor;
  }
  void set_setup_status_text_sensor(text_sensor::TextSensor *sensor) {
    this->setup_status_sensor_ = sensor;
  }
  void set_magicdns_text_sensor(text_sensor::TextSensor *sensor) {
    this->magicdns_sensor_ = sensor;
  }
  void set_peer_list_text_sensor(text_sensor::TextSensor *sensor) {
    this->peer_list_sensor_ = sensor;
  }
  void set_tailnet_name_text_sensor(text_sensor::TextSensor *sensor) {
    this->tailnet_name_sensor_ = sensor;
  }
  void set_auth_key_status_text_sensor(text_sensor::TextSensor *sensor) {
    this->auth_key_status_sensor_ = sensor;
  }
  void set_auth_key_expiry_text_sensor(text_sensor::TextSensor *sensor) {
    this->auth_key_expiry_sensor_ = sensor;
  }
  void set_ha_connection_route_text_sensor(text_sensor::TextSensor *sensor) {
    this->ha_route_sensor_ = sensor;
  }
  void set_ha_connection_ip_text_sensor(text_sensor::TextSensor *sensor) {
    this->ha_ip_sensor_ = sensor;
  }
#endif
#ifdef USE_SWITCH
  void set_enable_switch(switch_::Switch *sw) { this->enable_switch_ = sw; }
#endif
#ifdef USE_SENSOR
  void set_peers_total_sensor(sensor::Sensor *sensor) { this->peers_total_sensor_ = sensor; }
  void set_peers_online_sensor(sensor::Sensor *sensor) { this->peers_online_sensor_ = sensor; }
  void set_peers_direct_sensor(sensor::Sensor *sensor) { this->peers_direct_sensor_ = sensor; }
  void set_peers_derp_sensor(sensor::Sensor *sensor) { this->peers_derp_sensor_ = sensor; }
  void set_peers_max_sensor(sensor::Sensor *sensor) { this->peers_max_sensor_ = sensor; }
  void set_uptime_sensor(sensor::Sensor *sensor) { this->uptime_sensor_ = sensor; }
#endif

  bool is_connected() const;
  std::string get_vpn_ip() const;
  int get_peer_count() const;
  void request_reconnect();
  void set_tailscale_enabled(bool enabled);
  void confirm_enable_rollback();

 protected:
  // Static callbacks for microlink
  static void state_callback(microlink_t *ml, microlink_state_t state, void *user_data);
  static void peer_callback(microlink_t *ml, const microlink_peer_info_t *peer, void *user_data);

  void publish_state_();
  void start_microlink_();
  void check_ip_config_(const char *vpn_ip);
  void send_ip_notification_();
  std::string detect_ha_route_(std::string *out_ip = nullptr);

  // Config
  std::string auth_key_;
  std::string hostname_;
  bool enable_stun_{true};
  bool enable_disco_{true};
  uint8_t max_peers_{16};
  std::string login_server_;

  // Runtime
  microlink_t *ml_{nullptr};
  std::atomic<microlink_state_t> current_state_{ML_STATE_IDLE};
  std::atomic<bool> state_changed_{false};
  bool psram_available_{false};
  bool ip_notify_pending_{false};
  bool force_publish_{true};
  uint8_t hint_counter_{0};
  std::string vpn_ip_str_;
  std::string tailnet_name_;
  uint32_t connected_since_ms_{0};

  // Reconnect state machine
  enum ReconnectPhase : uint8_t { RECONNECT_IDLE, RECONNECT_REBIND, RECONNECT_FULL, RECONNECT_REBOOT };
  ReconnectPhase reconnect_phase_{RECONNECT_IDLE};
  uint32_t reconnect_start_ms_{0};

  // Switch rollback state (60s dead man's switch)
  bool enable_rollback_pending_{false};
  bool enable_rollback_value_{true};
  uint32_t enable_rollback_ms_{0};
  bool tailscale_user_enabled_{true};

#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *connected_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *ip_address_sensor_{nullptr};
  text_sensor::TextSensor *hostname_sensor_{nullptr};
  text_sensor::TextSensor *memory_mode_sensor_{nullptr};
  text_sensor::TextSensor *peer_status_sensor_{nullptr};
  text_sensor::TextSensor *setup_status_sensor_{nullptr};
  text_sensor::TextSensor *magicdns_sensor_{nullptr};
  text_sensor::TextSensor *peer_list_sensor_{nullptr};
  text_sensor::TextSensor *tailnet_name_sensor_{nullptr};
  text_sensor::TextSensor *auth_key_status_sensor_{nullptr};
  text_sensor::TextSensor *auth_key_expiry_sensor_{nullptr};
  text_sensor::TextSensor *ha_route_sensor_{nullptr};
  text_sensor::TextSensor *ha_ip_sensor_{nullptr};
#endif
#ifdef USE_SENSOR
  sensor::Sensor *peers_total_sensor_{nullptr};
  sensor::Sensor *peers_online_sensor_{nullptr};
  sensor::Sensor *peers_direct_sensor_{nullptr};
  sensor::Sensor *peers_derp_sensor_{nullptr};
  sensor::Sensor *peers_max_sensor_{nullptr};
  sensor::Sensor *uptime_sensor_{nullptr};
#endif
#ifdef USE_SWITCH
  switch_::Switch *enable_switch_{nullptr};
#endif
};

#ifdef USE_BUTTON
class TailscaleReconnectButton : public button::Button, public Component {
 public:
  void set_parent(TailscaleComponent *parent) { this->parent_ = parent; }

 protected:
  void press_action() override { this->parent_->request_reconnect(); }
  TailscaleComponent *parent_{nullptr};
};
#endif

#ifdef USE_SWITCH
class TailscaleEnableSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(TailscaleComponent *parent) { this->parent_ = parent; }
  void setup() override { this->publish_state(true); }

 protected:
  void write_state(bool state) override {
    this->parent_->set_tailscale_enabled(state);
    this->publish_state(state);
  }
  TailscaleComponent *parent_{nullptr};
};
#endif

}  // namespace tailscale
}  // namespace esphome
