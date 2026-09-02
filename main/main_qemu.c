/* Deterministic UI-only firmware entry point for ESP32-S3 QEMU. */
#include <math.h>
#include <stdint.h>

#include "bsp_display.h"
#include "device_settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "somnotrace_qemu";

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    ESP_ERROR_CHECK(bsp_display_init());
    device_settings_t settings;
    device_settings_load(&settings);
    bsp_display_set_brightness(settings.brightness);
    bsp_display_enable_touch_services(false, false);
    bsp_display_qemu_seed_demo();
    bsp_display_set_wifi_connected(true);
    bsp_display_set_as11_paired(true);
    bsp_display_set_sd_ready(true);
    bsp_display_set_therapy_start_time(esp_timer_get_time() - 42 * 60 * 1000000LL);
    bsp_display_set_therapy_active(true);
    bsp_display_set_notice("QEMU preview - simulated data");

    ESP_LOGI(TAG, "1024x600 UI preview ready; cycling tabs every 8 seconds");
    unsigned iteration = 0;
    float phase = 0.0f;
    while (true) {
        float flow = 36.0f * sinf(phase) + 7.0f * sinf(phase * 2.3f);
        bsp_display_push_flow(flow);
        if ((iteration % 10) == 0) {
            bsp_display_push_leak(3.2f + 0.8f * sinf(phase * 0.3f));
            bsp_display_push_metrics(8.6f + 0.4f * sinf(phase * 0.2f),
                                     14.4f, 0.08f);
        }
        if ((iteration % 80) == 0) {
            uint8_t tab = (iteration / 80) % 5;
            bsp_display_qemu_set_tab(tab);
            ESP_LOGI(TAG, "preview tab %u/4", (unsigned)tab);
        }
        phase += 0.12f;
        iteration++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
