import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import TailscaleComponent

CONF_TAILSCALE_ID = "tailscale_id"

TS_SCHEMA = text_sensor.text_sensor_schema(entity_category="diagnostic")
TS_TIMESTAMP_SCHEMA = text_sensor.text_sensor_schema(
    entity_category="diagnostic",
    device_class="timestamp",
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TAILSCALE_ID): cv.use_id(TailscaleComponent),
        cv.Optional("ip_address"): TS_SCHEMA,
        cv.Optional("vpn_hostname"): TS_SCHEMA,
        cv.Optional("memory_mode"): TS_SCHEMA,
        cv.Optional("setup_status"): TS_SCHEMA,
        cv.Optional("peer_status"): TS_SCHEMA,
        cv.Optional("magicdns"): TS_SCHEMA,
        cv.Optional("peer_list"): TS_SCHEMA,
        cv.Optional("network_name"): TS_SCHEMA,
        cv.Optional("key_expiry"): TS_TIMESTAMP_SCHEMA,
        cv.Optional("ha_connection_route"): TS_SCHEMA,
        cv.Optional("ha_connection_ip"): TS_SCHEMA,
        cv.Optional("control_plane"): TS_SCHEMA,
        cv.Optional("login_server"): TS_SCHEMA,
        cv.Optional("auth_key_status"): TS_SCHEMA,
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TAILSCALE_ID])

    for key, setter in [
        ("ip_address", "set_ip_address_text_sensor"),
        ("vpn_hostname", "set_hostname_text_sensor"),
        ("memory_mode", "set_memory_mode_text_sensor"),
        ("setup_status", "set_setup_status_text_sensor"),
        ("peer_status", "set_peer_status_text_sensor"),
        ("magicdns", "set_magicdns_text_sensor"),
        ("peer_list", "set_peer_list_text_sensor"),
        ("network_name", "set_tailnet_name_text_sensor"),
        ("key_expiry", "set_key_expiry_text_sensor"),
        ("ha_connection_route", "set_ha_connection_route_text_sensor"),
        ("ha_connection_ip", "set_ha_connection_ip_text_sensor"),
        ("control_plane", "set_control_plane_text_sensor"),
        ("login_server", "set_login_server_text_sensor"),
        ("auth_key_status", "set_auth_key_status_text_sensor"),
    ]:
        if key not in config:
            continue
        sens = await text_sensor.new_text_sensor(config[key])
        cg.add(getattr(parent, setter)(sens))
