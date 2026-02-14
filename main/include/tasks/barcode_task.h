#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

class BarcodeDevice;

struct BarcodeTaskParams {
    QueueHandle_t printQueue;
    EventGroupHandle_t eventGroup;
    BarcodeDevice& device;
};

[[noreturn]] void barcode_task(void* pvParameters);
