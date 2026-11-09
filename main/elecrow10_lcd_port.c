/*
 * Display bring-up for the Elecrow CrowPanel Advanced 10.1" ESP32-P4 HMI
 * (1024x600 IPS, EK79007 over 2-lane MIPI-DSI, GT911 touch, ESP32-C6 for
 * Wi-Fi over SDIO). Beta for issue #54.
 *
 * Facts from Elecrow's own V1.2 lessons and the V1.2 schematic:
 *   panel native landscape 1024x600 (no rotation), DSI 2 lanes @ 900 Mbps,
 *   DPI 51 MHz, hsync 70/160/160, vsync 10/23/12, EK79007 default init;
 *   LCD reset GPIO41 (schematic; the vendor code leaves it unused);
 *   backlight: GPIO31 into the MT9201 boost EN, active high, PWM-able;
 *   touch GT911 on I2C SDA 45 / SCL 46, RST 40, INT 42;
 *   LDO3 2.5 V = MIPI PHY, LDO4 3.3 V = the GPIO39-48 I/O domain (touch
 *   I2C pull-ups, touch reset, LCD reset), so LDO4 comes up first.
 *   C6 SDIO on V1.1/V1.2: CLK 18, CMD 19, D0 17, D1 16, D2 15, D3 14,
 *   reset 32 (sdkconfig.elecrow10). V1.0 boards are wired differently.
 */
#include "waveshare_rgb_lcd_port.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_touch_gt911.h"
#include "freertos/semphr.h"

static const char *TAG = "elecrow10";

#define EC_I2C_SDA        45
#define EC_I2C_SCL        46
#define EC_I2C_FREQ_HZ    400000
#define EC_TOUCH_RST      40
#define EC_TOUCH_INT      42
#define EC_LCD_RST        41
#define EC_LCD_BACKLIGHT  31
#define EC_LEDC_CH        LEDC_CHANNEL_1

#define EC_DSI_LDO_CHAN   3
#define EC_DSI_LDO_MV     2500
#define EC_IO_LDO_CHAN    4
#define EC_IO_LDO_MV      3300

#define EC_LCD_H_RES      1024
#define EC_LCD_V_RES      600
#define EC_BOARD_NAME     "Elecrow CrowPanel Advanced 10.1 (ESP32-P4)"

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_trans_done;

/* ---------- backlight ---------- */

static void backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const ledc_channel_config_t ch = {
        .gpio_num = EC_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = EC_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,          /* dark until the first frame */
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

esp_err_t waveshare_rgb_lcd_bl_on(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, EC_LEDC_CH, 1023);
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, EC_LEDC_CH);
}

esp_err_t waveshare_rgb_lcd_bl_off(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, EC_LEDC_CH, 0);
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, EC_LEDC_CH);
}

bool waveshare_rgb_lcd_bl_dimmable(void) { return true; }

esp_err_t waveshare_rgb_lcd_bl_pct(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, EC_LEDC_CH, (uint32_t)(1023 * pct / 100));
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, EC_LEDC_CH);
}

/* ---------- DSI panel ---------- */

static IRAM_ATTR bool trans_done_cb(esp_lcd_panel_handle_t panel,
                                    esp_lcd_dpi_panel_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_trans_done, &woken);
    return woken == pdTRUE;
}

void tab5_lcd_wait_trans_done(void)
{
    if (s_trans_done != NULL) {
        xSemaphoreTake(s_trans_done, pdMS_TO_TICKS(100));
    }
}

static esp_err_t panel_init(void)
{
    static esp_ldo_channel_handle_t phy_ldo;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = EC_DSI_LDO_CHAN,
        .voltage_mv = EC_DSI_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &phy_ldo));

    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 900,
    };
    esp_lcd_dsi_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &bus));

    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &io));

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 51,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = EC_LCD_H_RES,
            .v_size = EC_LCD_V_RES,
            .hsync_pulse_width = 70,
            .hsync_back_porch = 160,
            .hsync_front_porch = 160,
            .vsync_pulse_width = 10,
            .vsync_back_porch = 23,
            .vsync_front_porch = 12,
        },
        .flags.use_dma2d = true,
    };

    /* No custom table: the component's default EK79007 sequence (lane
     * setup, gamma, sleep out) is what Elecrow's examples run. */
    ek79007_vendor_config_t vendor = {
        .mipi_config = {
            .dsi_bus = bus,
            .dpi_config = &dpi_cfg,
            .lane_num = 2,
        },
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = EC_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(io, &dev_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_trans_done = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = trans_done_cb,
    };
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL));
    return ESP_OK;
}

/* ---------- touch ---------- */

static esp_err_t touch_init(esp_lcd_touch_handle_t *out_tp)
{
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = EC_LCD_H_RES,
        .y_max = EC_LCD_V_RES,
        .rst_gpio_num = EC_TOUCH_RST,
        .int_gpio_num = EC_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    /* The GT911 picks 0x5D or 0x14 from INT at reset; try both. */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2 && err != ESP_OK; attempt++) {
        esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        io_cfg.scl_speed_hz = EC_I2C_FREQ_HZ;
        io_cfg.dev_addr = attempt == 0 ? ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS
                                       : ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        esp_lcd_panel_io_handle_t tp_io = NULL;
        if (esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &tp_io) != ESP_OK) {
            continue;
        }
        err = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, out_tp);
        if (err != ESP_OK) {
            esp_lcd_panel_io_del(tp_io);
            ESP_LOGW(TAG, "GT911 not at 0x%02x", (unsigned)io_cfg.dev_addr);
        }
    }
    return err;
}

/* ---------- public API ---------- */

const char *waveshare_lcd_board_name(void)
{
    return EC_BOARD_NAME;
}

void *waveshare_lcd_get_fb(void)
{
    if (s_panel == NULL) {
        return NULL;
    }
    void *fb = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK) {
        return NULL;
    }
    return fb;
}

void waveshare_lcd_get_res(int *w, int *h)
{
    *w = EC_LCD_H_RES;
    *h = EC_LCD_V_RES;
}

esp_err_t waveshare_esp32_s3_rgb_lcd_init(void)
{
    /* GPIO39-48 (touch bus, touch/LCD reset) live in the LDO4 domain */
    static esp_ldo_channel_handle_t io_ldo;
    esp_ldo_channel_config_t io_ldo_cfg = {
        .chan_id = EC_IO_LDO_CHAN,
        .voltage_mv = EC_IO_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&io_ldo_cfg, &io_ldo));
    vTaskDelay(pdMS_TO_TICKS(10));

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = 0,
        .sda_io_num = EC_I2C_SDA,
        .scl_io_num = EC_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c));
    ESP_LOGI(TAG, "board: %s", EC_BOARD_NAME);

    backlight_init();
    ESP_ERROR_CHECK(panel_init());

    esp_lcd_touch_handle_t tp = NULL;
    if (touch_init(&tp) != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed - running without touch");
        tp = NULL;
    }
    ESP_ERROR_CHECK(lvgl_port_init(s_panel, tp));
    return ESP_OK;
}
