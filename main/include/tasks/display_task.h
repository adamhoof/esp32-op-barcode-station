#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

class DisplayDevice;

struct DisplayTaskParams {
    QueueHandle_t printQueue;
    EventGroupHandle_t eventGroup;
    DisplayDevice& device;
};

[[noreturn]] void display_task(void *pvParameters);
