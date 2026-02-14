#include <array>
#include <cstring>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "freertos/event_groups.h"
#include "services/wifi_service.h"
#include "services/mqtt_service.h"
#include "tasks/display_task.h"
#include "tasks/barcode_task.h"
#include "tasks/ota_task.h"
#include "devices/display_device.h"
#include "devices/barcode_device.h"
#include "events.h"
#include "print_message.h"
#include "services/control_mode_store.h"

static const char* TAG = "main";

static const char* control_type_to_string(const ControlType type)
{
    switch (type) {
        case ControlType::WAKE: return "WAKE";
        case ControlType::SLEEP: return "SLEEP";
        case ControlType::FIRMWARE: return "FIRMWARE";
        case ControlType::SCANNER_CONF: return "SCANNER_CONF";
        case ControlType::MQTT_UNREACHABLE: return "MQTT_UNREACHABLE";
        case ControlType::WIFI_CONNECTED: return "WIFI_CONNECTED";
        default: return "UNKNOWN";
    }
}

static const char* active_tasks_to_string(const EventBits_t task_bits)
{
    if (task_bits == 0) {
        return "none";
    }
    if ((task_bits & (BIT_ACK_DISPLAY | BIT_ACK_BARCODE)) == (BIT_ACK_DISPLAY | BIT_ACK_BARCODE)) {
        return "display, barcode";
    }
    if ((task_bits & BIT_ACK_DISPLAY) != 0) {
        return "display";
    }
    if ((task_bits & BIT_ACK_BARCODE) != 0) {
        return "barcode";
    }
    return "unknown";
}

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

static void send_nvs_error(QueueHandle_t printQueue, const char* action, esp_err_t err)
{
    PrintMessage err_msg{};
    err_msg.type = ERROR_MSG;
    snprintf(err_msg.data.error.msg,
             sizeof(err_msg.data.error.msg),
             "NVS error (%s): %s",
             action,
             esp_err_to_name(err));
    xQueueOverwrite(printQueue, &err_msg);
}

