#include "devices/display_device.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lvgl_port_disp.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "lvgl.h"

static const char* TAG = "DISPLAY_DEVICE";

#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_DUTY_RES           LEDC_TIMER_8_BIT
#define LEDC_FREQUENCY          5000
#define LEDC_DUTY_ON            200
#define LEDC_FADE_DELAY_MS      10

static void init_backlight_pwm()
{
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

static void set_backlight_brightness(const uint32_t duty)
{
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

DisplayDevice::DisplayDevice()
    : initialized_(false), io_handle_(nullptr), panel_handle_(nullptr), disp_(nullptr) {}

esp_err_t DisplayDevice::init()
{
    if (initialized_) {
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg{};
    bus_cfg.sclk_io_num = CONFIG_TFT_SCLK;
    bus_cfg.mosi_io_num = CONFIG_TFT_MOSI;
    bus_cfg.miso_io_num = CONFIG_TFT_MISO;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 320 * 240 * sizeof(uint16_t) + 100;
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    esp_lcd_panel_io_handle_t io_handle = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg{};
    io_cfg.cs_gpio_num = CONFIG_TFT_CS;
    io_cfg.dc_gpio_num = CONFIG_TFT_DC;
    io_cfg.pclk_hz = 20 * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;

    err = esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io_handle);
    if (err != ESP_OK) {
        spi_bus_free(SPI2_HOST);
        return err;
    }

    esp_lcd_panel_handle_t panel = nullptr;
    esp_lcd_panel_dev_config_t panel_cfg{};
    panel_cfg.reset_gpio_num = CONFIG_TFT_RST;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel = 16;

    err = esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &panel);
    if (err != ESP_OK) {
        esp_lcd_panel_io_del(io_handle);
        spi_bus_free(SPI2_HOST);
        return err;
    }

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
    lv_display_t* disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == nullptr) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(io_handle);
        spi_bus_free(SPI2_HOST);
        return ESP_FAIL;
    }

    io_handle_ = io_handle;
    panel_handle_ = panel;
    disp_ = disp;
    initialized_ = true;
    return ESP_OK;
}

esp_err_t DisplayDevice::sleep()
{
    if (!initialized_) {
        gpio_hold_dis((gpio_num_t)CONFIG_LED_PIN);
        gpio_set_direction((gpio_num_t)CONFIG_LED_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)CONFIG_LED_PIN, 0);
        gpio_hold_en((gpio_num_t)CONFIG_LED_PIN);
        return ESP_OK;
    }

    esp_lcd_panel_io_handle_t io = static_cast<esp_lcd_panel_io_handle_t>(io_handle_);

    display_backlight_fade_off();

    ledc_stop(LEDC_MODE, LEDC_CHANNEL_0, 0);
    gpio_set_direction((gpio_num_t)CONFIG_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)CONFIG_LED_PIN, 0);
    gpio_hold_en((gpio_num_t)CONFIG_LED_PIN);

    esp_lcd_panel_io_tx_param(io, 0x28, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    esp_lcd_panel_io_tx_param(io, 0x10, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    return ESP_OK;
}

void DisplayDevice::deinit()
{
    if (!initialized_) {
        return;
    }

    lv_display_t* disp = static_cast<lv_display_t*>(disp_);
    esp_lcd_panel_handle_t panel = static_cast<esp_lcd_panel_handle_t>(panel_handle_);
    esp_lcd_panel_io_handle_t io = static_cast<esp_lcd_panel_io_handle_t>(io_handle_);

    if (disp) lv_display_delete(disp);
    if (panel) esp_lcd_panel_del(panel);
    if (io) esp_lcd_panel_io_del(io);
    spi_bus_free(SPI2_HOST);

    initialized_ = false;
    io_handle_ = nullptr;
    panel_handle_ = nullptr;
    disp_ = nullptr;
}
