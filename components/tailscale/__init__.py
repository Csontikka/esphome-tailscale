import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
)

CODEOWNERS = ["@esphome-tailscale"]
DEPENDENCIES = ["wifi"]
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
