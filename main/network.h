#pragma once

#include "esp_err.h"

// Starts Wi-Fi, fallback provisioning, and the local HTTP API. This subsystem
// is optional: callers should preserve local appliance operation if it fails.
esp_err_t network_init(void);

// Opens the setup AP after a deliberate physical gesture. Existing station
// connectivity remains active until new settings are submitted.
esp_err_t network_start_provisioning(void);
