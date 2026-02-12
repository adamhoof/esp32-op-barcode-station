#include "services/mqtt_service.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <atomic>
#include "sdkconfig.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_event.h"
#include "print_message.h"
#include "json_parser.h"
#include "product_data.h"
#include "events.h"
#include "esp_mac.h"

extern const uint8_t ca_cert_start[]      asm("_binary_ca_crt_start");
extern const uint8_t ca_cert_end[]        asm("_binary_ca_crt_end");
extern const uint8_t client_cert_start[]  asm("_binary_client_crt_start");
extern const uint8_t client_cert_end[]    asm("_binary_client_crt_end");
extern const uint8_t client_key_start[]   asm("_binary_client_key_start");
extern const uint8_t client_key_end[]     asm("_binary_client_key_end");

ESP_EVENT_DEFINE_BASE(APP_EVENT);

static const char *TAG = "mqtt_service";

constexpr size_t MAC_HEX_LEN = 12;
constexpr size_t TOPIC_BASE_LEN = sizeof(CONFIG_MQTT_REQ_TOPIC_PREFIX) + MAC_HEX_LEN + 2;
constexpr size_t TOPIC_BUFFER_SIZE = (TOPIC_BASE_LEN + CONFIG_MAX_BARCODE_BUFFER_SIZE) * 2;

static struct {
    esp_mqtt_client_handle_t client{};
    QueueHandle_t print_queue{};
    QueueHandle_t control_queue{};
    std::atomic<bool> publish_update;
    std::atomic<bool> unreachable_notified;
    char topic_base[TOPIC_BASE_LEN]{};
    char client_id[13]{};
} s_ctx;

static bool is_broker_unreachable(const esp_mqtt_event_t* event) {
    if (event == nullptr || event->error_handle == nullptr) return false;
    if (event->error_handle->error_type != MQTT_ERROR_TYPE_TCP_TRANSPORT) return false;

    switch (const int sock_errno = event->error_handle->esp_transport_sock_errno) {
        case ENETUNREACH:
        case EHOSTUNREACH:
        case ETIMEDOUT:
        case ECONNREFUSED:
        case ENETDOWN:
        case EHOSTDOWN:
            return true;
        default:
            return false;
    }
}

static void queue_mqtt_status(bool connected) {
    if (!s_ctx.publish_update) return;

    PrintMessage msg{};
    msg.type = MQTT_STATUS;
    msg.data.mqtt.connected = connected;
    xQueueSend(s_ctx.print_queue, &msg, 0);

    if (connected) s_ctx.publish_update = false;
}

static void publish_control(ControlType type, const char* payload = nullptr, size_t len = 0) {
    if (s_ctx.control_queue == nullptr) return;

    ControlMessage msg{};
    msg.type = type;

    if (payload != nullptr && len > 0) {
        size_t copy_len = (len < sizeof(msg.payload) - 1) ? len : (sizeof(msg.payload) - 1);
        memcpy(msg.payload, payload, copy_len);
        msg.payload[copy_len] = '\0';
    } else {
        msg.payload[0] = '\0';
    }

    xQueueSend(s_ctx.control_queue, &msg, 0);
}

static void handle_product_json(const char* payload, size_t len) {
    char json[512];
    const size_t cpy_len = (len < sizeof(json) - 1) ? len : (sizeof(json) - 1);
    memcpy(json, payload, cpy_len);
    json[cpy_len] = '\0';

    ProductData product{};
    PrintMessage msg{};
    if (parse_product_json(json, cpy_len, &product)) {
        if (product.valid) {
            msg.type = PRODUCT_DATA;
            msg.data.product = product;
            xQueueSend(s_ctx.print_queue, &msg, 0);
        } else {
            msg.type = ERROR_MSG;
            strlcpy(msg.data.error.msg, "Zavolejte prosim obsluhu ->\nprodukt chybi v db", sizeof(msg.data.error.msg));
            xQueueSend(s_ctx.print_queue, &msg, 0);
        }
    } else {
        msg.type = ERROR_MSG;
        strlcpy(msg.data.error.msg, "Zavolejte prosim obsluhu ->\nnevalidni format dat", sizeof(msg.data.error.msg));
        xQueueSend(s_ctx.print_queue, &msg, 0);
    }
}

