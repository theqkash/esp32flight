/*
 * Display bring-up for the supported 800x480 RGB boards. One binary drives
 * both: the board is picked at boot (or forced via menuconfig).
 *
 *  - Waveshare ESP32-S3-Touch-LCD-7: CH422G IO expander on I2C handles the
 *    backlight and touch reset. The expander's presence is also how the
 *    board is detected.
 *  - Guition JC8048W550 (5"): same panel type on different pins, backlight
 *    on a plain GPIO, GT911 reset on a GPIO, touch mirrored in both axes.
 *    Pin map extracted from KamKubicki/flyRadarEsp32 (working device).
 *
 * Adapted from Waveshare's 08_lvgl_Porting demo (CC0-1.0).
 */
#include "waveshare_rgb_lcd_port.h"
#include "driver/ledc.h"
#include "esp_check.h"
#if CONFIG_CANFLIGHT_BOARD_SUNTON_4827S043R
#include "driver/spi_master.h"
#include "esp_lcd_touch_xpt2046.h"
#endif

static const char *TAG = "lcd_port";

typedef struct {
    const char *name;
    int de, vsync, hsync, pclk;
    int data[16];               /* B0..B4, G0..G5, R0..R4 */
    int i2c_sda, i2c_scl;
    bool has_ch422g;            /* backlight + touch reset via expander */
    bool has_ch32v003;          /* 7B: helper MCU at 0x24 (regs, not CH422G) */
    int bl_gpio;                /* when neither expander is present */
    int tp_rst_gpio;            /* when neither expander is present */
    /* RGB timing set (the 1024x600 panel needs its own) */
    int hs_pulse, hs_bp, hs_fp, vs_pulse, vs_bp, vs_fp;
    int pclk_hz;                /* 0 -> EXAMPLE_LCD_PIXEL_CLOCK_HZ default */
    bool tp_mirror;             /* GT911 reports mirrored coordinates */
    bool has_xpt2046;           /* resistive touch over SPI instead of GT911 */
    int tp_sclk, tp_mosi, tp_miso, tp_cs, tp_irq;
    int lcd_rst_gpio;           /* panel reset line; 0 = none (no board resets via GPIO0) */
    bool has_stc8;              /* CrowPanel Advance: STC8 helper at 0x30 owns the backlight */
} board_cfg_t;

__attribute__((unused)) static const board_cfg_t k_waveshare = {
    /* One PCB family: the 4.3", 5" and 7" Waveshare 800x480 boards share
     * every pin, the expander and the timings (verified against the
     * official demos of both the 7 and the 4.3 repos), so this single
     * entry covers them all. */
    .name = "Waveshare ESP32-S3-Touch-LCD (4.3/5/7)",
    .de = 5, .vsync = 3, .hsync = 46, .pclk = 7,
    .data = { 14, 38, 18, 17, 10,       /* B0..B4 */
              39, 0, 45, 48, 47, 21,    /* G0..G5 */
              1, 2, 42, 41, 40 },       /* R0..R4 */
    .i2c_sda = 8, .i2c_scl = 9,
    .has_ch422g = true,
    .bl_gpio = -1,
    .tp_rst_gpio = -1,
    .hs_pulse = 4, .hs_bp = 8, .hs_fp = 8,
    .vs_pulse = 4, .vs_bp = 8, .vs_fp = 8,
    .tp_mirror = false,
};

__attribute__((unused)) static const board_cfg_t k_waveshare_7b = {
    /* Type-B 7": same RGB wiring as the 800x480 family, but a 1024x600
     * panel with its own timings and a CH32V003 helper MCU (I2C 0x24,
     * register protocol: 0x02 mode, 0x03 outputs, 0x05 backlight PWM).
     * Values verified against waveshareteam/ESP32-S3-Touch-LCD-7B. */
    .name = "Waveshare ESP32-S3-Touch-LCD-7B (1024x600)",
    .de = 5, .vsync = 3, .hsync = 46, .pclk = 7,
    .data = { 14, 38, 18, 17, 10,       /* B0..B4 */
              39, 0, 45, 48, 47, 21,    /* G0..G5 */
              1, 2, 42, 41, 40 },       /* R0..R4 */
    .i2c_sda = 8, .i2c_scl = 9,
    .has_ch422g = false,
    .has_ch32v003 = true,
    .bl_gpio = -1,
    .tp_rst_gpio = -1,
    .hs_pulse = 162, .hs_bp = 152, .hs_fp = 48,
    .vs_pulse = 45, .vs_bp = 13, .vs_fp = 3,
    .tp_mirror = false,
};

