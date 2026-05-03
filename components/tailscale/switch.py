import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch

from . import TailscaleComponent

CONF_TAILSCALE_ID = "tailscale_id"

tailscale_ns = cg.esphome_ns.namespace("tailscale")
TailscaleEnableSwitch = tailscale_ns.class_(
    "TailscaleEnableSwitch", switch.Switch, cg.Component
)
TailscaleDebugLogSwitch = tailscale_ns.class_(
    "TailscaleDebugLogSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TAILSCALE_ID): cv.use_id(TailscaleComponent),
        cv.Optional("vpn_enabled"): switch.switch_schema(
            TailscaleEnableSwitch,
            entity_category="config",
            icon="mdi:vpn",
        ),
        cv.Optional("debug_log"): switch.switch_schema(
            TailscaleDebugLogSwitch,
            entity_category="config",
            icon="mdi:bug-outline",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TAILSCALE_ID])

    for key, setter in [
        ("vpn_enabled", "set_enable_switch"),
        ("debug_log", "set_debug_log_switch"),
    ]:
        if key not in config:
            continue
        sw = await switch.new_switch(config[key])
        await cg.register_component(sw, {})
        cg.add(sw.set_parent(parent))
        cg.add(getattr(parent, setter)(sw))
