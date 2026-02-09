#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_event.h>

// main -> tasks
constexpr EventBits_t BIT_REQ_SLEEP = (1 << 0);
constexpr EventBits_t BIT_REQ_OTA = (1 << 1);
constexpr EventBits_t BIT_REQ_BARCODE_SCANNER_CONF = (1 << 2);

// tasks -> main
constexpr EventBits_t BIT_ACK_DISPLAY = (1 << 3);
constexpr EventBits_t BIT_ACK_BARCODE = (1 << 4);

ESP_EVENT_DECLARE_BASE(APP_EVENT);

enum {
    APP_EVENT_BARCODE_SCANNED = 1,
};

struct ScanEvent {
    char barcode[32];
};

enum class ControlType {
    WAKE,
    SLEEP,
    FIRMWARE,
    SCANNER_CONF
};

struct ControlMessage {
    ControlType type;
    char payload[128];
};
