#include "fan_transition_core.h"

esp_err_t fan_transition_execute(bool turn_off, uint8_t address,
                                 const fan_transition_backend_t *backend,
                                 fan_transition_phase_t *phase)
{
    if (phase != NULL) {
        *phase = FAN_TRANSITION_PHASE_NONE;
    }
    if (backend == NULL || backend->enable == NULL || backend->set_address == NULL ||
        address > 7) {
        return ESP_ERR_INVALID_ARG;
    }
    if (phase != NULL) {
        *phase = FAN_TRANSITION_PHASE_DISABLE;
    }
    esp_err_t err = backend->enable(backend->context, false);
    if (err != ESP_OK) {
        backend->enable(backend->context, false);
        return err;
    }
    if (turn_off) {
        if (phase != NULL) {
            *phase = FAN_TRANSITION_PHASE_NONE;
        }
        return ESP_OK;
    }
    if (phase != NULL) {
        *phase = FAN_TRANSITION_PHASE_ADDRESS;
    }
    err = backend->set_address(backend->context, address);
    if (err != ESP_OK) {
        backend->enable(backend->context, false);
        return err;
    }
    if (phase != NULL) {
        *phase = FAN_TRANSITION_PHASE_ENABLE;
    }
    err = backend->enable(backend->context, true);
    if (err != ESP_OK) {
        backend->enable(backend->context, false);
        return err;
    }
    if (phase != NULL) {
        *phase = FAN_TRANSITION_PHASE_NONE;
    }
    return ESP_OK;
}
