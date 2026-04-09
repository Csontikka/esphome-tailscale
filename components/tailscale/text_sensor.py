import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import TailscaleComponent

CONF_TAILSCALE_ID = "tailscale_id"

TS_SCHEMA = text_sensor.text_sensor_schema(entity_category="diagnostic")

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TAILSCALE_ID): cv.use_id(TailscaleComponent),
        cv.Optional("ip_address", default={"name": "Tailscale IP"}): TS_SCHEMA,
        cv.Optional("tailscale_hostname", default={"name": "Tailscale Hostname"}): TS_SCHEMA,
        cv.Optional("memory_mode", default={"name": "Tailscale Memory"}): TS_SCHEMA,
        cv.Optional("setup_status", default={"name": "Tailscale Setup Status"}): TS_SCHEMA,
        cv.Optional("peer_status", default={"name": "Tailscale Peer Status"}): TS_SCHEMA,
        cv.Optional("magicdns", default={"name": "Tailscale MagicDNS"}): TS_SCHEMA,
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TAILSCALE_ID])

    for key, setter in [
        ("ip_address", "set_ip_address_text_sensor"),
        ("tailscale_hostname", "set_hostname_text_sensor"),
        ("memory_mode", "set_memory_mode_text_sensor"),
        ("setup_status", "set_setup_status_text_sensor"),
        ("peer_status", "set_peer_status_text_sensor"),
        ("magicdns", "set_magicdns_text_sensor"),
    ]:
        sens = await text_sensor.new_text_sensor(config[key])
        cg.add(getattr(parent, setter)(sens))
