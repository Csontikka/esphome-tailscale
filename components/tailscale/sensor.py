import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import STATE_CLASS_MEASUREMENT

from . import TailscaleComponent

CONF_TAILSCALE_ID = "tailscale_id"

PEER_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    entity_category="diagnostic",
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TAILSCALE_ID): cv.use_id(TailscaleComponent),
        cv.Optional("peers_total", default={"name": "Tailscale Peers Total"}): PEER_SCHEMA,
        cv.Optional("peers_online", default={"name": "Tailscale Peers Online"}): PEER_SCHEMA,
        cv.Optional("peers_direct", default={"name": "Tailscale Peers Direct"}): PEER_SCHEMA,
        cv.Optional("peers_derp", default={"name": "Tailscale Peers DERP"}): PEER_SCHEMA,
        cv.Optional("peers_max", default={"name": "Tailscale Peers Max"}): PEER_SCHEMA,
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TAILSCALE_ID])

    for key, setter in [
        ("peers_total", "set_peers_total_sensor"),
        ("peers_online", "set_peers_online_sensor"),
        ("peers_direct", "set_peers_direct_sensor"),
        ("peers_derp", "set_peers_derp_sensor"),
        ("peers_max", "set_peers_max_sensor"),
    ]:
        sens = await sensor.new_sensor(config[key])
        cg.add(getattr(parent, setter)(sens))
