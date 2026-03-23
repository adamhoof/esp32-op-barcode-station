#include "barcode_device.h"

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char* TAG = "BARCODE_DEVICE";

static const uint8_t CMD_SCANNER_SLEEP[] = {0x7E, 0x00, 0x08, 0x01, 0x00, 0xD9, 0xA5, 0xAB, 0xCD};
static const uint8_t CMD_SCANNER_SENSITIVITY[] = {0x7E, 0x00, 0x08, 0x01, 0x00, 0x0F, 0x60, 0xAB, 0xCD};
static const uint8_t CMD_SCANNER_SAVE[] = {0x7E, 0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0xAB, 0xCD};

BarcodeDevice::BarcodeDevice(const uart_port_t port)
    : port_(port), initialized_(false) {}

esp_err_t BarcodeDevice::init()
{
    if (initialized_) {
        return ESP_OK;
    }

    const uart_config_t cfg = {
        .baud_rate = CONFIG_BARCODE_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {
            .allow_pd = 0,
            .backup_before_sleep = 0
        }
    };

    esp_err_t err = uart_param_config(port_, &cfg);
    if (err != ESP_OK) return err;

    err = uart_set_pin(port_,
                       CONFIG_BARCODE_TX_PIN,
                       CONFIG_BARCODE_RX_PIN,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    err = uart_driver_install(port_, 256, 0, 0, nullptr, 0);
    if (err != ESP_OK) return err;

    gpio_set_pull_mode((gpio_num_t)CONFIG_BARCODE_RX_PIN, GPIO_PULLUP_ONLY);
    uart_flush_input(port_);

    initialized_ = true;
    return ESP_OK;
}

void BarcodeDevice::deinit()
{
    if (!initialized_) {
        return;
    }
    uart_driver_delete(port_);
    initialized_ = false;
}

esp_err_t BarcodeDevice::ensure_initialized()
{
    if (initialized_) {
        return ESP_OK;
    }
    return init();
}

void BarcodeDevice::send_cmd(const uint8_t* cmd, const size_t len) const
{
    if (!initialized_) {
        return;
    }
    uart_write_bytes(port_, reinterpret_cast<const char*>(cmd), len);
    vTaskDelay(pdMS_TO_TICKS(100));
}

esp_err_t BarcodeDevice::wake()
{
    gpio_hold_dis(static_cast<gpio_num_t>(CONFIG_BARCODE_TX_PIN));
    gpio_hold_dis(static_cast<gpio_num_t>(CONFIG_BARCODE_RX_PIN));

    const esp_err_t err = ensure_initialized();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wake: init failed: %s", esp_err_to_name(err));
        return err;
    }

    const uint8_t wake_byte = 0x00;
    uart_write_bytes(port_, reinterpret_cast<const char*>(&wake_byte), 1);
    return ESP_OK;
}

esp_err_t BarcodeDevice::sleep()
{
    const esp_err_t err = ensure_initialized();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sleep: init failed: %s", esp_err_to_name(err));
        return err;
    }

    send_cmd(CMD_SCANNER_SLEEP, sizeof(CMD_SCANNER_SLEEP));
    const int tx_done = uart_wait_tx_done(port_, pdMS_TO_TICKS(200));
    if (tx_done != ESP_OK) {
        ESP_LOGW(TAG, "sleep: uart_wait_tx_done timed out");
    }
    return ESP_OK;
}

esp_err_t BarcodeDevice::prepare_for_deep_sleep()
{
    const esp_err_t err = ensure_initialized();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "prepare_for_deep_sleep: init failed: %s", esp_err_to_name(err));
        return err;
    }

    const gpio_num_t tx_pin = static_cast<gpio_num_t>(CONFIG_BARCODE_TX_PIN);
    const gpio_num_t rx_pin = static_cast<gpio_num_t>(CONFIG_BARCODE_RX_PIN);

    gpio_set_direction(tx_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(tx_pin, 1);
    gpio_hold_en(tx_pin);

    gpio_set_direction(rx_pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(rx_pin, GPIO_PULLUP_ONLY);
    gpio_hold_en(rx_pin);

    return ESP_OK;
}

esp_err_t BarcodeDevice::configure()
{
    const esp_err_t err = ensure_initialized();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configure: init failed: %s", esp_err_to_name(err));
        return err;
    }

    send_cmd(CMD_SCANNER_SENSITIVITY, sizeof(CMD_SCANNER_SENSITIVITY));
    send_cmd(CMD_SCANNER_SAVE, sizeof(CMD_SCANNER_SAVE));
    return ESP_OK;
}

int BarcodeDevice::read_bytes(uint8_t* dst, const size_t len, const TickType_t timeout_ticks) const
{
    if (!initialized_) {
        return -1;
    }
    return uart_read_bytes(port_, dst, len, timeout_ticks);
}

void BarcodeDevice::flush_input() const
{
    if (initialized_) {
        uart_flush_input(port_);
    }
}