__attribute__((unused)) static const board_cfg_t k_waveshare_7c = {
    /* The 7C "BOX": same 800x480 glass family as the classic 7, but the
     * G5/G6 data lines moved to GPIO 9/8 and the I2C bus to 47/48 (the
     * classic 7 uses 8/9 for I2C). The IO helper at 0x24 speaks the
     * CH32V003 register protocol known from the 7B (0x03 outputs, 0x05
     * backlight PWM), TP_RST is helper output 1 and touch INT is GPIO4,
     * so the whole 7B helper flow applies. ES8389 codec, battery and
     * 32 MB flash / 16 MB PSRAM on board (we use the standard 16 MB
     * layout; the extra flash stays unused for now). Pins and timings
     * verbatim from waveshareteam/ESP32-S3-Touch-LCD-7C 03_LCD. */
    .name = "Waveshare ESP32-S3-Touch-LCD-7C (800x480)",
    .de = 5, .vsync = 3, .hsync = 46, .pclk = 7,
    .data = { 14, 38, 18, 17, 10,       /* B0..B4 */
              39, 0, 45, 9, 8, 21,      /* G0..G5 (G5/G6 differ from the 7) */
              1, 2, 42, 41, 40 },       /* R0..R4 */
    .i2c_sda = 47, .i2c_scl = 48,
    .has_ch422g = false,
    .has_ch32v003 = true,
    .bl_gpio = -1,
    .tp_rst_gpio = -1,
    .hs_pulse = 8, .hs_bp = 8, .hs_fp = 4,
    .vs_pulse = 8, .vs_bp = 8, .vs_fp = 4,
    .tp_mirror = false,
};

__attribute__((unused)) static const board_cfg_t k_guition = {
    .name = "Guition JC8048W550",
    .de = 40, .vsync = 41, .hsync = 39, .pclk = 42,
    .data = { 8, 3, 46, 9, 1,           /* B0..B4 */
              5, 6, 7, 15, 16, 4,       /* G0..G5 */
              45, 48, 47, 21, 14 },     /* R0..R4 */
    .i2c_sda = 19, .i2c_scl = 20,
    .has_ch422g = false,
    .bl_gpio = 2,
    .tp_rst_gpio = 38,
    .hs_pulse = 4, .hs_bp = 8, .hs_fp = 8,
    .vs_pulse = 4, .vs_bp = 8, .vs_fp = 8,
    .tp_mirror = false,
};

__attribute__((unused)) static const board_cfg_t k_crowpanel_50 = {
    /* Elecrow CrowPanel 5.0 (DIS07050H): Guition-family RGB wiring with
     * PCLK moved to GPIO0 (a strapping pin - harmless after boot) and no
     * touch reset line under our control. Pin map from the ESPHome
     * devices database (working configs). 4 MB flash: pairs with the
     * partitions-4mb.csv single-slot layout, never auto-detected. */
    .name = "Elecrow CrowPanel 5.0 (4MB)",
    .de = 40, .vsync = 41, .hsync = 39, .pclk = 0,
    .data = { 8, 3, 46, 9, 1,           /* B0..B4 */
              5, 6, 7, 15, 16, 4,       /* G0..G5 */
              45, 48, 47, 21, 14 },     /* R0..R4 */
    .i2c_sda = 19, .i2c_scl = 20,
    .has_ch422g = false,
    .bl_gpio = 2,
    .tp_rst_gpio = -1,
    /* Classic wide 5" TFT window, confirmed on hardware: the Guition's
     * tight 8/8 porches shifted the display window and blanked ~40% of
     * the panel on the left (hardware report + A/B/C test builds). */
    .hs_pulse = 4, .hs_bp = 40, .hs_fp = 40,
    .vs_pulse = 4, .vs_bp = 30, .vs_fp = 13,
    .tp_mirror = false,
};

