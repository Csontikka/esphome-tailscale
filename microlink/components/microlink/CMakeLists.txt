set(SRCS
    "src/microlink.c"
    "src/ml_coord.c"
    "src/ml_derp.c"
    "src/ml_net_io.c"
    "src/ml_wg_mgr.c"
    "src/ml_stun.c"
    "src/ml_netcheck.c"
    "src/ml_noise.c"
    "src/ml_h2.c"
    "src/ml_udp.c"
    "src/ml_tcp.c"
    "src/ml_peer_nvs.c"
    "src/nacl_box.c"
    "src/ml_x25519.c"
    "src/ml_zerocopy.c"
    "src/ml_cellular.c"
    "src/ml_at_socket.c"
    "src/ml_net_switch.c"
    "src/ml_config_httpd.c"
)

set(REQUIRES
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_timer
    lwip
    mbedtls
    json
    driver
    esp_driver_tsens
    esp_http_server
    esp-tls
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES ${REQUIRES}
    PRIV_REQUIRES
        wireguard_lwip
)

# Add esp-tls include path so we can #include "esp_tls.h" directly.
# PlatformIO does not propagate REQUIRES include dirs the way native IDF cmake does.
# The cmake variable for the IDF root is esp-idf_SOURCE_DIR (set by PlatformIO).
target_include_directories(__idf_microlink PRIVATE
    ${esp-idf_SOURCE_DIR}/components/esp-tls
    ${esp-idf_SOURCE_DIR}/components/mbedtls/esp_crt_bundle/include
)
