#include "services/control_mode_store.h"

#include "nvs.h"
#include "esp_log.h"

static const char* TAG = "control_mode_store";
static constexpr const char* NVS_NAMESPACE = "ctrl_mode";
static constexpr const char* NVS_KEY_MODE = "mode";

esp_err_t control_mode_store_set(PersistedControlMode mode)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, NVS_KEY_MODE, static_cast<uint8_t>(mode));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs write/commit failed: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t control_mode_store_get(PersistedControlMode* mode_out)
{
    if (mode_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t raw_mode = static_cast<uint8_t>(PERSISTED_MODE_WAKE);
    err = nvs_get_u8(handle, NVS_KEY_MODE, &raw_mode);
    nvs_close(handle);

    if (err != ESP_OK) {
        return err;
    }

    if (raw_mode == static_cast<uint8_t>(PERSISTED_MODE_SLEEP)) {
        *mode_out = PERSISTED_MODE_SLEEP;
    } else {
        *mode_out = PERSISTED_MODE_WAKE;
    }

    return ESP_OK;
}
