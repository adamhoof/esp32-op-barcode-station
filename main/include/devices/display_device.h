#pragma once

#include "esp_err.h"

class DisplayDevice {
public:
    DisplayDevice();

    esp_err_t init();
    esp_err_t sleep();
    void deinit();

    bool is_initialized() const { return initialized_; }

private:
    bool initialized_;
    void* io_handle_;
    void* panel_handle_;
    void* disp_;
};
