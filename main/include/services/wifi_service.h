#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

void wifi_service_init(QueueHandle_t printQueue);
