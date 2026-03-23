#pragma once

#include <cstdint>
#include "product_data.h"

enum PrintMessageType {
    PRODUCT_DATA,
    WIFI_STATUS,
    MQTT_STATUS,
    ERROR_MSG,
};

struct WifiStatusPayload {
    bool connected;
    uint8_t ipLastOctet;
};

struct MqttStatusPayload {
    bool connected;
};

struct ErrorPayload {
    char msg[128];
};

struct PrintMessage {
    PrintMessageType type;
    union {
        ProductData product;
        WifiStatusPayload wifi;
        MqttStatusPayload mqtt;
        ErrorPayload error;
    } data;
};