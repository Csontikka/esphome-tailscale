import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, STATE_CLASS_MEASUREMENT

from . import TailscaleComponent, tailscale_ns

CONF_TAILSCALE_ID = "tailscale_id"
CONF_PEER_COUNT = "peer_count"
CONF_MAX_PEERS = "max_peers"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TAILSCALE_ID): cv.use_id(TailscaleComponent),
        cv.Optional(CONF_PEER_COUNT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category="diagnostic",
        ),
        cv.Optional(CONF_MAX_PEERS): sensor.sensor_schema(
            accuracy_decimals=0,
            entity_category="diagnostic",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TAILSCALE_ID])

    if peer_config := config.get(CONF_PEER_COUNT):
        sens = await sensor.new_sensor(peer_config)
        cg.add(parent.set_peer_count_sensor(sens))

    if max_config := config.get(CONF_MAX_PEERS):
        sens = await sensor.new_sensor(max_config)
        cg.add(parent.set_max_peers_sensor(sens))
