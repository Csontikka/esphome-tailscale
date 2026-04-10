import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor

from . import TailscaleComponent

CONF_TAILSCALE_ID = "tailscale_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TAILSCALE_ID): cv.use_id(TailscaleComponent),
        cv.Optional("connected", default={"name": "Tailscale Connected"}): binary_sensor.binary_sensor_schema(
            device_class="connectivity",
            entity_category="diagnostic",
        ),
        cv.Optional(
            "key_expiry_warning",
            default={"name": "Tailscale Key Expiry Warning"},
        ): binary_sensor.binary_sensor_schema(
            device_class="problem",
            entity_category="diagnostic",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TAILSCALE_ID])
    sens = await binary_sensor.new_binary_sensor(config["connected"])
    cg.add(parent.set_connected_binary_sensor(sens))
    if "key_expiry_warning" in config:
        warn = await binary_sensor.new_binary_sensor(config["key_expiry_warning"])
        cg.add(parent.set_key_expiry_warning_binary_sensor(warn))
