import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text

from . import TailscaleComponent

CONF_TAILSCALE_ID = "tailscale_id"

tailscale_ns = cg.esphome_ns.namespace("tailscale")
TailscaleAuthKeyText = tailscale_ns.class_(
    "TailscaleAuthKeyText", text.Text, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TAILSCALE_ID): cv.use_id(TailscaleComponent),
        cv.Optional("auth_key"): text.text_schema(
            TailscaleAuthKeyText,
            entity_category="config",
            icon="mdi:key-variant",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TAILSCALE_ID])

    if "auth_key" in config:
        t = await text.new_text(config["auth_key"], min_length=0, max_length=128)
        await cg.register_component(t, {})
        cg.add(t.set_parent(parent))
        cg.add(parent.set_auth_key_text(t))
