#pragma once

#include <cstdint>
#include "esp_err.h"

enum PersistedControlMode : uint8_t {
    PERSISTED_MODE_WAKE = 0,
    PERSISTED_MODE_SLEEP = 1,
};

esp_err_t control_mode_store_set(PersistedControlMode mode);
esp_err_t control_mode_store_get(PersistedControlMode* mode_out);
