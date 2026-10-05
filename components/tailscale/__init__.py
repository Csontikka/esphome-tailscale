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
CONF_NETCHECK_OVERRIDE = "netcheck_override"
CONF_NETCHECK_OVERRIDE_THRESHOLD = "netcheck_override_threshold"
CONF_IPN_VERSION = "ipn_version"
CONF_NETMAP_BUFFER_KB = "netmap_buffer_kb"
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
        # Measure every DERP region and adopt the fastest one instead of the
        # region the control plane echoed back. Off by default: a home-region
        # change has to be announced to every peer, so it is disruptive, and
        # the current selection logic is a plain threshold. Useful when the
        # device is far from the default region (e.g. Frankfurt fallback on a
        # device in Australia) — see the README.
        cv.Optional(CONF_NETCHECK_OVERRIDE, default=False): cv.boolean,
        cv.Optional(
            CONF_NETCHECK_OVERRIDE_THRESHOLD, default="50ms"
        ): cv.positive_time_period_milliseconds,
        # Reported to the control plane as Hostinfo.IPNVersion. Default off
        # (omitted): a public client should not claim a version by default.
        # Set it only to clear the admin console's "Device is too old" gate,
        # e.g. ipn_version: "1.98.9". See README (Troubleshooting).
        cv.Optional(CONF_IPN_VERSION, default=""): cv.string,
        # Size (KB) of the PSRAM buffer microlink allocates for a netmap fetch
        # (and afterwards of the long-poll accumulator). Since #48 the fetch
        # needs one such block, not two. Without PSRAM nothing is pre-allocated
        # and this is only the upper limit for the tailnet map. The Kconfig default (512) is sized for
        # 300+ peer tailnets; on a 2 MB-PSRAM board that shares PSRAM with
        # other components the 2 x 512 KB allocation fails and every
        # MapRequest dies within milliseconds of a successful registration
        # (#45). 128 is plenty below ~50 peers. No default here on purpose:
        # unset leaves the Kconfig value (and any sdkconfig_options override)
        # untouched, so existing builds do not change.
        cv.Optional(CONF_NETMAP_BUFFER_KB): cv.int_range(min=64, max=2048),
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

    cg.add(var.set_netcheck_override(config[CONF_NETCHECK_OVERRIDE]))
    cg.add(
        var.set_netcheck_override_threshold(
            config[CONF_NETCHECK_OVERRIDE_THRESHOLD].total_milliseconds
        )
    )

    cg.add(var.set_telemetry_disabled(config[CONF_DISABLE_TELEMETRY]))
    cg.add(var.set_ipn_version(config[CONF_IPN_VERSION]))
    # Telemetry POSTs over HTTPS via ESP-IDF's esp_http_client, which ESPHome
    # excludes by default to save compile time — re-enable it. (TLS uses the
    # mbedtls certificate bundle already enabled below.)
    include_builtin_idf_component("esp_http_client")

    # microlink sizes the static `ml_peer_t peers[ML_MAX_PEERS]` array at compile
    # time from CONFIG_ML_MAX_PEERS (default 16), and microlink_init() clamps the
    # runtime config to that compile-time ceiling. Propagate the YAML value into
    # sdkconfig so the compiled ceiling matches the runtime intent.
    add_idf_sdkconfig_option("CONFIG_ML_MAX_PEERS", config[CONF_MAX_PEERS])

    # Both MapResponse buffers are compile-time Kconfig sizes (microlink_internal.h
    # derives ML_H2_BUFFER_SIZE / ML_JSON_BUFFER_SIZE from them). One YAML knob
    # sets both - the Kconfig help says to keep them equal. Only emitted when the
    # user asked for it, so the Kconfig default / a manual sdkconfig_options
    # override keeps working unchanged (#45).
    if CONF_NETMAP_BUFFER_KB in config:
        add_idf_sdkconfig_option("CONFIG_ML_H2_BUFFER_SIZE_KB", config[CONF_NETMAP_BUFFER_KB])
        add_idf_sdkconfig_option("CONFIG_ML_JSON_BUFFER_SIZE_KB", config[CONF_NETMAP_BUFFER_KB])

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
