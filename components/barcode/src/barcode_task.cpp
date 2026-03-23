#include "barcode_task.h"
#include <cctype>
#include "events.h"
#include "print_message.h"
#include "barcode_device.h"
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

static void process_rx_chunk(const BarcodeTaskParams* params,
                             const uint8_t* rx,
                             const int n,
                             char* buffer,
                             size_t& buffer_occupancy,
                             bool& overflow)
{
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
        ESP_LOGD(TAG, "Scanned: %s", buffer);

        if (is_numeric(buffer)) {
            ScanEvent evt{};
            strlcpy(evt.barcode, buffer, sizeof(evt.barcode));
            if (esp_event_post(APP_EVENT, APP_EVENT_BARCODE_SCANNED, &evt, sizeof(evt), pdMS_TO_TICKS(3000)) != ESP_OK) {
                ESP_LOGW(TAG, "Event post timed out, barcode dropped");
            }
        } else {
            PrintMessage msg{};
            msg.type = ERROR_MSG;
            strlcpy(msg.data.error.msg, "Zkuste prosim znovu...", sizeof(msg.data.error.msg));
            xQueueSend(params->printQueue, &msg, 0);
        }
        buffer_occupancy = 0;
    }
}

[[noreturn]] void barcode_task(void *pvParameters) {
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    ESP_LOGD(TAG, "Barcode task started");

    const auto* params = static_cast<const BarcodeTaskParams*>(pvParameters);
    BarcodeDevice& device = params->device;

    ESP_ERROR_CHECK(device.init());
    ESP_ERROR_CHECK(device.wake());

    uint8_t rx[64];
    char buffer[CONFIG_MAX_BARCODE_BUFFER_SIZE + 1];
    size_t buffer_occupancy = 0;
    bool overflow = false;

    for (;;) {
        const EventBits_t req_bits = xEventGroupWaitBits(
            params->eventGroup,
            BIT_REQ_STOP | BIT_REQ_BARCODE_SCANNER_CONF,
            pdFALSE,
            pdFALSE,
            0
        );

        if ((req_bits & BIT_REQ_STOP) != 0) {
            const int n_pending = device.read_bytes(rx, sizeof(rx), 0);
            if (n_pending > 0) {
                process_rx_chunk(params, rx, n_pending, buffer, buffer_occupancy, overflow);
            }

            ESP_LOGI(TAG, "Barcode task ready, acknowledging STOP and suspending");
            xEventGroupSetBits(params->eventGroup, BIT_ACK_BARCODE);
            vTaskSuspend(nullptr);
        }

        if ((req_bits & BIT_REQ_BARCODE_SCANNER_CONF) != 0) {
            ESP_LOGW(TAG, "STARTING SCANNER CONFIGURATION...");

            (void)device.configure();

            ESP_LOGW(TAG, "SCANNER CONFIG COMPLETE. Reading response buffer...");

            uint8_t dump[256];
            int len = device.read_bytes(dump, sizeof(dump), pdMS_TO_TICKS(200));
            if (len > 0) {
                ESP_LOG_BUFFER_HEX(TAG, dump, len);
            }

            xEventGroupClearBits(params->eventGroup, BIT_REQ_BARCODE_SCANNER_CONF);
            xEventGroupSetBits(params->eventGroup, BIT_ACK_BARCODE);
        }

        const int n = device.read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(50));

        if (n < 0) {
            ESP_LOGE(TAG, "UART Read Error");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (n == 0) {
            continue;
        }

        process_rx_chunk(params, rx, n, buffer, buffer_occupancy, overflow);
    }
}