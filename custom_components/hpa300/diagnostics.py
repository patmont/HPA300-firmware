"""Diagnostics support for HPA300."""

from typing import Any

from homeassistant.components.diagnostics import async_redact_data
from homeassistant.core import HomeAssistant

from . import HPA300ConfigEntry
from .const import CONF_API_TOKEN


async def async_get_config_entry_diagnostics(
    hass: HomeAssistant, entry: HPA300ConfigEntry
) -> dict[str, Any]:
    """Return redacted config and the latest controller data."""
    coordinator = entry.runtime_data
    return {
        "config": async_redact_data(dict(entry.data), {CONF_API_TOKEN}),
        "device": coordinator.data.device,
        "state": coordinator.data.state,
    }

