#include "display_task.h"
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "soc/soc_caps.h"
#include "lvgl.h"
#include "events.h"
#include "print_message.h"
#include "display_device.h"

static const char *TAG = "DISPLAY";

#if !SOC_RTCIO_HOLD_SUPPORTED
    #error "CRITICAL ERROR: The selected ESP chip does not support RTC GPIO Hold!"
#endif

struct UiContext {
    lv_obj_t *root;
    lv_obj_t *cont_main;
    lv_obj_t *cont_status;
    lv_obj_t *lbl_wifi;
    lv_obj_t *lbl_mqtt;
    lv_obj_t *lbl_name;
    lv_obj_t *lbl_price;
    lv_obj_t *lbl_unit;
    lv_obj_t *lbl_stock;
    bool wifi_connected;
    bool mqtt_connected;
    int ip_suffix;
};

static void ui_set_text(lv_obj_t *label, const char *text, uint32_t color_hex, const lv_font_t *font)
{
    if (text == nullptr) text = "";
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color_hex), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
}

static void ui_render_status(const UiContext &ui)
{
    char wifi_buf[32];
    if (ui.wifi_connected) {
        if (ui.ip_suffix >= 0) {
            snprintf(wifi_buf, sizeof(wifi_buf), "WiFi .%d", ui.ip_suffix);
        } else {
            snprintf(wifi_buf, sizeof(wifi_buf), "WiFi");
        }
        ui_set_text(ui.lbl_wifi, wifi_buf, 0x00FF00, &lv_font_montserrat_10);
    } else {
        ui_set_text(ui.lbl_wifi, "WiFi", 0xFF0000, &lv_font_montserrat_10);
    }

    if (ui.mqtt_connected) {
        ui_set_text(ui.lbl_mqtt, "MQTT", 0x00FF00, &lv_font_montserrat_10);
    } else {
        ui_set_text(ui.lbl_mqtt, "MQTT", 0xFF0000, &lv_font_montserrat_10);
    }
}

