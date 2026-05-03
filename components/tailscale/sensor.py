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
        cv.Optional("peers_online"): PEER_SCHEMA,
        cv.Optional("peers_direct"): PEER_SCHEMA,
        cv.Optional("peers_derp"): PEER_SCHEMA,
        cv.Optional("peers_max"): PEER_SCHEMA,
        cv.Optional("connections"): PEER_SCHEMA,
        cv.Optional("uptime"): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            device_class="duration",
            unit_of_measurement="s",
            entity_category="diagnostic",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TAILSCALE_ID])

    for key, setter in [
        ("peers_online", "set_peers_online_sensor"),
        ("peers_direct", "set_peers_direct_sensor"),
        ("peers_derp", "set_peers_derp_sensor"),
        ("peers_max", "set_peers_max_sensor"),
        ("connections", "set_connections_sensor"),
        ("uptime", "set_uptime_sensor"),
    ]:
        if key not in config:
            continue
        sens = await sensor.new_sensor(config[key])
        cg.add(getattr(parent, setter)(sens))
