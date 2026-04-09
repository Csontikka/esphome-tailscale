import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch

from . import TailscaleComponent

CONF_TAILSCALE_ID = "tailscale_id"

tailscale_ns = cg.esphome_ns.namespace("tailscale")
TailscaleDerpSwitch = tailscale_ns.class_(
    "TailscaleDerpSwitch", switch.Switch, cg.Component
)
TailscaleEnableSwitch = tailscale_ns.class_(
    "TailscaleEnableSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TAILSCALE_ID): cv.use_id(TailscaleComponent),
        cv.Optional("derp_enabled", default={"name": "Tailscale DERP"}): switch.switch_schema(
            TailscaleDerpSwitch,
            entity_category="config",
            icon="mdi:cloud-sync",
        ),
        cv.Optional("tailscale_enabled", default={"name": "Tailscale Enabled"}): switch.switch_schema(
            TailscaleEnableSwitch,
            entity_category="config",
            icon="mdi:vpn",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TAILSCALE_ID])

    for key, setter in [
        ("derp_enabled", "set_derp_switch"),
        ("tailscale_enabled", "set_enable_switch"),
    ]:
        sw = await switch.new_switch(config[key])
        await cg.register_component(sw, {})
        cg.add(getattr(sw, "set_parent")(parent))