static void ui_init(UiContext &ui)
{
    ui.root = lv_screen_active();
    lv_obj_clean(ui.root);
    lv_obj_set_style_bg_color(ui.root, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(ui.root, LV_SCROLLBAR_MODE_OFF);

    ui.cont_main = lv_obj_create(ui.root);
    lv_obj_set_size(ui.cont_main, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui.cont_main, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.cont_main, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.cont_main, 5, LV_PART_MAIN);
    lv_obj_set_flex_flow(ui.cont_main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui.cont_main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(ui.cont_main, 16, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(ui.cont_main, LV_SCROLLBAR_MODE_OFF);

    ui.cont_status = lv_obj_create(ui.root);
    lv_obj_set_size(ui.cont_status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(ui.cont_status, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.cont_status, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.cont_status, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(ui.cont_status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui.cont_status, 2, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(ui.cont_status, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(ui.cont_status, LV_ALIGN_BOTTOM_RIGHT, -4, -4);

    ui.lbl_wifi = lv_label_create(ui.cont_status);
    ui.lbl_mqtt = lv_label_create(ui.cont_status);

    ui.lbl_name = lv_label_create(ui.cont_main);
    lv_label_set_long_mode(ui.lbl_name, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(ui.lbl_name, LV_PCT(100));

    ui.lbl_price = lv_label_create(ui.cont_main);
    lv_label_set_long_mode(ui.lbl_price, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(ui.lbl_price, LV_PCT(100));

    ui.lbl_unit = lv_label_create(ui.cont_main);
    lv_label_set_long_mode(ui.lbl_unit, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(ui.lbl_unit, LV_PCT(100));

    ui.lbl_stock = lv_label_create(ui.cont_main);
    lv_label_set_long_mode(ui.lbl_stock, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(ui.lbl_stock, LV_PCT(100));

    ui.wifi_connected = false;
    ui.mqtt_connected = false;
    ui.ip_suffix = -1;

    ui_render_status(ui);

    ui_set_text(ui.lbl_name, "^__^", 0xFFFFFF, &lv_font_montserrat_22);
    ui_set_text(ui.lbl_price, "", 0x000000, &lv_font_montserrat_22);
    ui_set_text(ui.lbl_unit, "", 0x000000, &lv_font_montserrat_22);
    ui_set_text(ui.lbl_stock, "", 0x000000, &lv_font_montserrat_22);
}

static void ui_show_error(UiContext &ui, const char *msg)
{
    ui_set_text(ui.lbl_name, "Chyba", 0xFF0000, &lv_font_montserrat_28);
    ui_set_text(ui.lbl_price, msg, 0xFF0000, &lv_font_montserrat_28);
    ui_set_text(ui.lbl_unit, "", 0x000000, &lv_font_montserrat_28);
    ui_set_text(ui.lbl_stock, "", 0x000000, &lv_font_montserrat_28);
}

static void ui_show_product(UiContext &ui, const ProductData &p)
{
    char buf[128];

    ui_set_text(ui.lbl_name, p.name, 0xFFFFFF, &lv_font_montserrat_24);

    snprintf(buf, sizeof(buf), "Cena: %.2f Kc", static_cast<double>(p.price));
    ui_set_text(ui.lbl_price, buf, 0x00FF00, &lv_font_montserrat_38);

    if (p.unitOfMeasure[0] != '\0' && p.unitCoef > 0.0f) {
        snprintf(buf, sizeof(buf), "Cena za %s: %.2f Kc",
                 p.unitOfMeasure, static_cast<double>(p.price * p.unitCoef));
        ui_set_text(ui.lbl_unit, buf, 0xFFFFFF, &lv_font_montserrat_18);
    } else {
        ui_set_text(ui.lbl_unit, "", 0xFFFFFF, &lv_font_montserrat_18);
    }
    snprintf(buf, sizeof(buf), "Skladem: %u ks", static_cast<unsigned>(p.stock));
    ui_set_text(ui.lbl_stock, buf, 0xFFFFFF, &lv_font_montserrat_18);
}

static void ui_handle_message(UiContext &ui, const PrintMessage &msg)
{
    switch (msg.type) {
    case WIFI_STATUS:
        ui.wifi_connected = msg.data.wifi.connected;
        if (ui.wifi_connected) {
            ui.ip_suffix = msg.data.wifi.ipLastOctet;
        } else {
            ui.ip_suffix = -1;
        }
        ui_render_status(ui);
        break;

    case MQTT_STATUS:
        ui.mqtt_connected = msg.data.mqtt.connected;
        ui_render_status(ui);
        break;

    case ERROR_MSG:
        ui_show_error(ui, msg.data.error.msg);
        break;

    case PRODUCT_DATA:
        ui_show_product(ui, msg.data.product);
        break;
    }
}

[[noreturn]] void display_task(void *pvParameters)
{
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    ESP_LOGD(TAG, "Display task started");

    const auto* params = static_cast<const DisplayTaskParams*>(pvParameters);
    DisplayDevice& device = params->device;

    ESP_ERROR_CHECK(device.init());

    UiContext ui{};

    lvgl_port_lock(0);
    ui_init(ui);
    lvgl_port_unlock();

    for (;;) {
        const EventBits_t req_bits = xEventGroupWaitBits(
            params->eventGroup,
            BIT_REQ_STOP,
            pdFALSE,
            pdFALSE,
            0
        );

        if ((req_bits & BIT_REQ_STOP) != 0) {
            PrintMessage pending_msg{};
            if (xQueueReceive(params->printQueue, &pending_msg, 0) == pdTRUE) {
                if (lvgl_port_lock(1000)) {
                    ui_handle_message(ui, pending_msg);
                    lvgl_port_unlock();
                } else {
                    ESP_LOGW(TAG, "LVGL lock timeout during STOP, skipping pending frame");
                }
            }

            ESP_LOGI(TAG, "Display task ready, acknowledging STOP and suspending");
            xEventGroupSetBits(params->eventGroup, BIT_ACK_DISPLAY);
            vTaskSuspend(nullptr);
        }

        PrintMessage msg{};
        if (xQueueReceive(params->printQueue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }

        if (!lvgl_port_lock(1000)) {
            ESP_LOGW(TAG, "LVGL lock timeout, skipping frame");
            continue;
        }

        ui_handle_message(ui, msg);

        lvgl_port_unlock();
    }
}
