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
        cv.Optional("ip_address", default={"name": "VPN IP"}): TS_SCHEMA,
        cv.Optional("vpn_hostname", default={"name": "VPN Hostname"}): TS_SCHEMA,
        cv.Optional("memory_mode", default={"name": "Device Memory"}): TS_SCHEMA,
        cv.Optional("setup_status", default={"name": "VPN Setup Hint"}): TS_SCHEMA,
        cv.Optional("peer_status", default={"name": "VPN Peer Status"}): TS_SCHEMA,
        cv.Optional("magicdns", default={"name": "VPN MagicDNS"}): TS_SCHEMA,
        cv.Optional("peer_list"): TS_SCHEMA,
        cv.Optional("network_name", default={"name": "VPN Network"}): TS_SCHEMA,
        cv.Optional("key_expiry", default={"name": "VPN Node Key Expiry"}): TS_TIMESTAMP_SCHEMA,
        cv.Optional("ha_connection_route", default={"name": "HA API Connection Route"}): TS_SCHEMA,
        cv.Optional("ha_connection_ip", default={"name": "HA API Connection IP"}): TS_SCHEMA,
        cv.Optional("control_plane", default={"name": "VPN Control Plane"}): TS_SCHEMA,
        cv.Optional("login_server", default={"name": "VPN Login Server"}): TS_SCHEMA,
        cv.Optional("auth_key_status", default={"name": "VPN Auth Key Source"}): TS_SCHEMA,
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
