"""Data coordinator for HPA300 devices."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import UTC, datetime
import logging
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryAuthFailed
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import HPA300Api, HPA300AuthError, HPA300Error
from .const import DEVICE_POLL_INTERVAL, DOMAIN, STATE_POLL_INTERVAL

_LOGGER = logging.getLogger(__name__)


@dataclass(slots=True)
class HPA300Data:
    """Latest data received from an HPA300."""

    device: dict[str, Any]
    state: dict[str, Any]
    device_updated_at: datetime


class HPA300Coordinator(DataUpdateCoordinator[HPA300Data]):
    """Poll fast-changing state and slower device diagnostics."""

    def __init__(
        self, hass: HomeAssistant, entry: ConfigEntry, api: HPA300Api
    ) -> None:
        super().__init__(
            hass,
            _LOGGER,
            config_entry=entry,
            name=DOMAIN,
            update_interval=STATE_POLL_INTERVAL,
        )
        self.api = api

    async def _async_update_data(self) -> HPA300Data:
        try:
            state = await self.api.async_get_state()
            now = datetime.now(UTC)
            if (
                self.data is None
                or now - self.data.device_updated_at >= DEVICE_POLL_INTERVAL
            ):
                device = await self.api.async_get_device()
                device_updated_at = now
            else:
                device = self.data.device
                device_updated_at = self.data.device_updated_at
        except HPA300AuthError as err:
            raise ConfigEntryAuthFailed("The HPA300 rejected its API token") from err
        except HPA300Error as err:
            raise UpdateFailed(f"Unable to update HPA300: {err}") from err
        return HPA300Data(device, state, device_updated_at)

