/**
 * Stub header for microlink API - allows ESPHome component to compile
 * without the full microlink library. Replace with real microlink.h
 * when integrating the actual library.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct microlink_s microlink_t;

typedef struct {
    const char *auth_key;
    const char *device_name;
    bool enable_derp;
    bool enable_stun;
    bool enable_disco;
    uint8_t max_peers;
    int8_t wifi_tx_power_dbm;
    uint32_t priority_peer_ip;
    uint32_t disco_heartbeat_ms;
    uint32_t stun_interval_ms;
    uint32_t ctrl_watchdog_ms;
} microlink_config_t;

typedef struct {
    uint32_t vpn_ip;
    char hostname[64];
    uint8_t public_key[32];
    bool online;
    bool direct_path;
} microlink_peer_info_t;

typedef enum {
    ML_STATE_IDLE = 0,
    ML_STATE_WIFI_WAIT,
    ML_STATE_CONNECTING,
    ML_STATE_REGISTERING,
    ML_STATE_CONNECTED,
    ML_STATE_RECONNECTING,
    ML_STATE_ERROR,
} microlink_state_t;

typedef void (*microlink_state_cb_t)(microlink_t *ml, microlink_state_t state, void *user_data);
typedef void (*microlink_peer_cb_t)(microlink_t *ml, const microlink_peer_info_t *peer, void *user_data);

// Stub implementations - these will be replaced by the real microlink library
static inline microlink_t *microlink_init(const microlink_config_t *config) { return (microlink_t*)1; }
static inline int microlink_start(microlink_t *ml) { return 0; }
static inline int microlink_stop(microlink_t *ml) { return 0; }
static inline void microlink_destroy(microlink_t *ml) {}
static inline microlink_state_t microlink_get_state(const microlink_t *ml) { return ML_STATE_IDLE; }
static inline bool microlink_is_connected(const microlink_t *ml) { return false; }
static inline uint32_t microlink_get_vpn_ip(const microlink_t *ml) { return 0; }
static inline int microlink_get_peer_count(const microlink_t *ml) { return 0; }
static inline int microlink_get_peer_info(const microlink_t *ml, int index, microlink_peer_info_t *info) { return -1; }
static inline void microlink_set_state_callback(microlink_t *ml, microlink_state_cb_t cb, void *user_data) {}
static inline void microlink_set_peer_callback(microlink_t *ml, microlink_peer_cb_t cb, void *user_data) {}
static inline void microlink_ip_to_str(uint32_t ip, char *buf) { buf[0] = '0'; buf[1] = 0; }

#ifdef __cplusplus
}
#endif
