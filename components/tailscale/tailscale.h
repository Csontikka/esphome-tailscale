#pragma once

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

// Use stub when microlink library is not available
#if __has_include("microlink.h")
#include "microlink.h"
#else
#include "microlink_stub.h"
#endif

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
  void set_enable_derp(bool enable) { this->enable_derp_ = enable; }
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
#endif
#ifdef USE_SENSOR
  void set_peer_count_sensor(sensor::Sensor *sensor) {
    this->peer_count_sensor_ = sensor;
  }
#endif

  bool is_connected() const;
  std::string get_vpn_ip() const;
  int get_peer_count() const;

 protected:
  // Static callbacks for microlink
  static void state_callback(microlink_t *ml, microlink_state_t state, void *user_data);
  static void peer_callback(microlink_t *ml, const microlink_peer_info_t *peer, void *user_data);

  void publish_state_();

  // Config
  std::string auth_key_;
  std::string hostname_;
  bool enable_derp_{true};
  bool enable_stun_{true};
  bool enable_disco_{true};
  uint8_t max_peers_{16};
  std::string login_server_;

  // Runtime
  microlink_t *ml_{nullptr};
  microlink_state_t current_state_{ML_STATE_IDLE};
  bool state_changed_{false};

#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *connected_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *ip_address_sensor_{nullptr};
  text_sensor::TextSensor *hostname_sensor_{nullptr};
#endif
#ifdef USE_SENSOR
  sensor::Sensor *peer_count_sensor_{nullptr};
#endif
};

}  // namespace tailscale
}  // namespace esphome
