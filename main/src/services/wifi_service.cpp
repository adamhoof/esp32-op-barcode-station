#include "services/wifi_service.h"
#include <cstring>
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_sntp.h"

#include "print_message.h"
#include "events.h"

static const char *TAG = "wifi_service";

constexpr int WIFI_MAX_FAILURES = 3;

static struct {
    QueueHandle_t print_queue{};
    QueueHandle_t control_queue{};
    int disconnect_count{0};
    bool unreachable_notified{false};
} s_ctx;

static void send_wifi_status(bool connected, uint8_t last_octet = 0)
{
    PrintMessage msg{};
    msg.type = WIFI_STATUS;
    msg.data.wifi.connected = connected;
    msg.data.wifi.ipLastOctet = last_octet;

    xQueueSend(s_ctx.print_queue, &msg, 0);
}

static void publish_control(ControlType type) {
    if (s_ctx.control_queue == nullptr) return;
    ControlMessage msg{};
    msg.type = type;
    msg.payload[0] = '\0';
    xQueueSend(s_ctx.control_queue, &msg, 0);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        }

        else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            send_wifi_status(false);

            s_ctx.disconnect_count++;
            if (s_ctx.disconnect_count >= WIFI_MAX_FAILURES && !s_ctx.unreachable_notified) {
                ESP_LOGW(TAG, "WiFi failed %d times, publishing MQTT_UNREACHABLE", s_ctx.disconnect_count);
                publish_control(ControlType::MQTT_UNREACHABLE);
                s_ctx.unreachable_notified = true;
            }

            esp_wifi_connect();
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto *ev = static_cast<const ip_event_got_ip_t *>(event_data);
        const uint8_t last_octet = esp_ip4_addr4(&ev->ip_info.ip);
        send_wifi_status(true, last_octet);

        s_ctx.disconnect_count = 0;
        s_ctx.unreachable_notified = false;
        publish_control(ControlType::WIFI_CONNECTED);
    }
}

void wifi_service_init(QueueHandle_t printQueue, QueueHandle_t controlQueue)
{
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    s_ctx.print_queue = printQueue;
    s_ctx.control_queue = controlQueue;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

    wifi_config_t sta_cfg{};
    std::strncpy(reinterpret_cast<char *>(sta_cfg.sta.ssid), CONFIG_WIFI_SSID, sizeof(sta_cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(sta_cfg.sta.password), CONFIG_WIFI_PASSWORD, sizeof(sta_cfg.sta.password) - 1);

    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}
