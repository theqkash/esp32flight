/*
 * Adapted from Waveshare's 08_lvgl_Porting demo (CC0-1.0).
 */
#include "waveshare_rgb_lcd_port.h"

static const char *TAG = "lcd_port";

static esp_lcd_panel_handle_t s_panel;

void *waveshare_lcd_get_fb(void)
{
    if (s_panel == NULL) {
        return NULL;
    }
    void *fb0 = NULL, *fb1 = NULL;
    if (esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1) != ESP_OK) {
        return NULL;
    }
    return fb0;
}

IRAM_ATTR static bool rgb_lcd_on_vsync_event(esp_lcd_panel_handle_t panel,
                                             const esp_lcd_rgb_panel_event_data_t *edata,
                                             void *user_ctx)
{
    return lvgl_port_notify_rgb_vsync();
}

static esp_err_t i2c_master_init(void)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &i2c_conf);
    return i2c_driver_install(I2C_MASTER_NUM, i2c_conf.mode, 0, 0, 0);
}

/* CH422G I2C expander: raw writes, 0x24 = mode reg (0x01 -> push-pull out),
 * 0x38 = EXIO0-7 output byte. EXIO1=TP_RST, EXIO2=backlight, EXIO3=LCD_RST. */
static void ch422g_write(uint8_t addr, uint8_t val)
{
    i2c_master_write_to_device(I2C_MASTER_NUM, addr, &val, 1,
                               I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}

static void touch_reset(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = 1ULL << GPIO_TOUCH_INT,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);

    ch422g_write(0x24, 0x01);
    ch422g_write(0x38, 0x2C);           /* TP_RST low */
    esp_rom_delay_us(100 * 1000);
    gpio_set_level(GPIO_TOUCH_INT, 0);  /* INT low during reset -> addr 0x5D */
    esp_rom_delay_us(100 * 1000);
    ch422g_write(0x38, 0x2E);           /* TP_RST high */
    esp_rom_delay_us(200 * 1000);
}

esp_err_t waveshare_esp32_s3_rgb_lcd_init(void)
{
    ESP_LOGI(TAG, "Install RGB LCD panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
            .h_res = EXAMPLE_LCD_H_RES,
            .v_res = EXAMPLE_LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags = {
                .pclk_active_neg = 1,
            },
        },
        .data_width = EXAMPLE_RGB_DATA_WIDTH,
        .bits_per_pixel = EXAMPLE_RGB_BIT_PER_PIXEL,
        .num_fbs = LVGL_PORT_LCD_RGB_BUFFER_NUMS,
        .bounce_buffer_size_px = EXAMPLE_RGB_BOUNCE_BUFFER_SIZE,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = EXAMPLE_LCD_IO_RGB_HSYNC,
        .vsync_gpio_num = EXAMPLE_LCD_IO_RGB_VSYNC,
        .de_gpio_num = EXAMPLE_LCD_IO_RGB_DE,
        .pclk_gpio_num = EXAMPLE_LCD_IO_RGB_PCLK,
        .disp_gpio_num = EXAMPLE_LCD_IO_RGB_DISP,
        .data_gpio_nums = {
            EXAMPLE_LCD_IO_RGB_DATA0,
            EXAMPLE_LCD_IO_RGB_DATA1,
            EXAMPLE_LCD_IO_RGB_DATA2,
            EXAMPLE_LCD_IO_RGB_DATA3,
            EXAMPLE_LCD_IO_RGB_DATA4,
            EXAMPLE_LCD_IO_RGB_DATA5,
            EXAMPLE_LCD_IO_RGB_DATA6,
            EXAMPLE_LCD_IO_RGB_DATA7,
            EXAMPLE_LCD_IO_RGB_DATA8,
            EXAMPLE_LCD_IO_RGB_DATA9,
            EXAMPLE_LCD_IO_RGB_DATA10,
            EXAMPLE_LCD_IO_RGB_DATA11,
            EXAMPLE_LCD_IO_RGB_DATA12,
            EXAMPLE_LCD_IO_RGB_DATA13,
            EXAMPLE_LCD_IO_RGB_DATA14,
            EXAMPLE_LCD_IO_RGB_DATA15,
        },
        .flags = {
            .fb_in_psram = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    s_panel = panel_handle;

    ESP_LOGI(TAG, "Initialize I2C bus + GT911 touch");
    i2c_master_init();
    touch_reset();

    esp_lcd_touch_handle_t tp_handle = NULL;
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    /* Legacy i2c driver sets the bus speed itself; v1 io rejects a non-zero value here. */
    tp_io_config.scl_speed_hz = 0;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2C_MASTER_NUM,
                                             &tp_io_config, &tp_io_handle));
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
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
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp_handle));

    ESP_ERROR_CHECK(lvgl_port_init(panel_handle, tp_handle));

    esp_lcd_rgb_panel_event_callbacks_t cbs = {
#if EXAMPLE_RGB_BOUNCE_BUFFER_SIZE > 0
        .on_bounce_frame_finish = rgb_lcd_on_vsync_event,
#else
        .on_vsync = rgb_lcd_on_vsync_event,
#endif
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, NULL));

    return ESP_OK;
}

esp_err_t waveshare_rgb_lcd_bl_on(void)
{
    ch422g_write(0x24, 0x01);
    ch422g_write(0x38, 0x1E);
    return ESP_OK;
}

esp_err_t waveshare_rgb_lcd_bl_off(void)
{
    ch422g_write(0x24, 0x01);
    ch422g_write(0x38, 0x1A);
    return ESP_OK;
}
