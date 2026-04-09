import os
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
)
from esphome.components.esp32 import add_idf_sdkconfig_option

CODEOWNERS = ["@esphome-tailscale"]
DEPENDENCIES = ["wifi", "esp32"]
AUTO_LOAD = ["binary_sensor", "text_sensor", "sensor"]

CONF_AUTH_KEY = "auth_key"
CONF_HOSTNAME = "hostname"
CONF_ENABLE_DERP = "enable_derp"
CONF_ENABLE_STUN = "enable_stun"
CONF_ENABLE_DISCO = "enable_disco"
CONF_MAX_PEERS = "max_peers"
CONF_LOGIN_SERVER = "login_server"

tailscale_ns = cg.esphome_ns.namespace("tailscale")
TailscaleComponent = tailscale_ns.class_("TailscaleComponent", cg.PollingComponent)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TailscaleComponent),
        cv.Required(CONF_AUTH_KEY): cv.string,
        cv.Optional(CONF_HOSTNAME, default=""): cv.string,
        cv.Optional(CONF_ENABLE_DERP, default=True): cv.boolean,
        cv.Optional(CONF_ENABLE_STUN, default=True): cv.boolean,
        cv.Optional(CONF_ENABLE_DISCO, default=True): cv.boolean,
        cv.Optional(CONF_MAX_PEERS, default=16): cv.int_range(min=1, max=64),
        cv.Optional(CONF_LOGIN_SERVER, default=""): cv.string,
    }
).extend(cv.polling_component_schema("10s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_auth_key(config[CONF_AUTH_KEY]))
    cg.add(var.set_hostname(config[CONF_HOSTNAME]))
    cg.add(var.set_enable_derp(config[CONF_ENABLE_DERP]))
    cg.add(var.set_enable_stun(config[CONF_ENABLE_STUN]))
    cg.add(var.set_enable_disco(config[CONF_ENABLE_DISCO]))
    cg.add(var.set_max_peers(config[CONF_MAX_PEERS]))

    if config[CONF_LOGIN_SERVER]:
        cg.add(var.set_login_server(config[CONF_LOGIN_SERVER]))

    # Add microlink ESP-IDF components to the build
    # Find the project root (where microlink submodule lives)
    this_dir = os.path.dirname(__file__)
    project_root = os.path.abspath(os.path.join(this_dir, "..", ".."))
    idf_components = os.path.join(project_root, "idf_components").replace("\\", "/")
    wg_components = os.path.join(
        project_root, "microlink", "components", "microlink", "components"
    ).replace("\\", "/")

    # Required ESP-IDF sdkconfig for Tailscale/WireGuard
    # PSRAM: enable driver but don't crash if not present
    add_idf_sdkconfig_option("CONFIG_SPIRAM", True)
    add_idf_sdkconfig_option("CONFIG_SPIRAM_IGNORE_NOTFOUND", True)
    add_idf_sdkconfig_option("CONFIG_SPIRAM_MODE_OCT", True)
    add_idf_sdkconfig_option("CONFIG_SPIRAM_SPEED_80M", True)
    # lwIP: IP forwarding between WiFi and WG netif, IPv6 for STUN/DERP
    add_idf_sdkconfig_option("CONFIG_LWIP_IP_FORWARD", True)
    add_idf_sdkconfig_option("CONFIG_LWIP_IPV6", True)
    add_idf_sdkconfig_option("CONFIG_LWIP_CHECK_THREAD_SAFETY", False)
    # mbedTLS: WireGuard crypto (ChaCha20-Poly1305) + Tailscale control plane TLS
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CHACHAPOLY_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CHACHA20_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_POLY1305_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_HKDF_C", True)

    # Add microlink include paths and config override header
    ml_include = os.path.join(project_root, "microlink", "components", "microlink", "include").replace("\\", "/")
    ml_src = os.path.join(project_root, "microlink", "components", "microlink", "src").replace("\\", "/")
    override_h = os.path.join(idf_components, "microlink", "ml_config_override.h").replace("\\", "/")
    cg.add_build_flag(f"-I{ml_include}")
    cg.add_build_flag(f"-I{ml_src}")
    cg.add_build_flag(f"-include {override_h}")

    # Copy patch_cmake.py to the build directory so extra_scripts can find it
    import shutil
    import esphome.core as core

    build_dir = core.CORE.relative_build_path("")
    patch_src = os.path.join(this_dir, "patch_cmake.py")
    patch_dst = os.path.join(build_dir, "patch_cmake.py")
    os.makedirs(build_dir, exist_ok=True)
    shutil.copy2(patch_src, patch_dst)
    cg.add_platformio_option("extra_scripts", ["pre:patch_cmake.py"])
