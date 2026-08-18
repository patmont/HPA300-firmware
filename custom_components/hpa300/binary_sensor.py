"""Binary sensor platform for HPA300."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
    BinarySensorEntityDescription,
)
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HPA300ConfigEntry
from .coordinator import HPA300Coordinator, HPA300Data
from .entity import HPA300Entity


@dataclass(frozen=True, kw_only=True)
class HPA300BinarySensorDescription(BinarySensorEntityDescription):
    value_fn: Callable[[HPA300Data], bool]


BINARY_SENSORS: tuple[HPA300BinarySensorDescription, ...] = (
    HPA300BinarySensorDescription(
        key="fault",
        translation_key="fault",
        device_class=BinarySensorDeviceClass.PROBLEM,
        value_fn=lambda data: bool(data.state.get("fault_latched")),
    ),
    HPA300BinarySensorDescription(
        key="maintenance",
        translation_key="maintenance",
        device_class=BinarySensorDeviceClass.RUNNING,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda data: bool(data.state.get("maintenance_active")),
    ),
    HPA300BinarySensorDescription(
        key="wifi",
        translation_key="wifi",
        device_class=BinarySensorDeviceClass.CONNECTIVITY,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda data: bool(data.state.get("wifi_connected")),
    ),
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: HPA300ConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    """Set up HPA300 binary sensors."""
    async_add_entities(
        HPA300BinarySensor(entry.runtime_data, description)
        for description in BINARY_SENSORS
    )


class HPA300BinarySensor(HPA300Entity, BinarySensorEntity):
    """A binary sensor backed by coordinator data."""

    entity_description: HPA300BinarySensorDescription

    def __init__(
        self,
        coordinator: HPA300Coordinator,
        description: HPA300BinarySensorDescription,
    ) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    @property
    def is_on(self) -> bool:
        return self.entity_description.value_fn(self.coordinator.data)
