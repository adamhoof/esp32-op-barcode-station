#include <array>
#include <cstring>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "freertos/event_groups.h"
#include "services/wifi_service.h"
#include "services/mqtt_service.h"
#include "tasks/display_task.h"
#include "tasks/barcode_task.h"
#include "tasks/ota_task.h"
#include "events.h"
#include "print_message.h"

static const char* TAG = "main";

static void init_system()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
}

static void enter_deep_sleep(const uint64_t durationSec)
{
    ESP_LOGI(TAG, "Entering deep sleep...");
    if (durationSec > 0) {
        esp_sleep_enable_timer_wakeup(durationSec* 1000000ULL);
    }
    esp_deep_sleep_start();
}

static void delete_task_safe(TaskHandle_t& handle_ref)
{
    if (handle_ref != nullptr) {
        vTaskDelete(handle_ref);
        handle_ref = nullptr;
    }
}

void time_sync_cb(struct timeval *tv)
{
    time_t now;
    struct tm timeinfo{};
    time(&now);
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Time sync event hit, current time: %s", strftime_buf);
}

extern "C" [[noreturn]] void app_main(void)
{
    init_system();

    static QueueHandle_t printQueue = xQueueCreate(1, sizeof(PrintMessage));
    static QueueHandle_t controlQueue = xQueueCreate(3, sizeof(ControlMessage));
    static EventGroupHandle_t eventGroup = xEventGroupCreate();

    wifi_service_init(printQueue);
    mqtt_service_init(printQueue, controlQueue);

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();

    static DisplayTaskParams display_params { .printQueue = printQueue, .eventGroup = eventGroup };
    static BarcodeTaskParams barcode_params { .printQueue = printQueue, .eventGroup = eventGroup };
    static OtaTaskParams ota_params { .eventGroup = eventGroup, .url = "" };

    static TaskHandle_t h_display = nullptr;
    static TaskHandle_t h_barcode = nullptr;

    xTaskCreate(display_task, "display", 4096, &display_params, 5, &h_display);

    ControlMessage msg{};

    for (;;) {
        if (xQueueReceive(controlQueue, &msg, portMAX_DELAY) == pdTRUE) {

            ESP_LOGI(TAG, "Received Control Type: %d", (int)msg.type);

            EventBits_t task_bits = 0;
            if (h_display != nullptr) task_bits |= BIT_ACK_DISPLAY;
            if (h_barcode != nullptr) task_bits |= BIT_ACK_BARCODE;

            ESP_LOGI(TAG, "Active Tasks: %d", task_bits);

            switch (msg.type) {
                case ControlType::WAKE:
                    xEventGroupClearBits(eventGroup, BIT_REQ_SLEEP | BIT_REQ_OTA | task_bits);
                    if (h_display == nullptr) {
                        xTaskCreate(display_task, "display", 4096, &display_params, 5, &h_display);
                    }
                    if (h_barcode == nullptr) {
                        xTaskCreate(barcode_task, "barcode", 4096, &barcode_params, 5, &h_barcode);
                    }
                    break;

                case ControlType::SLEEP:
                    xEventGroupClearBits(eventGroup, task_bits);
                    xEventGroupSetBits(eventGroup, BIT_REQ_SLEEP);
                    xEventGroupWaitBits(eventGroup, task_bits, pdFALSE, pdTRUE, portMAX_DELAY);
                    enter_deep_sleep(CONFIG_DEEP_SLEEP_DURATION);
                    break;

                case ControlType::FIRMWARE:
                    xEventGroupClearBits(eventGroup, task_bits);
                    xEventGroupSetBits(eventGroup, BIT_REQ_OTA);
                    xEventGroupWaitBits(eventGroup, task_bits, pdFALSE, pdTRUE, portMAX_DELAY);

                    // free ram so that another tls handshake can happen safely, smol
                    delete_task_safe(h_display);
                    delete_task_safe(h_barcode);
                    mqtt_service_stop();
                    vTaskDelay(pdMS_TO_TICKS(500));

                    strlcpy(ota_params.url, msg.payload, sizeof(ota_params.url));

                    xTaskCreate(ota_task, "ota", 8192, &ota_params, 3, nullptr);
                    break;

                case ControlType::SCANNER_CONF:
                    if (h_barcode == nullptr)  {
                        ESP_LOGE(TAG, "No Barcode Task Running!");
                        break;
                    }
                    ESP_LOGI(TAG, "Initiating Scanner Configuration...");
                    xEventGroupClearBits(eventGroup, BIT_ACK_BARCODE);
                    xEventGroupSetBits(eventGroup, BIT_REQ_BARCODE_SCANNER_CONF);
                    xEventGroupWaitBits(eventGroup, BIT_ACK_BARCODE, pdFALSE, pdTRUE, portMAX_DELAY);
                    ESP_LOGI(TAG, "Scanner Configuration & Save Completed.");
                    break;
            }
        }
    }
}
