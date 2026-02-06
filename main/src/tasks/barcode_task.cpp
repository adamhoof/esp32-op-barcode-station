#include "tasks/barcode_task.h"
#include <cctype>
#include "events.h"
#include "print_message.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"

static const char *TAG = "BARCODE";

static bool is_numeric(const char *s) {
    if (s == nullptr || *s == '\0') return false;
    for (size_t i = 0; s[i] != '\0'; ++i) {
        if (!isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

[[noreturn]] void barcode_task(void *pvParameters) {
    const auto *params = static_cast<const BarcodeTaskParams *>(pvParameters);

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

    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &cfg));

    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1,
                                 CONFIG_BARCODE_TX_PIN,
                                 CONFIG_BARCODE_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 256, 0, 0, nullptr, 0));

    gpio_set_pull_mode((gpio_num_t)CONFIG_BARCODE_RX_PIN, GPIO_PULLUP_ONLY);
    uart_flush_input(UART_NUM_1);

    const uint8_t wake_byte = 0x00;
    uart_write_bytes(UART_NUM_1, &wake_byte, 1);

    uint8_t rx[64];
    char buffer[CONFIG_MAX_BARCODE_BUFFER_SIZE + 1];
    size_t buffer_occupancy = 0;
    bool overflow = false;

    ESP_LOGD(TAG, "Barcode Task Started on UART1 (TX:%d, RX:%d)", CONFIG_BARCODE_TX_PIN, CONFIG_BARCODE_RX_PIN);

    for (;;) {
        const EventBits_t req_bits = xEventGroupWaitBits(
            params->eventGroup,
            BIT_REQ_SLEEP | BIT_REQ_OTA,
            pdFALSE,
            pdFALSE,
            0
        );

        if ((req_bits & BIT_REQ_SLEEP) != 0) {
            static const uint8_t sleep_cmd[] = {0x7E, 0x00, 0x08, 0x01, 0x00, 0xD9, 0xA5, 0xAB, 0xCD};
            uart_write_bytes(UART_NUM_1, sleep_cmd, sizeof(sleep_cmd));
            uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(100));
            xEventGroupSetBits(params->eventGroup, BIT_ACK_BARCODE);
            vTaskSuspend(nullptr);
        }

        if ((req_bits & BIT_REQ_OTA) != 0) {
            uart_driver_delete(UART_NUM_1);
            xEventGroupSetBits(params->eventGroup, BIT_ACK_BARCODE);
            vTaskSuspend(nullptr);
        }

        const int n = uart_read_bytes(UART_NUM_1, rx, sizeof(rx), pdMS_TO_TICKS(50));

        if (n < 0) {
            ESP_LOGE(TAG, "UART Read Error");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (n == 0) {
            continue;
        }

        for (int i = 0; i < n; ++i) {
            const char c = static_cast<char>(rx[i]);

            ESP_LOGD(TAG, "Char: %c (%02x)", (c >= 32 && c <= 126) ? c : '.', c);

            if (c != CONFIG_BARCODE_DELIMITER && c != '\n' && c != '\r') {
                if (buffer_occupancy < CONFIG_MAX_BARCODE_BUFFER_SIZE) {
                    buffer[buffer_occupancy++] = c;
                } else {
                    overflow = true;
                }
                continue;
            }

            if (buffer_occupancy == 0) {
                continue;
            }

            if (overflow) {
                PrintMessage msg{};
                msg.type = ERROR_MSG;
                strlcpy(msg.data.error.msg, "Barcode too long", sizeof(msg.data.error.msg));
                xQueueSend(params->printQueue, &msg, 0);

                buffer_occupancy = 0;
                overflow = false;
                continue;
            }

            buffer[buffer_occupancy] = '\0';
            ESP_LOGI(TAG, "Scanned: %s", buffer);

            if (is_numeric(buffer)) {
                ScanEvent evt{};
                strlcpy(evt.barcode, buffer, sizeof(evt.barcode));
                esp_event_post(APP_EVENT, APP_EVENT_BARCODE_SCANNED, &evt, sizeof(evt), portMAX_DELAY);
            } else {
                PrintMessage msg{};
                msg.type = ERROR_MSG;
                strlcpy(msg.data.error.msg, "Zkuste prosim znovu...", sizeof(msg.data.error.msg));
                xQueueSend(params->printQueue, &msg, 0);
            }
            buffer_occupancy = 0;
        }
    }
}