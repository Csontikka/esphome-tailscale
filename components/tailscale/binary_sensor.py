import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor

from . import TailscaleComponent

CONF_TAILSCALE_ID = "tailscale_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TAILSCALE_ID): cv.use_id(TailscaleComponent),
        cv.Optional("connected"): binary_sensor.binary_sensor_schema(
            device_class="connectivity",
            entity_category="diagnostic",
        ),
        cv.Optional("key_expiry_warning"): binary_sensor.binary_sensor_schema(
            device_class="problem",
            entity_category="diagnostic",
        ),
        cv.Optional("ha_connected"): binary_sensor.binary_sensor_schema(
            device_class="connectivity",
            entity_category="diagnostic",
        ),
        cv.Optional("vpn_auto_rollback"): binary_sensor.binary_sensor_schema(
            entity_category="diagnostic",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TAILSCALE_ID])
    if "connected" in config:
        sens = await binary_sensor.new_binary_sensor(config["connected"])
        cg.add(parent.set_connected_binary_sensor(sens))
    if "key_expiry_warning" in config:
        warn = await binary_sensor.new_binary_sensor(config["key_expiry_warning"])
        cg.add(parent.set_key_expiry_warning_binary_sensor(warn))
    if "ha_connected" in config:
        ha = await binary_sensor.new_binary_sensor(config["ha_connected"])
        cg.add(parent.set_ha_connected_binary_sensor(ha))
    if "vpn_auto_rollback" in config:
        rb = await binary_sensor.new_binary_sensor(config["vpn_auto_rollback"])
        cg.add(parent.set_vpn_auto_rollback_binary_sensor(rb))
