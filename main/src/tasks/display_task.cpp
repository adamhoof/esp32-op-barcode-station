#include "tasks/display_task.h"
#include <cstdio>
#include <cstring>
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lvgl_port_disp.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "soc/soc_caps.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "events.h"
#include "print_message.h"

#if !SOC_RTCIO_HOLD_SUPPORTED
    #error "CRITICAL ERROR: The selected ESP chip does not support RTC GPIO Hold!"
#endif

#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_DUTY_RES           LEDC_TIMER_8_BIT
#define LEDC_FREQUENCY          5000
#define LEDC_DUTY_ON            200

#define LEDC_FADE_DELAY_MS      10

struct UiContext {
    lv_obj_t *root;
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
    bool first_scan_done;
};

static void init_backlight_pwm() {
    gpio_hold_dis((gpio_num_t)CONFIG_LED_PIN);

    ledc_timer_config_t ledc_timer_cfg{};
    ledc_timer_cfg.speed_mode       = LEDC_MODE;
    ledc_timer_cfg.duty_resolution  = LEDC_DUTY_RES;
    ledc_timer_cfg.timer_num        = LEDC_TIMER;
    ledc_timer_cfg.freq_hz          = LEDC_FREQUENCY;
    ledc_timer_cfg.clk_cfg          = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer_cfg));

    ledc_channel_config_t chan_cfg{};
    chan_cfg.gpio_num       = CONFIG_LED_PIN;
    chan_cfg.speed_mode     = LEDC_MODE;
    chan_cfg.channel        = LEDC_CHANNEL_0;
    chan_cfg.intr_type      = LEDC_INTR_DISABLE;
    chan_cfg.timer_sel      = LEDC_TIMER;
    chan_cfg.duty           = 0;
    chan_cfg.hpoint         = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&chan_cfg));
}

static void set_backlight_brightness(uint32_t duty) {
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
}

static void display_backlight_fade_on(const uint32_t max_brightness, const uint32_t fade_step = 20)
{
    uint32_t duty = 0;
    while (true) {
        set_backlight_brightness(duty);
        vTaskDelay(pdMS_TO_TICKS(LEDC_FADE_DELAY_MS));

        if (duty == max_brightness) {
            break;
        }

        duty = (duty + fade_step >= max_brightness) ? max_brightness : (duty + fade_step);
    }

    set_backlight_brightness(max_brightness);
}

static void display_backlight_fade_off(const uint32_t fade_step = 20)
{
    uint32_t duty = LEDC_DUTY_ON;
    while (true) {
        set_backlight_brightness(duty);
        vTaskDelay(pdMS_TO_TICKS(LEDC_FADE_DELAY_MS));

        if (duty == 0) {
            break;
        }

        duty = (duty <= fade_step) ? 0 : (duty - fade_step);
    }

    set_backlight_brightness(0);
}

static void ui_set_text(lv_obj_t *label, const char *text, uint32_t color_hex, const lv_font_t *font)
{
    if (text == nullptr) text = "";
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color_hex), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
}

