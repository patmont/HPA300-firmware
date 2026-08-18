"""Base entity for the HPA300 integration."""

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import HPA300Coordinator


class HPA300Entity(CoordinatorEntity[HPA300Coordinator]):
    """Base for entities belonging to one HPA300 device."""

    _attr_has_entity_name = True

    def __init__(self, coordinator: HPA300Coordinator, key: str) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = f"{coordinator.data.device['id']}_{key}"

    @property
    def device_info(self) -> DeviceInfo:
        """Return the Home Assistant device registry information."""
        device = self.coordinator.data.device
        return DeviceInfo(
            identifiers={(DOMAIN, device["id"])},
            name=device.get("name", "HPA300"),
            manufacturer="Honeywell (custom controller)",
            model=device.get("model", "HPA300"),
            serial_number=device["id"],
            sw_version=device.get("firmware_version"),
            configuration_url=f"http://{self.coordinator.api.host}/update",
        )

