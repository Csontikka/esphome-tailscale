import os
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    STATE_CLASS_MEASUREMENT,
)
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.components import binary_sensor, text_sensor, sensor

CODEOWNERS = ["@esphome-tailscale"]
DEPENDENCIES = ["wifi", "esp32"]
AUTO_LOAD = ["binary_sensor", "text_sensor", "sensor", "button", "switch"]

CONF_AUTH_KEY = "auth_key"
CONF_HOSTNAME = "hostname"
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
        cv.Optional(CONF_ENABLE_STUN, default=True): cv.boolean,
        cv.Optional(CONF_ENABLE_DISCO, default=True): cv.boolean,
        cv.Optional(CONF_MAX_PEERS, default=16): cv.int_range(min=1, max=64),
        cv.Optional(CONF_LOGIN_SERVER, default=""): cv.string,
    }
).extend(cv.polling_component_schema("30s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_auth_key(config[CONF_AUTH_KEY]))
    cg.add(var.set_hostname(config[CONF_HOSTNAME]))
    cg.add(var.set_enable_stun(config[CONF_ENABLE_STUN]))
    cg.add(var.set_enable_disco(config[CONF_ENABLE_DISCO]))
    cg.add(var.set_max_peers(config[CONF_MAX_PEERS]))

    if config[CONF_LOGIN_SERVER]:
        cg.add(var.set_login_server(config[CONF_LOGIN_SERVER]))

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

    # Add microlink include paths (config override handled by Kconfig defaults)
    cg.add_build_flag(f"-I{ml_include}")
    cg.add_build_flag(f"-I{ml_src}")

    # Generate patch_cmake.py to add microlink ESP-IDF components to CMake build
    import esphome.core as core
    ml_components = os.path.join(project_root, "microlink", "components").replace("\\", "/")
    ml_wg = f"{ml_base}/components"

    build_dir = core.CORE.relative_build_path("")
    os.makedirs(build_dir, exist_ok=True)
    with open(os.path.join(build_dir, "patch_cmake.py"), "w") as f:
        f.write(f'''import os
Import("env")
cmake_file = os.path.join(env.subst("$PROJECT_DIR"), "CMakeLists.txt")
components = "{ml_components}"
wg = "{ml_wg}"
if os.path.exists(cmake_file):
    content = open(cmake_file).read()
    if components not in content:
        content = content.replace(
            "include($ENV{{IDF_PATH}}/tools/cmake/project.cmake)",
            'set(EXTRA_COMPONENT_DIRS "${{EXTRA_COMPONENT_DIRS}}" "' + components + '" "' + wg + '")\\n'
            "include($ENV{{IDF_PATH}}/tools/cmake/project.cmake)")
        open(cmake_file, "w").write(content)
else:
    content = 'cmake_minimum_required(VERSION 3.16.0)\\n'
    content += 'set(EXTRA_COMPONENT_DIRS "${{EXTRA_COMPONENT_DIRS}}" "' + components + '" "' + wg + '")\\n'
    content += 'include($ENV{{IDF_PATH}}/tools/cmake/project.cmake)\\n'
    content += 'project({core.CORE.name})\\n'
    open(cmake_file, "w").write(content)
''')
    cg.add_platformio_option("extra_scripts", ["pre:patch_cmake.py"])
