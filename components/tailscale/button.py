import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button

from . import TailscaleComponent

CONF_TAILSCALE_ID = "tailscale_id"

tailscale_ns = cg.esphome_ns.namespace("tailscale")
TailscaleReconnectButton = tailscale_ns.class_(
    "TailscaleReconnectButton", button.Button, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TAILSCALE_ID): cv.use_id(TailscaleComponent),
        cv.Optional("reconnect"): button.button_schema(
            TailscaleReconnectButton,
            entity_category="config",
            icon="mdi:connection",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TAILSCALE_ID])
    if "reconnect" in config:
        btn = await button.new_button(config["reconnect"])
        await cg.register_component(btn, {})
        cg.add(btn.set_parent(parent))
