#include "tasks/ota_task.h"

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include <cstring>

extern const uint8_t ca_cert_start[] asm("_binary_ca_crt_start");
extern const uint8_t ca_cert_end[] asm("_binary_ca_crt_end");
extern const uint8_t client_cert_start[] asm("_binary_client_crt_start");
extern const uint8_t client_cert_end[] asm("_binary_client_crt_end");
extern const uint8_t client_key_start[] asm("_binary_client_key_start");
extern const uint8_t client_key_end[] asm("_binary_client_key_end");

static const char *TAG = "ota_task";

[[noreturn]] void ota_task(void *pvParameters)
{
    auto *params = static_cast<OtaTaskParams *>(pvParameters);

    esp_http_client_config_t http_config{};

    http_config.url = params->url;

    http_config.cert_pem = reinterpret_cast<const char *>(ca_cert_start);
    http_config.cert_len = ca_cert_end - ca_cert_start;

    http_config.client_cert_pem = reinterpret_cast<const char *>(client_cert_start);
    http_config.client_cert_len = client_cert_end - client_cert_start;
    http_config.client_key_pem = reinterpret_cast<const char *>(client_key_start);
    http_config.client_key_len = client_key_end - client_key_start;

    http_config.keep_alive_enable = true;
    http_config.skip_cert_common_name_check = false;
    http_config.timeout_ms = 10000;

    esp_https_ota_config_t ota_config{};
    ota_config.http_config = &http_config;

    ESP_LOGI(TAG, "Starting HTTPS OTA from %s", http_config.url);

    const esp_err_t err = esp_https_ota(&ota_config);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful, restarting");
    } else {
        ESP_LOGE(TAG, "OTA failed (%s), restarting", esp_err_to_name(err));
    }

    esp_restart();
}