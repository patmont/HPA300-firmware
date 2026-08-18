"""Constants for the HPA300 integration."""

from datetime import timedelta

DOMAIN = "hpa300"
PLATFORMS = ["binary_sensor", "fan", "sensor"]

CONF_API_TOKEN = "api_token"

STATE_POLL_INTERVAL = timedelta(seconds=5)
DEVICE_POLL_INTERVAL = timedelta(seconds=30)
