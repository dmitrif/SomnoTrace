/*
 * SomnoTrace board support for Waveshare ESP32-S3-Touch-LCD-7B.
 *
 * The RGB timings, GPIO map, and CH32V003 controller assignments are adapted
 * from the Waveshare ESP32-S3-Touch-LCD-7B examples (Apache-2.0). This
 * adaptation is part of SomnoTrace and distributed under GPL-3.0-or-later.
 */

#include "board_waveshare_7b.h"

#include <inttypes.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define I2C_SDA GPIO_NUM_8
#define I2C_SCL GPIO_NUM_9
#define I2C_HZ  400000

#define IOX_ADDR       0x24
#define IOX_REG_MODE   0x02
#define IOX_REG_OUTPUT 0x03
#define IOX_REG_PWM    0x05
#define IOX_TOUCH_RST  1
#define IOX_BACKLIGHT  2
#define IOX_SD_CS      4
#define IOX_LCD_POWER  6
#define IOX_USB_SELECT 5

static const char *TAG = "board_7b";
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_iox;
static SemaphoreHandle_t s_lock;
static uint8_t s_output = 0xff;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;

static esp_err_t iox_write(uint8_t reg, uint8_t value)
{
    if (!s_iox) return ESP_ERR_INVALID_STATE;
    uint8_t bytes[2] = { reg, value };
    return i2c_master_transmit(s_iox, bytes, sizeof(bytes), 100);
}

static esp_err_t iox_output(unsigned pin, bool high)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (high) s_output |= (uint8_t)(1U << pin);
    else s_output &= (uint8_t)~(1U << pin);
    esp_err_t ret = iox_write(IOX_REG_OUTPUT, s_output);
    xSemaphoreGive(s_lock);
    return ret;
}

static esp_err_t init_i2c_and_expander(void)
{
    if (s_i2c) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c), TAG,
                        "create board I2C bus");

    i2c_device_config_t dev_cfg = {
        .device_address = IOX_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev_cfg, &s_iox), TAG,
                        "attach CH32V003 I/O controller");

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "create expander mutex");
    ESP_RETURN_ON_ERROR(iox_write(IOX_REG_MODE, 0xff), TAG,
                        "configure CH32V003 outputs");
    ESP_RETURN_ON_ERROR(iox_write(IOX_REG_OUTPUT, s_output), TAG,
                        "initialize CH32V003 outputs");
    return ESP_OK;
}

static esp_err_t init_rgb_panel(void)
{
    if (s_panel) return ESP_OK;

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            /* Keep enough PSRAM-to-bounce-buffer margin while LVGL is
             * actively repainting a 1024x600 view.  The board is stable at
             * 24 MHz for static frames, but physical scrolling/touch tests
             * can still starve scanout and shift the lower part of a frame.
             * 18 MHz remains effectively a 20 fps panel with these porches,
             * matching the UI's 20 Hz live-chart presentation ceiling. */
            .pclk_hz = 18000000,
            .h_res = WAVESHARE_7B_H_RES,
            .v_res = WAVESHARE_7B_V_RES,
            .hsync_pulse_width = 162,
            .hsync_back_porch = 152,
            .hsync_front_porch = 48,
            .vsync_pulse_width = 45,
            .vsync_back_porch = 13,
            .vsync_front_porch = 3,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        /* Ten lines matches Waveshare's older 30.85 MHz 7B configuration.
         * Each RGB565 refill is 20 KiB, comfortably inside the configured
         * 64 KiB data cache, so scanout does not evict LVGL's entire working
         * set on every DMA EOF. The two buffers also use 40 KiB less internal
         * RAM than the previous twenty-line configuration. */
        .bounce_buffer_size_px = WAVESHARE_7B_H_RES * 10,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = GPIO_NUM_46,
        .vsync_gpio_num = GPIO_NUM_3,
        .de_gpio_num = GPIO_NUM_5,
        .pclk_gpio_num = GPIO_NUM_7,
        .disp_gpio_num = GPIO_NUM_NC,
        .data_gpio_nums = {
            GPIO_NUM_14, GPIO_NUM_38, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_10,
            GPIO_NUM_39, GPIO_NUM_0, GPIO_NUM_45, GPIO_NUM_48, GPIO_NUM_47,
            GPIO_NUM_21, GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_42, GPIO_NUM_41,
            GPIO_NUM_40,
        },
        .flags.fb_in_psram = true,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &s_panel), TAG,
                        "create RGB panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "initialize RGB panel");
    return ESP_OK;
}

