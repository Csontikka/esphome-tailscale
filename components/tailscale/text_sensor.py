import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID, CONF_IP_ADDRESS

from . import TailscaleComponent, tailscale_ns

CONF_TAILSCALE_ID = "tailscale_id"
CONF_TAILSCALE_HOSTNAME = "tailscale_hostname"
CONF_MEMORY_MODE = "memory_mode"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TAILSCALE_ID): cv.use_id(TailscaleComponent),
        cv.Optional(CONF_IP_ADDRESS): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
        ),
        cv.Optional(CONF_TAILSCALE_HOSTNAME): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
        ),
        cv.Optional(CONF_MEMORY_MODE): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TAILSCALE_ID])

    if ip_config := config.get(CONF_IP_ADDRESS):
        sens = await text_sensor.new_text_sensor(ip_config)
        cg.add(parent.set_ip_address_text_sensor(sens))

    if hostname_config := config.get(CONF_TAILSCALE_HOSTNAME):
        sens = await text_sensor.new_text_sensor(hostname_config)
        cg.add(parent.set_hostname_text_sensor(sens))

    if memory_config := config.get(CONF_MEMORY_MODE):
        sens = await text_sensor.new_text_sensor(memory_config)
        cg.add(parent.set_memory_mode_text_sensor(sens))
