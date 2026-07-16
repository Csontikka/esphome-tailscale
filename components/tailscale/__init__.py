import os
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    STATE_CLASS_MEASUREMENT,
)
from esphome.components.esp32 import (
    add_idf_component,
    add_idf_sdkconfig_option,
    include_builtin_idf_component,
)
from esphome.components import binary_sensor, text_sensor, sensor

CODEOWNERS = ["@esphome-tailscale"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["binary_sensor", "text_sensor", "sensor", "button", "switch", "text"]


def _validate_network(config):
    """Validate that either wifi or ethernet is configured."""
    import esphome.core as core

    full = core.CORE.raw_config
    if "wifi" not in full and "ethernet" not in full:
        raise cv.Invalid(
            "The tailscale component requires either 'wifi:' or 'ethernet:' "
            "to be configured. Add one of them to your YAML."
        )
    return config


FINAL_VALIDATE_SCHEMA = _validate_network

CONF_AUTH_KEY = "auth_key"
CONF_HOSTNAME = "hostname"
CONF_MAX_PEERS = "max_peers"
CONF_LOGIN_SERVER = "login_server"
CONF_DISABLE_TELEMETRY = "disable_telemetry"
tailscale_ns = cg.esphome_ns.namespace("tailscale")
TailscaleComponent = tailscale_ns.class_("TailscaleComponent", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TailscaleComponent),
        cv.Required(CONF_AUTH_KEY): cv.string,
        cv.Optional(CONF_HOSTNAME, default=""): cv.string,
        cv.Optional(CONF_MAX_PEERS, default=16): cv.int_range(min=1, max=64),
        cv.Optional(CONF_LOGIN_SERVER, default=""): cv.string,
        cv.Optional(CONF_DISABLE_TELEMETRY, default=False): cv.boolean,
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_auth_key(config[CONF_AUTH_KEY]))
    cg.add(var.set_hostname(config[CONF_HOSTNAME]))
    cg.add(var.set_max_peers(config[CONF_MAX_PEERS]))

    if config[CONF_LOGIN_SERVER]:
        cg.add(var.set_login_server(config[CONF_LOGIN_SERVER]))

    cg.add(var.set_telemetry_disabled(config[CONF_DISABLE_TELEMETRY]))
    # Telemetry POSTs over HTTPS via ESP-IDF's esp_http_client, which ESPHome
    # excludes by default to save compile time — re-enable it. (TLS uses the
    # mbedtls certificate bundle already enabled below.)
    include_builtin_idf_component("esp_http_client")

    # microlink sizes the static `ml_peer_t peers[ML_MAX_PEERS]` array at compile
    # time from CONFIG_ML_MAX_PEERS (default 16), and microlink_init() clamps the
    # runtime config to that compile-time ceiling. Propagate the YAML value into
    # sdkconfig so the compiled ceiling matches the runtime intent.
    add_idf_sdkconfig_option("CONFIG_ML_MAX_PEERS", config[CONF_MAX_PEERS])

    # Sensors are created via platform YAML files (binary_sensor.py, text_sensor.py, sensor.py)
    # They are auto-loaded and auto-configured - user doesn't need to add them manually

    # Find project root (where microlink submodule lives)
    this_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.abspath(os.path.join(this_dir, "..", ".."))
    ml_base = os.path.join(project_root, "microlink", "components", "microlink").replace("\\", "/")
    ml_include = f"{ml_base}/include"
    ml_src = f"{ml_base}/src"
    override_h = f"{ml_base}/ml_config_override.h"

    # Required ESP-IDF sdkconfig for Tailscale/WireGuard
    add_idf_sdkconfig_option("CONFIG_SPIRAM", True)
    add_idf_sdkconfig_option("CONFIG_SPIRAM_IGNORE_NOTFOUND", True)
    add_idf_sdkconfig_option("CONFIG_SPIRAM_MODE_OCT", True)
    add_idf_sdkconfig_option("CONFIG_SPIRAM_SPEED_80M", True)
    add_idf_sdkconfig_option("CONFIG_LWIP_IP_FORWARD", True)
    add_idf_sdkconfig_option("CONFIG_LWIP_IPV6", True)
    add_idf_sdkconfig_option("CONFIG_LWIP_CHECK_THREAD_SAFETY", False)
    add_idf_sdkconfig_option("CONFIG_LWIP_MAX_SOCKETS", 24)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CHACHAPOLY_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CHACHA20_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_POLY1305_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_HKDF_C", True)

    # ESPHome defaults CONFIG_LOG_MAXIMUM_LEVEL to ERROR (1), which compiles
    # out all ESP_LOGI/ESP_LOGW calls at the preprocessor level. Raise to INFO
    # so the runtime debug log switch can enable microlink logs via esp_log_level_set().
    add_idf_sdkconfig_option("CONFIG_LOG_MAXIMUM_LEVEL_ERROR", False)
    add_idf_sdkconfig_option("CONFIG_LOG_MAXIMUM_LEVEL_INFO", True)

    # Enable per-tag runtime log level control (esp_log_level_set)
    # ESPHome defaults to CONFIG_LOG_TAG_LEVEL_IMPL_NONE which makes it a no-op
    add_idf_sdkconfig_option("CONFIG_LOG_TAG_LEVEL_IMPL_NONE", False)
    add_idf_sdkconfig_option("CONFIG_LOG_TAG_LEVEL_IMPL_LINKED_LIST", True)

    # Add microlink include paths. On older ESPHome (and the arduino path)
    # these -I flags let the headers resolve directly; on the 2026.7+ esp-idf
    # build they don't propagate, but the component REQUIRE (below) covers it.
    cg.add_build_flag(f"-I{ml_include}")
    cg.add_build_flag(f"-I{ml_src}")

    # Wire the vendored microlink ESP-IDF components into the build via
    # ESPHome's official add_idf_component() with a local `path`. This replaces
    # the previous approach — a generated patch_cmake.py registered through
    # `extra_scripts` in platformio.ini that appended EXTRA_COMPONENT_DIRS — which
    # the ESPHome 2026.7 build_gen rewrite silently stopped honoring (the esp-idf
    # build no longer routes component options through platformio.ini), so
    # <microlink.h> could not be found (issue #28). add_idf_component registers
    # each local path as a managed component that ESPHome's main REQUIREs, so the
    # headers resolve; its signature is identical on 2026.6.x and 2026.7, so this
    # works across both. wireguard_lwip is microlink's nested dependency and is
    # added as its own component so microlink's REQUIRES(wireguard_lwip) resolves.
    add_idf_component(
        name="wireguard_lwip", path=f"{ml_base}/components/wireguard_lwip"
    )
    add_idf_component(name="microlink", path=ml_base)