__attribute__((unused)) static const board_cfg_t k_crowpanel_70 = {
    /* Elecrow CrowPanel 7.0 (DIS08070H) and its Amazon rebrands (e.g.
     * "IoTeikXgo" 7" HMI, identified by the EK9716BD3 driver and the
     * Elecrow support links on the listing). N4R8 module = 4 MB class.
     * PCLK on GPIO0 like the CrowPanel 5.0, sync on 41/40/39, and note
     * the R/B halves of the bus are swapped versus the Sunton 7". Pin
     * map and timings from Elecrow's official 7.0 v3.0 LVGL demo. */
    .name = "Elecrow CrowPanel 7.0 (4MB)",
    .de = 41, .vsync = 40, .hsync = 39, .pclk = 0,
    .data = { 15, 7, 6, 5, 4,           /* B0..B4 */
              9, 46, 3, 8, 16, 1,       /* G0..G5 */
              14, 21, 47, 48, 45 },     /* R0..R4 */
    .i2c_sda = 19, .i2c_scl = 20,
    .has_ch422g = false,
    .bl_gpio = 2,
    .tp_rst_gpio = -1,
    .hs_pulse = 48, .hs_bp = 40, .hs_fp = 40,
    .vs_pulse = 31, .vs_bp = 13, .vs_fp = 1,
    .pclk_hz = 15000000,
    .tp_mirror = false,
};

__attribute__((unused)) static const board_cfg_t k_crowpanel_adv70 = {
    /* Elecrow CrowPanel Advance 7.0 (DIS02170A), hardware V1.2 and newer
     * (V1.3/V1.4/V1.5 share one pin map). N16R8 module: full 16 MB class.
     * Pins and timings verbatim from Elecrow's official ESPHome config in
     * the V1.3_and_V1.4_and_V1.5 example folder, cross-checked against
     * espboards.dev; their IDF sample in the same folder still carries
     * Waveshare-era DE/backlight code and is NOT the reference. Note the
     * sync pins: HSYNC 40, VSYNC 41, DE 42, PCLK 39 - close to the Guition
     * family but not the same. The backlight is an STC8 helper MCU at I2C
     * 0x30 taking one byte (0 = brightest .. 244 = dimmest, 245 = off);
     * the vendor boot sequence writes 250 then 0. GT911 on I2C 15/16 at
     * 0x5D with no reset line under our control. V1.0 boards drove the
     * backlight through a PCA9557 instead and are not covered here. */
    .name = "Elecrow CrowPanel Advance 7.0 (V1.2+)",
    .de = 42, .vsync = 41, .hsync = 40, .pclk = 39,
    .data = { 21, 47, 48, 45, 38,       /* B0..B4 */
              9, 10, 11, 12, 13, 14,    /* G0..G5 */
              7, 17, 18, 3, 46 },       /* R0..R4 */
    .i2c_sda = 15, .i2c_scl = 16,
    .has_ch422g = false,
    .has_stc8 = true,
    .bl_gpio = -1,
    .tp_rst_gpio = -1,
    .hs_pulse = 4, .hs_bp = 8, .hs_fp = 8,
    .vs_pulse = 4, .vs_bp = 8, .vs_fp = 8,
    .pclk_hz = 16000000,
    .tp_mirror = false,
};

__attribute__((unused)) static const board_cfg_t k_pandatouch = {
    /* BigTreeTech Panda Touch: the 5" 800x480 Bambu-printer pendant is a
     * plain ESP32-S3 R8 (16 MB flash) smart display underneath, and BTT
     * publishes the full pin map in their own IDF SDK (PandaTouch_IDF,
     * include/pandatouch_board.h) - pins and timings below are verbatim
     * from there. DE-only panel: HSYNC/VSYNC are not wired, the esp_lcd
     * RGB driver takes -1 for both. The panel has a real reset line on
     * GPIO46 that must be released before init. Battery + enclosure
     * on board; flashing this replaces BTT's Bambu-remote firmware. */
    .name = "BigTreeTech Panda Touch",
    .de = 38, .vsync = -1, .hsync = -1, .pclk = 5,
    .data = { 17, 18, 48, 47, 39,       /* B0..B4 */
              11, 12, 13, 14, 15, 16,   /* G0..G5 */
              6, 7, 8, 9, 10 },         /* R0..R4 */
    .i2c_sda = 2, .i2c_scl = 1,
    .has_ch422g = false,
    .bl_gpio = 21,
    .tp_rst_gpio = 41,
    .hs_pulse = 4, .hs_bp = 8, .hs_fp = 8,
    .vs_pulse = 4, .vs_bp = 16, .vs_fp = 16,
    .pclk_hz = 23000000,
    .tp_mirror = false,
    .lcd_rst_gpio = 46,
};