static void ui_render_status(const UiContext &ui)
{
    if (ui.first_scan_done) {
        if (!lv_obj_has_flag(ui.cont_status, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(ui.cont_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    char wifi_buf[32];
    if (ui.wifi_connected) {
        if (ui.ip_suffix >= 0) {
            snprintf(wifi_buf, sizeof(wifi_buf), "WiFi .%d", ui.ip_suffix);
        } else {
            snprintf(wifi_buf, sizeof(wifi_buf), "WiFi");
        }
        ui_set_text(ui.lbl_wifi, wifi_buf, 0x00FF00, &lv_font_montserrat_22);
    } else {
        ui_set_text(ui.lbl_wifi, "WiFi", 0xFF0000, &lv_font_montserrat_22);
    }

    if (ui.mqtt_connected) {
        ui_set_text(ui.lbl_mqtt, "MQTT", 0x00FF00, &lv_font_montserrat_22);
    } else {
        ui_set_text(ui.lbl_mqtt, "MQTT", 0xFF0000, &lv_font_montserrat_22);
    }
}

static void ui_init(UiContext &ui)
{
    ui.root = lv_screen_active();
    lv_obj_clean(ui.root);
    lv_obj_set_style_bg_color(ui.root, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(ui.root, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *cont = lv_obj_create(ui.root);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 5, LV_PART_MAIN);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(cont, 20, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);

    ui.cont_status = lv_obj_create(cont);
    lv_obj_set_size(ui.cont_status, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(ui.cont_status, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.cont_status, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.cont_status, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(ui.cont_status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(ui.cont_status, LV_SCROLLBAR_MODE_OFF);

    ui.lbl_wifi = lv_label_create(ui.cont_status);
    ui.lbl_mqtt = lv_label_create(ui.cont_status);

    ui.lbl_name = lv_label_create(cont);
    lv_label_set_long_mode(ui.lbl_name, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(ui.lbl_name, LV_PCT(100));

    ui.lbl_price = lv_label_create(cont);
    lv_label_set_long_mode(ui.lbl_price, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(ui.lbl_price, LV_PCT(100));

    ui.lbl_unit = lv_label_create(cont);
    lv_label_set_long_mode(ui.lbl_unit, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(ui.lbl_unit, LV_PCT(100));

    ui.lbl_stock = lv_label_create(cont);
    lv_label_set_long_mode(ui.lbl_stock, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(ui.lbl_stock, LV_PCT(100));

    ui.wifi_connected = false;
    ui.mqtt_connected = false;
    ui.ip_suffix = -1;
    ui.first_scan_done = false;

    ui_render_status(ui);

    ui_set_text(ui.lbl_name, "^__^", 0xFFFFFF, &lv_font_montserrat_22);
    ui_set_text(ui.lbl_price, "", 0x000000, &lv_font_montserrat_22);
    ui_set_text(ui.lbl_unit, "", 0x000000, &lv_font_montserrat_22);
    ui_set_text(ui.lbl_stock, "", 0x000000, &lv_font_montserrat_22);
}

static void ui_show_error(UiContext &ui, const char *msg)
{
    if (!ui.first_scan_done) {
        ui.first_scan_done = true;
        lv_obj_add_flag(ui.cont_status, LV_OBJ_FLAG_HIDDEN);
    }
    ui_set_text(ui.lbl_name, "Chyba", 0xFF0000, &lv_font_montserrat_30);
    ui_set_text(ui.lbl_price, msg, 0xFF0000, &lv_font_montserrat_30);
    ui_set_text(ui.lbl_unit, "", 0x000000, &lv_font_montserrat_30);
    ui_set_text(ui.lbl_stock, "", 0x000000, &lv_font_montserrat_30);
}

static void ui_show_product(UiContext &ui, const ProductData &p)
{
    if (!ui.first_scan_done) {
        ui.first_scan_done = true;
        lv_obj_add_flag(ui.cont_status, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[128];

    ui_set_text(ui.lbl_name, p.name, 0xFFFFFF, &lv_font_montserrat_22);

    snprintf(buf, sizeof(buf), "Cena: %.2f kc", static_cast<double>(p.price));
    ui_set_text(ui.lbl_price, buf, 0x00FF00, &lv_font_montserrat_40);

    if (p.unitOfMeasure[0] != '\0' && p.unitCoef > 0.0f) {
        snprintf(buf, sizeof(buf), "Cena za %s: %.2f kc",
                 p.unitOfMeasure, static_cast<double>(p.price * p.unitCoef));
        ui_set_text(ui.lbl_unit, buf, 0xFFFFFF, &lv_font_montserrat_22);
    } else {
        ui_set_text(ui.lbl_unit, "", 0xFFFFFF, &lv_font_montserrat_22);
    }
    snprintf(buf, sizeof(buf), "Skladem: %u ks", static_cast<unsigned>(p.stock));
    ui_set_text(ui.lbl_stock, buf, 0xFFFFFF, &lv_font_montserrat_22);
}

static void ui_show_updating(UiContext &ui)
{
    ui.first_scan_done = true;
    lv_obj_add_flag(ui.cont_status, LV_OBJ_FLAG_HIDDEN);
    ui_set_text(ui.lbl_name, "UPDATING...", 0xFFFF00, &lv_font_montserrat_22);
    ui_set_text(ui.lbl_price, "", 0x000000, &lv_font_montserrat_22);
    ui_set_text(ui.lbl_unit, "", 0x000000, &lv_font_montserrat_22);
    ui_set_text(ui.lbl_stock, "", 0x000000, &lv_font_montserrat_22);
}

static void init_display_resources(esp_lcd_panel_io_handle_t* out_io, esp_lcd_panel_handle_t* out_panel, lv_display_t** out_disp) {
    spi_bus_config_t bus_cfg{};
    bus_cfg.sclk_io_num = CONFIG_TFT_SCLK;
    bus_cfg.mosi_io_num = CONFIG_TFT_MOSI;
    bus_cfg.miso_io_num = CONFIG_TFT_MISO;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 320 * 240 * sizeof(uint16_t) + 100;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg{};
    io_cfg.cs_gpio_num = CONFIG_TFT_CS;
    io_cfg.dc_gpio_num = CONFIG_TFT_DC;
    io_cfg.pclk_hz = 20 * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io_handle));

    esp_lcd_panel_handle_t panel = nullptr;
    esp_lcd_panel_dev_config_t panel_cfg{};
    panel_cfg.reset_gpio_num = CONFIG_TFT_RST;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel = 16;

    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 0, 0));

    init_backlight_pwm();
    display_backlight_fade_on(LEDC_DUTY_ON);

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg{};
    disp_cfg.io_handle = io_handle;
    disp_cfg.panel_handle = panel;
    disp_cfg.buffer_size = 320 * 40;
    disp_cfg.double_buffer = false;
    disp_cfg.hres = 320;
    disp_cfg.vres = 240;
    disp_cfg.monochrome = false;
    disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.swap_bytes = true;

    disp_cfg.rotation.swap_xy = true;
    disp_cfg.rotation.mirror_x = false;
    disp_cfg.rotation.mirror_y = false;

    disp_cfg.flags.buff_dma = true;
    *out_disp = lvgl_port_add_disp(&disp_cfg);
    *out_io = io_handle;
    *out_panel = panel;
}

static void deinit_display_resources(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel, lv_display_t* disp) {
    if (disp) lv_display_delete(disp);
    if (panel) esp_lcd_panel_del(panel);
    if (io) esp_lcd_panel_io_del(io);
    spi_bus_free(SPI2_HOST);
}

[[noreturn]] void display_task(void *pvParameters)
{
    const auto *params = static_cast<const DisplayTaskParams *>(pvParameters);

    esp_lcd_panel_io_handle_t io_handle = nullptr;
    esp_lcd_panel_handle_t panel_handle = nullptr;
    lv_display_t *disp = nullptr;

    init_display_resources(&io_handle, &panel_handle, &disp);

    UiContext ui{};

    lvgl_port_lock(0);
    ui_init(ui);
    lvgl_port_unlock();

    for (;;) {
        const EventBits_t req_bits = xEventGroupWaitBits(
            params->eventGroup,
            BIT_REQ_SLEEP | BIT_REQ_OTA,
            pdFALSE,
            pdFALSE,
            0
        );

        if ((req_bits & BIT_REQ_SLEEP) != 0) {
            lvgl_port_lock(0);

            display_backlight_fade_off();

            ledc_stop(LEDC_MODE, LEDC_CHANNEL_0, 0);
            gpio_set_direction((gpio_num_t)CONFIG_LED_PIN, GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)CONFIG_LED_PIN, 0);
            gpio_hold_en((gpio_num_t)CONFIG_LED_PIN);

            esp_lcd_panel_io_tx_param(io_handle, 0x28, nullptr, 0);
            vTaskDelay(pdMS_TO_TICKS(20));

            esp_lcd_panel_io_tx_param(io_handle, 0x10, nullptr, 0);
            vTaskDelay(pdMS_TO_TICKS(120));

            lvgl_port_unlock();

            xEventGroupSetBits(params->eventGroup, BIT_ACK_DISPLAY);
            vTaskSuspend(nullptr);
        }

        if ((req_bits & BIT_REQ_OTA) != 0) {
            lvgl_port_lock(0);
            ui_show_updating(ui);
            lvgl_port_unlock();

            vTaskDelay(pdMS_TO_TICKS(500));
            deinit_display_resources(io_handle, panel_handle, disp);

            xEventGroupSetBits(params->eventGroup, BIT_ACK_DISPLAY);
            vTaskSuspend(nullptr);
        }

        PrintMessage msg{};
        if (xQueueReceive(params->printQueue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }

        lvgl_port_lock(0);

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

        lvgl_port_unlock();
    }
}
