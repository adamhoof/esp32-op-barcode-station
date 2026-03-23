#pragma once

#include <freertos/FreeRTOS.h>

struct OtaTaskParams {
    EventGroupHandle_t eventGroup;
    char url[128];
};


[[noreturn]] void ota_task(void *pvParameters);