static esp_err_t init_touch(void)
{
    if (s_touch) return ESP_OK;

    /* Select the GT911's 0x5d address while releasing reset through EXIO1. */
    ESP_RETURN_ON_ERROR(iox_output(IOX_TOUCH_RST, false), TAG, "hold touch reset");
    gpio_config_t int_out = {
        .pin_bit_mask = 1ULL << GPIO_NUM_4,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_out), TAG, "configure touch interrupt");
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(GPIO_NUM_4, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(iox_output(IOX_TOUCH_RST, true), TAG, "release touch reset");
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = I2C_HZ;
    esp_lcd_panel_io_handle_t touch_io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &touch_io), TAG,
                        "create GT911 I2C IO");

    static esp_lcd_touch_io_gt911_config_t gt_cfg;
    gt_cfg.dev_addr = io_cfg.dev_addr;
    esp_lcd_touch_config_t touch_cfg = {
        .x_max = WAVESHARE_7B_H_RES,
        .y_max = WAVESHARE_7B_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_4,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        .driver_data = &gt_cfg,
    };
    return esp_lcd_touch_new_i2c_gt911(touch_io, &touch_cfg, &s_touch);
}

esp_err_t waveshare_7b_init(esp_lcd_panel_handle_t *panel,
                            esp_lcd_touch_handle_t *touch)
{
    ESP_RETURN_ON_ERROR(init_i2c_and_expander(), TAG, "board control init");
    ESP_RETURN_ON_ERROR(iox_output(IOX_LCD_POWER, true), TAG, "LCD power enable");
    /* EXIO5 low connects the native USB port instead of the optional CAN
     * transceiver, preserving USB-Serial-JTAG logs after board init. */
    ESP_RETURN_ON_ERROR(iox_output(IOX_USB_SELECT, false), TAG, "USB select");
    ESP_RETURN_ON_ERROR(iox_output(IOX_SD_CS, true), TAG, "TF card select release");
    ESP_RETURN_ON_ERROR(init_rgb_panel(), TAG, "RGB display init");

    esp_err_t touch_ret = init_touch();
    if (touch_ret != ESP_OK) {
        /* The display and the rest of SomnoTrace remain useful without touch. */
        ESP_LOGW(TAG, "GT911 unavailable: %s", esp_err_to_name(touch_ret));
    }
    ESP_RETURN_ON_ERROR(iox_output(IOX_BACKLIGHT, true), TAG, "backlight enable");

    if (panel) *panel = s_panel;
    if (touch) *touch = s_touch;
    ESP_LOGI(TAG, "Waveshare 7B ready: RGB=%dx%d touch=%s",
             WAVESHARE_7B_H_RES, WAVESHARE_7B_V_RES, s_touch ? "yes" : "no");
    return ESP_OK;
}

esp_err_t waveshare_7b_set_backlight(bool on)
{
    ESP_RETURN_ON_ERROR(init_i2c_and_expander(), TAG, "board control init");
    /* EXIO2 is the panel's hardware enable. An off request removes the
     * backlight electrically; it is not a black framebuffer or 0% PWM. */
    return iox_output(IOX_BACKLIGHT, on);
}

esp_err_t waveshare_7b_set_brightness(uint8_t percent)
{
    ESP_RETURN_ON_ERROR(init_i2c_and_expander(), TAG, "board control init");
    if (percent > 100) percent = 100;
    /* The Waveshare I/O controller drives the backlight PWM active-low:
     * command 0 is steady/full-on and increasing values add off-time. Keep
     * the vendor's 97% attenuation limit so minimum brightness never becomes
     * indistinguishable from the separate hard-off control. */
    uint8_t attenuation = (uint8_t)(100U - percent);
    if (attenuation > 97) attenuation = 97;
    uint8_t pwm = (uint8_t)(attenuation * 255U / 100U);
    return iox_write(IOX_REG_PWM, pwm);
}

esp_err_t waveshare_7b_set_panel_pclk(uint32_t hz)
{
    /* Keep this deliberately narrow: 18 MHz is SomnoTrace's redraw-safe
     * clock, while 30.85 MHz comes from Waveshare's older 7B LVGL examples.
     * The diagnostic endpoint must not become an arbitrary clock control. */
    if (hz != 18000000U && hz != 30850000U) return ESP_ERR_INVALID_ARG;
    if (!s_panel) return ESP_ERR_INVALID_STATE;

    esp_err_t err = esp_lcd_rgb_panel_set_pclk(s_panel, hz);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "diagnostic RGB pixel clock requested: %" PRIu32 " Hz", hz);
    }
    return err;
}

esp_err_t waveshare_7b_prepare_sd(void)
{
    ESP_RETURN_ON_ERROR(init_i2c_and_expander(), TAG, "board control init");
    /* In one-bit SD mode DAT3/CS must remain high. */
    return iox_output(IOX_SD_CS, true);
}
