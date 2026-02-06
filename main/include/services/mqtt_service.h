#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

void mqtt_service_init(QueueHandle_t printQueue, QueueHandle_t controlQueue);