__attribute__((unused)) static const board_cfg_t k_sunton_8070 = {
    /* Sunton ESP32-8048S070 (7" 800x480, ST7262) and its unbranded
     * Amazon/AliExpress clones. NOT Guition-compatible: DE and VSYNC are
     * swapped versus the Guition, so that build shows garbage here.
     * Timings from rzeldent/esp32-smartdisplay; its data map however had
     * the R and B halves swapped - hardware-confirmed by an owner
     * (2026-08: red rendered blue until the halves were flipped). The
     * corrected bus is byte-for-byte the CrowPanel 7.0 wiring; only the
     * PCLK pin (42 vs GPIO0) and the panel timings differ. Note the slow
     * 12.5 MHz pclk and the unusual 210-pixel HSYNC front porch - both
     * are what the 7" panel actually wants. */
    .name = "Sunton ESP32-8048S070 (7in 800x480)",
    .de = 41, .vsync = 40, .hsync = 39, .pclk = 42,
    .data = { 15, 7, 6, 5, 4,           /* B0..B4 */
              9, 46, 3, 8, 16, 1,       /* G0..G5 */
              14, 21, 47, 48, 45 },     /* R0..R4 */
    .i2c_sda = 19, .i2c_scl = 20,
    .has_ch422g = false,
    .bl_gpio = 2,
    .tp_rst_gpio = 38,
    /* Horizontal porches re-balanced by the same owner (16/210 -> 46/180,
     * total unchanged): centers the display window, which sat shifted
     * ~30 px with the smartdisplay values. */
    .hs_pulse = 30, .hs_bp = 46, .hs_fp = 180,
    .vs_pulse = 13, .vs_bp = 10, .vs_fp = 22,
    .pclk_hz = 12500000,
    .tp_mirror = false,
};

__attribute__((unused)) static const board_cfg_t k_sunton_4827 = {
    /* Sunton ESP32-4827S043 (4.3" 480x272): electrically the Guition
     * JC8048W550's smaller sibling - same RGB wiring, I2C bus, backlight
     * GPIO and GT911 reset, only the panel and its timings differ.
     * Pin map and timings from rzeldent/esp32-smartdisplay (working
     * device). The resolution differs from the 800x480 family, so this
     * is a dedicated build, never part of the auto-detected binary. */
    .name = "Sunton ESP32-4827S043 (480x272)",
    .de = 40, .vsync = 41, .hsync = 39, .pclk = 42,
    .data = { 8, 3, 46, 9, 1,           /* B0..B4 */
              5, 6, 7, 15, 16, 4,       /* G0..G5 */
              45, 48, 47, 21, 14 },     /* R0..R4 */
    .i2c_sda = 19, .i2c_scl = 20,
    .has_ch422g = false,
    .bl_gpio = 2,
    .tp_rst_gpio = 38,
    .hs_pulse = 4, .hs_bp = 43, .hs_fp = 8,
    .vs_pulse = 4, .vs_bp = 12, .vs_fp = 8,
    .pclk_hz = 8 * 1000 * 1000,
    .tp_mirror = false,
};

__attribute__((unused)) static const board_cfg_t k_sunton_4827r = {
    /* Resistive-touch variant of the 4827S043: identical panel and RGB
     * wiring, XPT2046 on the SD-card SPI bus instead of the GT911. */
    .name = "Sunton ESP32-4827S043R (480x272, resistive)",
    .de = 40, .vsync = 41, .hsync = 39, .pclk = 42,
    .data = { 8, 3, 46, 9, 1,           /* B0..B4 */
              5, 6, 7, 15, 16, 4,       /* G0..G5 */
              45, 48, 47, 21, 14 },     /* R0..R4 */
    .i2c_sda = -1, .i2c_scl = -1,
    .has_ch422g = false,
    .bl_gpio = 2,
    .tp_rst_gpio = -1,
    .hs_pulse = 4, .hs_bp = 43, .hs_fp = 8,
    .vs_pulse = 4, .vs_bp = 12, .vs_fp = 8,
    .pclk_hz = 8 * 1000 * 1000,
    .tp_mirror = false,
    .has_xpt2046 = true,
    .tp_sclk = 12, .tp_mosi = 11, .tp_miso = 13, .tp_cs = 38, .tp_irq = 18,
};

