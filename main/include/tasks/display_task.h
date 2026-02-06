#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

struct DisplayTaskParams {
    QueueHandle_t printQueue;
    EventGroupHandle_t eventGroup;
};

[[noreturn]] void display_task(void *pvParameters);
