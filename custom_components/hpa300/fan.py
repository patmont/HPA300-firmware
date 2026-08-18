"""Fan platform for HPA300."""

from __future__ import annotations

import math
from typing import Any

from homeassistant.components.fan import FanEntity, FanEntityFeature
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HPA300ConfigEntry
from .api import HPA300Error
from .coordinator import HPA300Coordinator
from .entity import HPA300Entity


async def async_setup_entry(
    hass: HomeAssistant,
    entry: HPA300ConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    """Set up the HPA300 fan."""
    async_add_entities([HPA300Fan(entry.runtime_data)])


class HPA300Fan(HPA300Entity, FanEntity):
    """The HPA300 purifier fan."""

    _attr_name = None
    _attr_supported_features = (
        FanEntityFeature.SET_SPEED
        | FanEntityFeature.TURN_ON
        | FanEntityFeature.TURN_OFF
    )

    def __init__(self, coordinator: HPA300Coordinator) -> None:
        super().__init__(coordinator, "fan")

    @property
    def is_on(self) -> bool:
        return bool(self.coordinator.data.state.get("power"))

    @property
    def percentage(self) -> int:
        return int(self.coordinator.data.state.get("percentage", 0))

    @property
    def speed_count(self) -> int:
        return int(self.coordinator.data.device.get("speed_count", 4))

    async def async_turn_on(
        self,
        percentage: int | None = None,
        preset_mode: str | None = None,
        **kwargs: Any,
    ) -> None:
        speed = 1 if percentage is None else self._percentage_to_speed(percentage)
        await self._async_set_speed(max(1, speed))
        await self.coordinator.async_request_refresh()

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self._async_set_speed(0)
        await self.coordinator.async_request_refresh()

    async def async_set_percentage(self, percentage: int) -> None:
        await self._async_set_speed(self._percentage_to_speed(percentage))
        await self.coordinator.async_request_refresh()

    async def _async_set_speed(self, speed: int) -> None:
        try:
            await self.coordinator.api.async_set_speed(speed)
        except HPA300Error as err:
            raise HomeAssistantError(f"Unable to set HPA300 speed: {err}") from err

    def _percentage_to_speed(self, percentage: int) -> int:
        if percentage <= 0:
            return 0
        return min(self.speed_count, math.ceil(percentage * self.speed_count / 100))