static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    const auto *event = static_cast<const esp_mqtt_event_t*>(event_data);

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            s_ctx.unreachable_notified = false;
            queue_mqtt_status(true);

            esp_mqtt_client_subscribe_single(event->client, s_ctx.topic_base, 1);
            esp_mqtt_client_subscribe_single(event->client, CONFIG_MQTT_TOPIC_CONTROL, 1);

            ESP_LOGI(TAG, "Subscribed to topics: '%s', '%s'", s_ctx.topic_base, CONFIG_MQTT_TOPIC_CONTROL);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED");
            queue_mqtt_status(false);
            break;

        case MQTT_EVENT_DATA:
            if (event->topic_len == static_cast<int>(strlen(s_ctx.topic_base)) &&
                memcmp(event->topic, s_ctx.topic_base, event->topic_len) == 0) {
                handle_product_json(event->data, event->data_len);
                }
            else if (event->topic_len == static_cast<int>(strlen(CONFIG_MQTT_TOPIC_CONTROL)) &&
                     memcmp(event->topic, CONFIG_MQTT_TOPIC_CONTROL, event->topic_len) == 0) {

                if (event->data_len == 4 && memcmp(event->data, "wake", 4) == 0) {
                    publish_control(ControlType::WAKE);
                }
                else if (event->data_len == 5 && memcmp(event->data, "sleep", 5) == 0) {
                    publish_control(ControlType::SLEEP);
                }
                else if (event->data_len == 12 && memcmp(event->data, "conf_scanner", 12) == 0) {
                    publish_control(ControlType::SCANNER_CONF);
                }
                else if (event->data_len > 8 && memcmp(event->data, "https://", 8) == 0) {
                    publish_control(ControlType::FIRMWARE, event->data, event->data_len);
                }
            }
            break;

        case MQTT_EVENT_ERROR:
            if (event->error_handle != nullptr &&
                event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                 ESP_LOGE(TAG, "TLS/TCP Error: 0x%x, Sock Errno: %d",
                          event->error_handle->esp_tls_last_esp_err,
                          event->error_handle->esp_transport_sock_errno);
            }

            if (is_broker_unreachable(event) && !s_ctx.unreachable_notified.exchange(true)) {
                ESP_LOGW(TAG, "Broker unreachable, publishing UNREACHABLE control event");
                publish_control(ControlType::MQTT_UNREACHABLE);
            }
            break;
        default: break;
    }
}

static void on_barcode_scanned(void* handler_args, esp_event_base_t base, int32_t id, void* event_data) {
    if (s_ctx.client == nullptr) return;

    const auto* ev = static_cast<const ScanEvent*>(event_data);

    ESP_LOGI(TAG, "Processing Barcode: %s", ev->barcode);

    static char full_topic[TOPIC_BUFFER_SIZE];

    int written = snprintf(full_topic, TOPIC_BUFFER_SIZE, "%s/%s", s_ctx.topic_base, ev->barcode);

    if (written > 0 && written < static_cast<int>(TOPIC_BUFFER_SIZE)) {
        int msg_id = esp_mqtt_client_publish(s_ctx.client, full_topic, "", 0, 1, 0);
        if (msg_id != -1) {
            ESP_LOGI(TAG, "Published to '%s' (Msg ID: %d)", full_topic, msg_id);
        } else {
            ESP_LOGE(TAG, "Publish failed");
        }
    }
}

void mqtt_service_init(QueueHandle_t printQueue, QueueHandle_t controlQueue) {
    s_ctx.print_queue = printQueue;
    s_ctx.control_queue = controlQueue;
    s_ctx.publish_update = true;
    s_ctx.unreachable_notified = false;

    uint8_t mac[6]{};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));

    snprintf(s_ctx.client_id, sizeof(s_ctx.client_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(s_ctx.topic_base, sizeof(s_ctx.topic_base), "%s/%s",
             CONFIG_MQTT_REQ_TOPIC_PREFIX, s_ctx.client_id);

    ESP_LOGI(TAG, "Device Topic Base: %s", s_ctx.topic_base);
    ESP_LOGI(TAG, "MQTT Client ID: %s", s_ctx.client_id);

    esp_mqtt_client_config_t cfg{};
    cfg.broker.address.uri = CONFIG_MQTT_BROKER_URI;
    cfg.broker.verification.certificate = (const char *)ca_cert_start;
    cfg.broker.verification.certificate_len = (ca_cert_end - ca_cert_start);
    cfg.broker.verification.skip_cert_common_name_check = false;

    cfg.credentials.authentication.certificate = (const char *)client_cert_start;
    cfg.credentials.authentication.certificate_len = (client_cert_end - client_cert_start);
    cfg.credentials.authentication.key = (const char *)client_key_start;
    cfg.credentials.authentication.key_len = (client_key_end - client_key_start);
    cfg.credentials.client_id = s_ctx.client_id;

    cfg.network.reconnect_timeout_ms = 5000;
    cfg.network.timeout_ms = 10000;
    cfg.network.disable_auto_reconnect = false;

    s_ctx.client = esp_mqtt_client_init(&cfg);

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_ctx.client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, &mqtt_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(APP_EVENT, APP_EVENT_BARCODE_SCANNED, &on_barcode_scanned, nullptr, nullptr));

    ESP_ERROR_CHECK(esp_mqtt_client_start(s_ctx.client));
}

void mqtt_service_stop() {
    if (s_ctx.client != nullptr) {
        esp_mqtt_client_stop(s_ctx.client);
        esp_mqtt_client_destroy(s_ctx.client);
        s_ctx.client = nullptr;
    }
}