static const board_cfg_t *s_board = &k_waveshare;
static esp_lcd_panel_handle_t s_panel;

const char *waveshare_lcd_board_name(void)
{
    return s_board->name;
}

void waveshare_lcd_get_res(int *w, int *h)
{
    *w = EXAMPLE_LCD_H_RES;
    *h = EXAMPLE_LCD_V_RES;
}

void *waveshare_lcd_get_fb(void)
{
    if (s_panel == NULL) {
        return NULL;
    }
    void *fb0 = NULL, *fb1 = NULL;
    /* single-framebuffer builds (7B) have no second buffer to ask for */
    if (esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb0) != ESP_OK &&
        esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1) != ESP_OK) {
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

static esp_err_t i2c_master_init(int sda, int scl)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &i2c_conf);
    return i2c_driver_install(I2C_MASTER_NUM, i2c_conf.mode, 0, 0, 0);
}

/* CH422G I2C expander: raw writes, 0x24 = mode reg (0x01 -> push-pull out),
 * 0x38 = EXIO0-7 output byte. EXIO1=TP_RST, EXIO2=backlight, EXIO3=LCD_RST. */
static esp_err_t ch422g_write(uint8_t addr, uint8_t val)
{
    return i2c_master_write_to_device(I2C_MASTER_NUM, addr, &val, 1,
                                      I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}

/* CH32V003 helper MCU on the 7B (I2C 0x24): register writes, not the
 * CH422G address scheme. 0x02 pin modes, 0x03 output byte, 0x05 PWM.
 * IO1 = TP_RST, IO2 = backlight enable, IO3 = LCD_RST, IO4 = SD_CS. */
static uint8_t s_ch32_out = 0xFF;

/* CrowPanel Advance backlight helper: one raw byte to the STC8 at 0x30.
 * 0 = brightest, 244 = dimmest, 245 = off; Elecrow's boot sequence sends
 * 250 first (helper reset/handshake) and then the level. */
static esp_err_t stc8_write(uint8_t val)
{
    return i2c_master_write_to_device(I2C_MASTER_NUM, 0x30, &val, 1,
                                      I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}

static esp_err_t ch32v003_reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(I2C_MASTER_NUM, 0x24, buf, 2,
                                      I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}

static void ch32v003_output(uint8_t pin, uint8_t value)
{
    if (value) {
        s_ch32_out |= (1 << pin);
    } else {
        s_ch32_out &= ~(1 << pin);
    }
    ch32v003_reg_write(0x03, s_ch32_out);
}

/* 0-100; the vendor driver caps at 97 because 100 makes the panel flicker.
 * The PWM register is inverted (measured on hardware: 90 -> ~10% light),
 * so the duty is written as 100-pct. */
static void ch32v003_backlight_pct(int pct)
{
    if (pct > 97) pct = 97;
    if (pct < 0) pct = 0;
    ch32v003_reg_write(0x05, (uint8_t)((100 - pct) * 255 / 100));
}

/* Board detection: only the Waveshare has the CH422G expander, and probing
 * an I2C address is harmless on the Guition (those pins are RGB data lines,
 * still idle at this point; I2C is open-drain). */
static void board_detect(void)
{
#if CONFIG_CANFLIGHT_BOARD_WAVESHARE_7
    s_board = &k_waveshare;
    i2c_master_init(s_board->i2c_sda, s_board->i2c_scl);
    ch422g_write(0x24, 0x01);
    ch422g_write(0x38, 0x2A);           /* backlight off until the first frame */
#elif CONFIG_CANFLIGHT_BOARD_GUITION_JC8048W550
    s_board = &k_guition;
    i2c_master_init(s_board->i2c_sda, s_board->i2c_scl);
#elif CONFIG_CANFLIGHT_BOARD_WAVESHARE_7B
    s_board = &k_waveshare_7b;
    i2c_master_init(s_board->i2c_sda, s_board->i2c_scl);
    /* all helper-MCU pins to output, everything released (high) */
    ch32v003_reg_write(0x02, 0xFF);
    ch32v003_reg_write(0x03, s_ch32_out);
#elif CONFIG_CANFLIGHT_BOARD_CROWPANEL_50
    s_board = &k_crowpanel_50;
    i2c_master_init(s_board->i2c_sda, s_board->i2c_scl);
#elif CONFIG_CANFLIGHT_BOARD_CROWPANEL_70
    s_board = &k_crowpanel_70;
    i2c_master_init(s_board->i2c_sda, s_board->i2c_scl);
#elif CONFIG_CANFLIGHT_BOARD_SUNTON_8070
    s_board = &k_sunton_8070;
    i2c_master_init(s_board->i2c_sda, s_board->i2c_scl);
#elif CONFIG_CANFLIGHT_BOARD_PANDATOUCH
    s_board = &k_pandatouch;
    i2c_master_init(s_board->i2c_sda, s_board->i2c_scl);
#elif CONFIG_CANFLIGHT_BOARD_CROWPANEL_ADV70
    s_board = &k_crowpanel_adv70;
    i2c_master_init(s_board->i2c_sda, s_board->i2c_scl);
    stc8_write(250);                    /* vendor boot handshake */
    vTaskDelay(pdMS_TO_TICKS(50));
    stc8_write(245);                    /* backlight stays off until bl_on */
#elif CONFIG_CANFLIGHT_BOARD_WAVESHARE_7C
    s_board = &k_waveshare_7c;
    i2c_master_init(s_board->i2c_sda, s_board->i2c_scl);
    /* same helper protocol as the 7B: all pins to output, released */
    ch32v003_reg_write(0x02, 0xFF);
    ch32v003_reg_write(0x03, s_ch32_out);
#elif CONFIG_CANFLIGHT_BOARD_SUNTON_4827S043
    s_board = &k_sunton_4827;
    i2c_master_init(s_board->i2c_sda, s_board->i2c_scl);
#elif CONFIG_CANFLIGHT_BOARD_SUNTON_4827S043R
    s_board = &k_sunton_4827r;   /* no I2C: touch is SPI, no expander */
#else
    i2c_master_init(k_waveshare.i2c_sda, k_waveshare.i2c_scl);
    if (ch422g_write(0x24, 0x01) == ESP_OK) {
        s_board = &k_waveshare;
        ch422g_write(0x38, 0x2A);       /* backlight off until the first frame */
    } else {
        s_board = &k_guition;
        i2c_driver_delete(I2C_MASTER_NUM);
        i2c_master_init(s_board->i2c_sda, s_board->i2c_scl);
    }
#endif
    ESP_LOGI(TAG, "board: %s", s_board->name);
}

static void touch_reset(void)
{
    if (s_board->has_ch32v003) {
        gpio_config_t io_conf = {
            .intr_type = GPIO_INTR_DISABLE,
            .pin_bit_mask = 1ULL << GPIO_TOUCH_INT,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io_conf);

        /* LCD panel reset first (IO3), then GT911 with INT held low so it
         * comes up at address 0x5D, exactly like the CH422G flow */
        ch32v003_output(3, 0);
        esp_rom_delay_us(20 * 1000);
        ch32v003_output(3, 1);
        esp_rom_delay_us(120 * 1000);

        ch32v003_output(1, 0);              /* TP_RST low */
        esp_rom_delay_us(100 * 1000);
        gpio_set_level(GPIO_TOUCH_INT, 0);  /* INT low during reset -> addr 0x5D */
        esp_rom_delay_us(100 * 1000);
        ch32v003_output(1, 1);              /* TP_RST high */
        esp_rom_delay_us(200 * 1000);
    } else if (s_board->has_ch422g) {
        gpio_config_t io_conf = {
            .intr_type = GPIO_INTR_DISABLE,
            .pin_bit_mask = 1ULL << GPIO_TOUCH_INT,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io_conf);

        /* Output bits: 0x02 TP_RST, 0x04 backlight (DISP), 0x08 LCD_RST.
         * The old values 0x2C/0x2E had the backlight bit set, lighting
         * the panel for ~400 ms before it had a picture: the white flash
         * at every boot. The backlight stays dark until bl_on. */
        ch422g_write(0x24, 0x01);
        ch422g_write(0x38, 0x28);           /* TP_RST low, backlight off */
        esp_rom_delay_us(100 * 1000);
        gpio_set_level(GPIO_TOUCH_INT, 0);  /* INT low during reset -> addr 0x5D */
        esp_rom_delay_us(100 * 1000);
        ch422g_write(0x38, 0x2A);           /* TP_RST high, backlight off */
        esp_rom_delay_us(200 * 1000);
    } else {
        if (s_board->tp_rst_gpio < 0) {
            /* CrowPanel family: GT911 reset is not ours to pulse; the
             * controller comes up on its own RC reset */
            vTaskDelay(pdMS_TO_TICKS(200));
            return;
        }
        gpio_config_t io_conf = {
            .intr_type = GPIO_INTR_DISABLE,
            .pin_bit_mask = 1ULL << s_board->tp_rst_gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io_conf);
        gpio_set_level(s_board->tp_rst_gpio, 0);
        esp_rom_delay_us(100 * 1000);
        gpio_set_level(s_board->tp_rst_gpio, 1);
        esp_rom_delay_us(200 * 1000);
    }
}

esp_err_t waveshare_esp32_s3_rgb_lcd_init(void)
{
    board_detect();

    if (s_board->lcd_rst_gpio > 0) {
        /* Panda Touch class: panel held in reset until this line goes high */
        gpio_config_t rst_conf = {
            .intr_type = GPIO_INTR_DISABLE,
            .pin_bit_mask = 1ULL << s_board->lcd_rst_gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&rst_conf);
        gpio_set_level(s_board->lcd_rst_gpio, 0);
        esp_rom_delay_us(20 * 1000);
        gpio_set_level(s_board->lcd_rst_gpio, 1);
        esp_rom_delay_us(120 * 1000);
    }

    ESP_LOGI(TAG, "Install RGB LCD panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = s_board->pclk_hz ? s_board->pclk_hz
                                        : EXAMPLE_LCD_PIXEL_CLOCK_HZ,
            .h_res = EXAMPLE_LCD_H_RES,
            .v_res = EXAMPLE_LCD_V_RES,
            .hsync_pulse_width = s_board->hs_pulse,
            .hsync_back_porch = s_board->hs_bp,
            .hsync_front_porch = s_board->hs_fp,
            .vsync_pulse_width = s_board->vs_pulse,
            .vsync_back_porch = s_board->vs_bp,
            .vsync_front_porch = s_board->vs_fp,
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
        .hsync_gpio_num = s_board->hsync,
        .vsync_gpio_num = s_board->vsync,
        .de_gpio_num = s_board->de,
        .pclk_gpio_num = s_board->pclk,
        .disp_gpio_num = -1,
        .flags = {
            .fb_in_psram = 1,
        },
    };
    for (int i = 0; i < 16; i++) {
        panel_config.data_gpio_nums[i] = s_board->data[i];
    }
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    s_panel = panel_handle;

    esp_lcd_touch_handle_t tp_handle = NULL;
#if CONFIG_CANFLIGHT_BOARD_SUNTON_4827S043R
    ESP_LOGI(TAG, "Initialize XPT2046 touch");
    spi_bus_config_t buscfg = {
        .sclk_io_num = s_board->tp_sclk,
        .mosi_io_num = s_board->tp_mosi,
        .miso_io_num = s_board->tp_miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg =
        ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(s_board->tp_cs);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                             &tp_io_cfg, &tp_io));
    esp_lcd_touch_config_t xpt_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = s_board->tp_irq,
    };
    if (esp_lcd_touch_new_spi_xpt2046(tp_io, &xpt_cfg, &tp_handle) != ESP_OK) {
        ESP_LOGE(TAG, "XPT2046 init failed - running without touch");
        tp_handle = NULL;
    }
#else
    ESP_LOGI(TAG, "Initialize GT911 touch");
    touch_reset();

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    /* Legacy i2c driver sets the bus speed itself; v1 io rejects a non-zero value here. */
    tp_io_config.scl_speed_hz = 0;

    esp_lcd_touch_config_t tp_cfg = {
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
            .mirror_x = s_board->tp_mirror ? 1 : 0,
            .mirror_y = s_board->tp_mirror ? 1 : 0,
        },
    };

    /* Without the INT-pin trick the GT911 can come up on either address;
     * try the default, fall back to the alternate. */
    esp_err_t terr = ESP_FAIL;
    for (int attempt = 0; attempt < 2 && terr != ESP_OK; attempt++) {
        tp_io_config.dev_addr = attempt == 0
                                    ? ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS
                                    : ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        if (esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2C_MASTER_NUM,
                                     &tp_io_config, &tp_io_handle) != ESP_OK) {
            continue;
        }
        terr = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp_handle);
        if (terr != ESP_OK) {
            esp_lcd_panel_io_del(tp_io_handle);
            tp_io_handle = NULL;
            ESP_LOGW(TAG, "GT911 not at 0x%02x, trying alternate",
                     (unsigned)tp_io_config.dev_addr);
        }
    }
    if (terr != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed - running without touch");
        tp_handle = NULL;
    }
