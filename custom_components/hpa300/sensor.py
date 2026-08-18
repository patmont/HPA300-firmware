"""Sensor platform for HPA300."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorEntityDescription,
)
from homeassistant.const import (
    EntityCategory,
    UnitOfInformation,
    UnitOfTime,
)
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import HPA300ConfigEntry
from .coordinator import HPA300Coordinator, HPA300Data
from .entity import HPA300Entity


def _nested(data: dict[str, Any], *path: str) -> Any:
    value: Any = data
    for key in path:
        if not isinstance(value, dict):
            return None
        value = value.get(key)
    return value


@dataclass(frozen=True, kw_only=True)
class HPA300SensorDescription(SensorEntityDescription):
    value_fn: Callable[[HPA300Data], Any]
    attributes_fn: Callable[[HPA300Data], dict[str, Any]] | None = None


SENSORS: tuple[HPA300SensorDescription, ...] = (
    HPA300SensorDescription(
        key="activity",
        translation_key="activity",
        icon="mdi:history",
        value_fn=lambda data: _nested(data.device, "last_event", "type") or "unknown",
        attributes_fn=lambda data: data.device.get("last_event", {}),
    ),
    HPA300SensorDescription(
        key="control_source",
        translation_key="control_source",
        icon="mdi:remote",
        value_fn=lambda data: data.state.get("source"),
    ),
    HPA300SensorDescription(
        key="timer",
        translation_key="timer",
        icon="mdi:timer-outline",
        value_fn=lambda data: data.state.get("timer"),
    ),
    HPA300SensorDescription(
        key="control_state",
        translation_key="control_state",
        entity_category=EntityCategory.DIAGNOSTIC,
        icon="mdi:state-machine",
        value_fn=lambda data: data.state.get("control_state"),
    ),
    HPA300SensorDescription(
        key="last_boot",
        translation_key="last_boot",
        entity_category=EntityCategory.DIAGNOSTIC,
        icon="mdi:restart-alert",
        value_fn=lambda data: _nested(data.device, "last_boot", "reason"),
        attributes_fn=lambda data: data.device.get("last_boot", {}),
    ),
    HPA300SensorDescription(
        key="uptime",
        translation_key="uptime",
        device_class=SensorDeviceClass.DURATION,
        native_unit_of_measurement=UnitOfTime.SECONDS,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda data: _nested(data.device, "runtime", "uptime_seconds"),
    ),
    HPA300SensorDescription(
        key="free_heap",
        translation_key="free_heap",
        device_class=SensorDeviceClass.DATA_SIZE,
        native_unit_of_measurement=UnitOfInformation.BYTES,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda data: _nested(data.device, "runtime", "free_heap_bytes"),
    ),
    HPA300SensorDescription(
        key="minimum_free_heap",
        translation_key="minimum_free_heap",
        device_class=SensorDeviceClass.DATA_SIZE,
        native_unit_of_measurement=UnitOfInformation.BYTES,
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=lambda data: _nested(data.device, "runtime", "minimum_free_heap_bytes"),
    ),
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: HPA300ConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    """Set up HPA300 sensors."""
    async_add_entities(
        HPA300Sensor(entry.runtime_data, description) for description in SENSORS
    )


class HPA300Sensor(HPA300Entity, SensorEntity):
    """A sensor backed by coordinator data."""

    entity_description: HPA300SensorDescription

    def __init__(
        self,
        coordinator: HPA300Coordinator,
        description: HPA300SensorDescription,
    ) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    @property
    def native_value(self) -> Any:
        return self.entity_description.value_fn(self.coordinator.data)

    @property
    def extra_state_attributes(self) -> dict[str, Any] | None:
        if self.entity_description.attributes_fn is None:
            return None
        return self.entity_description.attributes_fn(self.coordinator.data)
