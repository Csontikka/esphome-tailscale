set(SRCS
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/microlink.c"
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/ml_coord.c"
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/ml_derp.c"
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/ml_net_io.c"
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/ml_wg_mgr.c"
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/ml_stun.c"
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/ml_noise.c"
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/ml_h2.c"
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/ml_udp.c"
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/ml_tcp.c"
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/ml_peer_nvs.c"
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/nacl_box.c"
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/ml_zerocopy.c"
    "C:/devuser2/esphome_tailscale/microlink/components/microlink/src/ml_config_httpd.c"
)
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "C:/devuser2/esphome_tailscale/microlink/components/microlink/include" "C:/devuser2/esphome_tailscale/microlink/components/microlink/src"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer lwip mbedtls json
    PRIV_REQUIRES wireguard_lwip
)