static void enforce_devices_sleep(DisplayDevice& display_device, BarcodeDevice& barcode_device)
{
    const esp_err_t display_err = display_device.sleep();
    if (display_err != ESP_OK) {
        ESP_LOGW(TAG, "Display sleep failed: %s", esp_err_to_name(display_err));
    }

    const esp_err_t barcode_err = barcode_device.sleep();
    if (barcode_err != ESP_OK) {
        ESP_LOGW(TAG, "Barcode sleep failed: %s", esp_err_to_name(barcode_err));
    }

    const esp_err_t barcode_prepare_err = barcode_device.prepare_for_deep_sleep();
    if (barcode_prepare_err != ESP_OK) {
        ESP_LOGW(TAG, "Barcode deep sleep preparation failed: %s", esp_err_to_name(barcode_prepare_err));
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
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    static QueueHandle_t printQueue = xQueueCreate(1, sizeof(PrintMessage));
    static QueueHandle_t controlQueue = xQueueCreate(3, sizeof(ControlMessage));
    static EventGroupHandle_t eventGroup = xEventGroupCreate();

    wifi_service_init(printQueue, controlQueue);

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();

    static DisplayDevice display_device;
    static BarcodeDevice barcode_device;

    static DisplayTaskParams display_params {
        .printQueue = printQueue,
        .eventGroup = eventGroup,
        .device = display_device,
    };

    static BarcodeTaskParams barcode_params {
        .printQueue = printQueue,
        .eventGroup = eventGroup,
        .device = barcode_device,
    };

    static OtaTaskParams ota_params { .eventGroup = eventGroup, .url = "" };

    static TaskHandle_t h_display = nullptr;
    static TaskHandle_t h_barcode = nullptr;

    xTaskCreate(display_task, "display", 4096, &display_params, 5, &h_display);

    ControlMessage msg{};

    for (;;) {
        if (xQueueReceive(controlQueue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGD(TAG, "Received Control Type: %s", control_type_to_string(msg.type));

        EventBits_t task_bits = 0;
        if (h_display != nullptr) task_bits |= BIT_ACK_DISPLAY;
        if (h_barcode != nullptr) task_bits |= BIT_ACK_BARCODE;

        ESP_LOGD(TAG, "Active Tasks: %s", active_tasks_to_string(task_bits));

        switch (msg.type) {
                case ControlType::WAKE: {
                    const esp_err_t persist_err = control_mode_store_set(PERSISTED_MODE_WAKE);
                    if (persist_err != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to persist WAKE mode: %s", esp_err_to_name(persist_err));
                        send_nvs_error(printQueue, "set wake", persist_err);
                    }
                    xEventGroupClearBits(eventGroup, BIT_REQ_STOP | task_bits);
                    if (h_display == nullptr) {
                        xTaskCreate(display_task, "display", 4096, &display_params, 5, &h_display);
                    }
                    if (h_barcode == nullptr) {
                        xTaskCreate(barcode_task, "barcode", 4096, &barcode_params, 5, &h_barcode);
                    }
                    break;
                }

                case ControlType::SLEEP: {
                    const esp_err_t persist_err = control_mode_store_set(PERSISTED_MODE_SLEEP);
                    if (persist_err != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to persist SLEEP mode: %s", esp_err_to_name(persist_err));
                        send_nvs_error(printQueue, "set sleep", persist_err);
                    }
                    xEventGroupClearBits(eventGroup, task_bits);
                    xEventGroupSetBits(eventGroup, BIT_REQ_STOP);
                    const EventBits_t acked = xEventGroupWaitBits(eventGroup, task_bits, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
                    if ((acked & task_bits) != task_bits) {
                        ESP_LOGE(TAG, "SLEEP: task ACK timeout (got 0x%lx, expected 0x%lx), forcing sleep",
                                 (unsigned long)acked, (unsigned long)task_bits);
                    }

                    enforce_devices_sleep(display_device, barcode_device);

                    enter_deep_sleep(CONFIG_DEEP_SLEEP_DURATION);
                    break;
                }

                case ControlType::MQTT_UNREACHABLE: {
                    PersistedControlMode persisted_mode = PERSISTED_MODE_WAKE;
                    const esp_err_t err = control_mode_store_get(&persisted_mode);
                    if (err == ESP_ERR_NVS_NOT_FOUND) {
                        ESP_LOGW(TAG, "No persisted mode found, defaulting to WAKE");
                    } else if (err != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to read persisted mode (%s), defaulting to WAKE", esp_err_to_name(err));
                        send_nvs_error(printQueue, "read mode", err);
                    }

                    ControlMessage fallback_msg{};
                    fallback_msg.type = (persisted_mode == PERSISTED_MODE_SLEEP)
                        ? ControlType::SLEEP
                        : ControlType::WAKE;
                    fallback_msg.payload[0] = '\0';

                    ESP_LOGD(TAG, "Unreachable fallback: %s",
                             (fallback_msg.type == ControlType::SLEEP) ? "SLEEP" : "WAKE");

                    if (xQueueSend(controlQueue, &fallback_msg, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "Failed to enqueue unreachable fallback control message");
                    }
                    break;
                }

                case ControlType::FIRMWARE: {
                    xEventGroupClearBits(eventGroup, task_bits);
                    xEventGroupSetBits(eventGroup, BIT_REQ_STOP);
                    const EventBits_t acked = xEventGroupWaitBits(eventGroup, task_bits, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
                    if ((acked & task_bits) != task_bits) {
                        ESP_LOGE(TAG, "FIRMWARE: task ACK timeout (got 0x%lx, expected 0x%lx), proceeding with OTA",
                                 (unsigned long)acked, (unsigned long)task_bits);
                    }

                    // free ram so that another tls handshake can happen safely, smol
                    delete_task_safe(h_display);
                    delete_task_safe(h_barcode);

                    display_device.deinit();
                    barcode_device.deinit();

                    mqtt_service_stop();
                    vTaskDelay(pdMS_TO_TICKS(500));

                    strlcpy(ota_params.url, msg.payload, sizeof(ota_params.url));

                    xTaskCreate(ota_task, "ota", 8192, &ota_params, 3, nullptr);
                    break;
                }

                case ControlType::SCANNER_CONF: {
                    if (h_barcode == nullptr) {
                        ESP_LOGE(TAG, "No Barcode Task Running!");
                        break;
                    }
                    ESP_LOGI(TAG, "Initiating Scanner Configuration...");
                    xEventGroupClearBits(eventGroup, BIT_ACK_BARCODE);
                    xEventGroupSetBits(eventGroup, BIT_REQ_BARCODE_SCANNER_CONF);
                    const EventBits_t acked = xEventGroupWaitBits(eventGroup, BIT_ACK_BARCODE, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
                    if ((acked & BIT_ACK_BARCODE) != BIT_ACK_BARCODE) {
                        ESP_LOGE(TAG, "SCANNER_CONF: barcode task ACK timeout");
                    }
                    ESP_LOGI(TAG, "Scanner Configuration & Save Completed.");
                    break;
                }

                case ControlType::WIFI_CONNECTED: {
                    mqtt_service_init(printQueue, controlQueue);
                    break;
                }
            }
    }
}
