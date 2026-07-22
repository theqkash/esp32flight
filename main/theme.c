#include "theme.h"
#include "settings.h"

static const struct {
    uint32_t bg, panel, row, row_sel, accent, text, dim;
} k_themes[THEME_COUNT] = {
    /* Dark      */ { 0x0b0f1a, 0x141b2d, 0x1a2338, 0x24457a, 0x4da3ff, 0xe8edf5, 0x8794ad },
    /* Light     */ { 0xf1f4f9, 0xffffff, 0xe3e9f2, 0xb9d3f5, 0x1f6fd6, 0x18202e, 0x5c6a84 },
    /* Black     */ { 0x000000, 0x0c0c10, 0x17171d, 0x1f3a66, 0x4da3ff, 0xededed, 0x8a8a94 },
    /* Nord      */ { 0x2e3440, 0x3b4252, 0x434c5e, 0x5e81ac, 0x88c0d0, 0xeceff4, 0x9aa5b8 },
    /* Solarized */ { 0x002b36, 0x073642, 0x0a4250, 0x1c6ea4, 0x2aa198, 0xfdf6e3, 0x839496 },
    /* Purple    */ { 0x12081f, 0x1d1130, 0x2a1a44, 0x5b2d91, 0xb388ff, 0xefe9f7, 0x8f7fae },
    /* Forest    */ { 0x0c1510, 0x15241b, 0x1d3226, 0x2e6b45, 0x5dd39e, 0xe8f5ee, 0x7fa38f },
};

const app_theme_t *app_theme(void)
{
    static app_theme_t t;
    static int loaded_idx = -1;

    int idx = settings_get()->theme;
    if (idx < 0 || idx >= THEME_COUNT) {
        idx = 0;
    }
    if (idx != loaded_idx) {
        t.bg = lv_color_hex(k_themes[idx].bg);
        t.panel = lv_color_hex(k_themes[idx].panel);
        t.row = lv_color_hex(k_themes[idx].row);
        t.row_sel = lv_color_hex(k_themes[idx].row_sel);
        t.accent = lv_color_hex(k_themes[idx].accent);
        t.text = lv_color_hex(k_themes[idx].text);
        t.dim = lv_color_hex(k_themes[idx].dim);
        loaded_idx = idx;
    }
    return &t;
}

const char *theme_names_option_string(void)
{
    return "Dark\nLight\nBlack\nNord\nSolarized\nPurple\nForest";
}
