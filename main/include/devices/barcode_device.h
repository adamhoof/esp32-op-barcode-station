#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "driver/uart.h"
#include "esp_err.h"

class BarcodeDevice {
public:
    explicit BarcodeDevice(uart_port_t port = UART_NUM_1);

    esp_err_t init();
    void deinit();

    esp_err_t wake();
    esp_err_t sleep();
    esp_err_t prepare_for_deep_sleep();
    esp_err_t configure();

    int read_bytes(uint8_t* dst, size_t len, TickType_t timeout_ticks) const;
    void flush_input() const;

    bool is_initialized() const { return initialized_; }

private:
    esp_err_t ensure_initialized();
    void send_cmd(const uint8_t* cmd, size_t len) const;

    uart_port_t port_;
    bool initialized_;
};
