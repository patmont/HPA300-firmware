"""HPA300 Home Assistant integration."""

from typing import TypeAlias

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .api import HPA300Api
from .const import CONF_API_TOKEN, PLATFORMS
from .coordinator import HPA300Coordinator

HPA300ConfigEntry: TypeAlias = ConfigEntry[HPA300Coordinator]


async def async_setup_entry(hass: HomeAssistant, entry: HPA300ConfigEntry) -> bool:
    """Set up HPA300 from a config entry."""
    api = HPA300Api(
        async_get_clientsession(hass),
        entry.data[CONF_HOST],
        entry.data[CONF_API_TOKEN],
    )
    coordinator = HPA300Coordinator(hass, entry, api)
    await coordinator.async_config_entry_first_refresh()
    entry.runtime_data = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: HPA300ConfigEntry) -> bool:
    """Unload an HPA300 config entry."""
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
