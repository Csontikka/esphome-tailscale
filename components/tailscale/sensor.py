import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, STATE_CLASS_MEASUREMENT

from . import TailscaleComponent, tailscale_ns

CONF_TAILSCALE_ID = "tailscale_id"
CONF_PEERS_TOTAL = "peers_total"
CONF_PEERS_ONLINE = "peers_online"
CONF_PEERS_DIRECT = "peers_direct"
CONF_PEERS_DERP = "peers_derp"
CONF_PEERS_MAX = "peers_max"

PEER_SENSOR = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    entity_category="diagnostic",
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TAILSCALE_ID): cv.use_id(TailscaleComponent),
        cv.Optional(CONF_PEERS_TOTAL): PEER_SENSOR,
        cv.Optional(CONF_PEERS_ONLINE): PEER_SENSOR,
        cv.Optional(CONF_PEERS_DIRECT): PEER_SENSOR,
        cv.Optional(CONF_PEERS_DERP): PEER_SENSOR,
        cv.Optional(CONF_PEERS_MAX): sensor.sensor_schema(
            accuracy_decimals=0,
            entity_category="diagnostic",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TAILSCALE_ID])

    for conf_key, setter in [
        (CONF_PEERS_TOTAL, "set_peers_total_sensor"),
        (CONF_PEERS_ONLINE, "set_peers_online_sensor"),
        (CONF_PEERS_DIRECT, "set_peers_direct_sensor"),
        (CONF_PEERS_DERP, "set_peers_derp_sensor"),
        (CONF_PEERS_MAX, "set_peers_max_sensor"),
    ]:
        if sub_config := config.get(conf_key):
            sens = await sensor.new_sensor(sub_config)
            cg.add(getattr(parent, setter)(sens))