#endif /* touch variant */

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

static esp_err_t bl_gpio_set(int level)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = 1ULL << s_board->bl_gpio,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    return gpio_set_level(s_board->bl_gpio, level);
}

esp_err_t waveshare_rgb_lcd_bl_on(void)
{
    if (s_board->has_stc8) {
        return stc8_write(0);
    }
    if (s_board->has_ch32v003) {
        ch32v003_output(2, 1);
        ch32v003_backlight_pct(90);
        return ESP_OK;
    }
    if (!s_board->has_ch422g) {
        return bl_gpio_set(1);
    }
    ch422g_write(0x24, 0x01);
    return ch422g_write(0x38, 0x1E);
}

bool waveshare_rgb_lcd_bl_dimmable(void)
{
    /* CH32V003 and STC8 helpers PWM natively; a plain backlight GPIO can
     * be driven by LEDC. The CH422G expander pin is on/off only. */
    return s_board->has_ch32v003 || s_board->has_stc8 || s_board->bl_gpio >= 0;
}

esp_err_t waveshare_rgb_lcd_bl_pct(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    if (s_board->has_ch32v003) {
        ch32v003_backlight_pct(pct);
        return ESP_OK;
    }
    if (s_board->has_stc8) {
        /* 0 = brightest .. 244 = dimmest (245 would switch off) */
        return stc8_write((uint8_t)((100 - pct) * 244 / 100));
    }
    if (s_board->bl_gpio >= 0) {
        /* lazily hang a LEDC channel on the backlight pin; bl_on/bl_off
         * keep using plain GPIO writes, which re-claim the pin from the
         * matrix, so re-attach on every call */
        static bool timer_ready;
        if (!timer_ready) {
            const ledc_timer_config_t timer = {
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .duty_resolution = LEDC_TIMER_10_BIT,
                .timer_num = LEDC_TIMER_1,
                .freq_hz = 5000,
                .clk_cfg = LEDC_AUTO_CLK,
            };
            ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), "bl", "ledc timer");
            timer_ready = true;
        }
        const ledc_channel_config_t ch = {
            .gpio_num = s_board->bl_gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_2,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_1,
            .duty = (uint32_t)(1023 * pct / 100),
            .hpoint = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), "bl", "ledc chan");
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t waveshare_rgb_lcd_bl_off(void)
{
    if (s_board->has_stc8) {
        return stc8_write(245);
    }
    if (s_board->has_ch32v003) {
        /* PWM 0 alone is fully dark. Do NOT drop helper output 2 here:
         * a 7B owner reported the panel could not be woken by touch in
         * night mode - cutting that line takes the touch controller down
         * with the backlight, so the tap-to-wake path never fires. */
        ch32v003_backlight_pct(0);
        return ESP_OK;
    }
    if (!s_board->has_ch422g) {
        return bl_gpio_set(0);
    }
    ch422g_write(0x24, 0x01);
    return ch422g_write(0x38, 0x1A);
}
