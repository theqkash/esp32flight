#include "app_port.h"

#include <SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* Read by the flight task: Android moved the app off screen. */
volatile bool g_app_background;

/* LVGL over SDL2: one window, RGB565 texture, mouse as the touch pad.
 * The same file works on macOS (desktop window) and Android (SDL activity
 * fullscreen; the window size request is ignored and content is scaled).
 *
 * Canvas size is adaptive: the UI was designed for 800x480, but the layout
 * is elastic, so on big/dense screens (tablets) we register a larger LVGL
 * canvas at an integer fraction of the physical resolution. Elements keep
 * a sane physical size, more content fits, and the integer scale keeps the
 * output crisp. Screens too small/dense for a >=800x480 canvas at a readable
 * density fall back to the original stretch of 800x480. */

#define DESIGN_W 800
#define DESIGN_H 480

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static pthread_mutex_t s_lvgl_mux;

static int s_lw = DESIGN_W;   /* logical canvas size (LVGL resolution) */
static int s_lh = DESIGN_H;

static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t *s_buf1;
static lv_color_t *s_fb;

static volatile int s_mouse_x, s_mouse_y;
static volatile bool s_mouse_down;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    for (int y = area->y1; y <= area->y2; y++) {
        lv_color_t *dst = &s_fb[y * s_lw + area->x1];
        int w = area->x2 - area->x1 + 1;
        memcpy(dst, px, w * sizeof(lv_color_t));
        px += w;
    }
    lv_disp_flush_ready(drv);
}

static void mouse_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->point.x = s_mouse_x;
    data->point.y = s_mouse_y;
    data->state = s_mouse_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* Desktop test override: --size WxH sets the window and canvas size. */
static int s_req_w = DESIGN_W, s_req_h = DESIGN_H;

void app_port_set_size(int w, int h)
{
    if (w >= DESIGN_W && h >= DESIGN_H) {
        s_req_w = w;
        s_req_h = h;
    }
}

#ifdef __ANDROID__
/* Pick the canvas as physical/z for the integer zoom z whose resulting
 * logical density lands closest to the ESP panel's ~133 dpi. Handhelds are
 * viewed closer, so they may go denser before falling back to stretch. */
static void pick_logical_size(void)
{
    /* The first surface can still carry system-bar insets; immersive mode
     * kicks in a moment later and grows it. Wait until the size settles. */
    int pw = 0, ph = 0;
    for (int stable = 0, i = 0; i < 20 && stable < 4; i++) {
        int w = 0, h = 0;
        SDL_PumpEvents();
        SDL_GetRendererOutputSize(s_ren, &w, &h);
        if (w == pw && h == ph) {
            stable++;
        } else {
            pw = w;
            ph = h;
            stable = 0;
        }
        SDL_Delay(50);
    }
    if (pw < DESIGN_W || ph < DESIGN_H || pw <= 0 || ph <= 0) {
        SDL_Log("apkflight: screen %dx%d -> stretched %dx%d canvas",
                pw, ph, DESIGN_W, DESIGN_H);
        return;
    }
    float ddpi = 0.0f;
    if (SDL_GetDisplayDPI(0, &ddpi, NULL, NULL) != 0 || ddpi <= 0.0f) {
        /* no DPI info: assume a ~6.5" phone so big screens still upgrade */
        ddpi = sqrtf((float)pw * pw + (float)ph * ph) / 6.5f;
    }
    float diag_in = sqrtf((float)pw * pw + (float)ph * ph) / ddpi;
    float max_ldpi = diag_in < 7.0f ? 240.0f : 200.0f;

    int best = 0;
    float best_score = 1e9f;
    for (int z = 1; z <= 4; z++) {
        if (pw / z < DESIGN_W || ph / z < DESIGN_H) {
            break;
        }
        float score = fabsf(ddpi / z - 140.0f);
        if (score < best_score) {
            best_score = score;
            best = z;
        }
    }
    if (best > 0 && ddpi / best <= max_ldpi) {
        s_lw = pw / best;
        s_lh = ph / best;
    }
    SDL_Log("apkflight: screen %dx%d, %.0f dpi, %.1f\" -> canvas %dx%d%s",
            pw, ph, (double)ddpi, (double)diag_in, s_lw, s_lh,
            s_lw == DESIGN_W && s_lh == DESIGN_H ? " (stretched)" : "");
}
#endif

int app_port_init(const char *title)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_lvgl_mux, &a);

    /* Touch drives LVGL directly via finger events; don't also synthesize
     * mouse events (they'd arrive in raw window pixels and fight the
     * normalized finger coords once we drop the logical size). */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    /* The settings screen has its own LVGL keyboard - suppress the Android
     * system IME so it doesn't cover the UI and block the Save button. */
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    SDL_DisableScreenSaver();   /* a desk radar should never dim out */

    Uint32 wflags = 0;
#ifdef __ANDROID__
    wflags |= SDL_WINDOW_FULLSCREEN;   /* immersive, no status/nav bars */
#else
    s_lw = s_req_w;
    s_lh = s_req_h;
#endif
    s_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             s_lw, s_lh, wflags);
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED |
                                          SDL_RENDERER_PRESENTVSYNC);
    if (s_win == NULL || s_ren == NULL) {
        fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
        return -1;
    }
#ifdef __ANDROID__
    pick_logical_size();
#endif

    /* No logical size on the renderer: the canvas is stretched to fill the
     * whole screen (RenderCopy dst = NULL), so no black letterbox bars. At
     * the usual integer scale the linear filter is pixel-exact; in stretch
     * mode it smooths what used to be hard pixel steps. */
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, s_lw, s_lh);
    if (s_tex == NULL) {
        fprintf(stderr, "SDL texture failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetTextureScaleMode(s_tex, SDL_ScaleModeLinear);

    s_buf1 = malloc((size_t)s_lw * s_lh * sizeof(lv_color_t));
    s_fb = calloc((size_t)s_lw * s_lh, sizeof(lv_color_t));
    if (s_buf1 == NULL || s_fb == NULL) {
        fprintf(stderr, "framebuffer alloc failed\n");
        return -1;
    }

    lv_init();
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, NULL, s_lw * s_lh);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = s_lw;
    disp_drv.ver_res = s_lh;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = mouse_cb;
    lv_indev_drv_register(&indev_drv);
    return 0;
}

bool lvgl_port_lock(int timeout_ms)
{
    (void)timeout_ms;
    return pthread_mutex_lock(&s_lvgl_mux) == 0;
}

void lvgl_port_unlock(void)
{
    pthread_mutex_unlock(&s_lvgl_mux);
}

int app_port_run(int shot_after_ms, const char *shot_path)
{
    uint32_t start = SDL_GetTicks();
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            /* screenshot runs are headless verification: a stray desktop
             * click on the fresh window must not drive the UI */
            if (shot_after_ms > 0 && e.type != SDL_QUIT) {
                continue;
            }
            switch (e.type) {
            case SDL_APP_DIDENTERBACKGROUND:
                g_app_background = true;
                break;
            case SDL_APP_WILLENTERFOREGROUND:
            case SDL_APP_DIDENTERFOREGROUND:
                g_app_background = false;
                break;
            case SDL_QUIT:
                running = false;
                break;
            case SDL_MOUSEMOTION:
                s_mouse_x = e.motion.x;
                s_mouse_y = e.motion.y;
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                s_mouse_x = e.button.x;
                s_mouse_y = e.button.y;
                s_mouse_down = e.button.state == SDL_PRESSED;
                break;
            /* Android sends touch as finger events */
            case SDL_FINGERDOWN:
            case SDL_FINGERUP:
            case SDL_FINGERMOTION:
                s_mouse_x = (int)(e.tfinger.x * s_lw);
                s_mouse_y = (int)(e.tfinger.y * s_lh);
                s_mouse_down = e.type != SDL_FINGERUP;
                break;
            }
        }

        lvgl_port_lock(-1);
        lv_timer_handler();
        lvgl_port_unlock();

        SDL_UpdateTexture(s_tex, NULL, s_fb, s_lw * sizeof(lv_color_t));
        SDL_RenderClear(s_ren);
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        SDL_RenderPresent(s_ren);

        if (shot_after_ms > 0 && SDL_GetTicks() - start > (uint32_t)shot_after_ms) {
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(
                0, s_lw, s_lh, 16, SDL_PIXELFORMAT_RGB565);
            memcpy(surf->pixels, s_fb, (size_t)s_lw * s_lh * sizeof(lv_color_t));
            SDL_SaveBMP(surf, shot_path);
            SDL_FreeSurface(surf);
            printf("screenshot: %s\n", shot_path);
            running = false;
        }
        SDL_Delay(5);
    }
    return 0;
}

/* Live RGB565 framebuffer for the web panel's /screen.bmp */
void *waveshare_lcd_get_fb(void)
{
    return s_fb;
}

void waveshare_lcd_get_res(int *w, int *h)
{
    *w = s_lw;
    *h = s_lh;
}

/* settings text fields paste this on long-press */
const char *apk_clipboard_text(void)
{
    static char buf[160];
    char *t = SDL_GetClipboardText();
    if (t == NULL) {
        return "";
    }
    strlcpy(buf, t, sizeof(buf));
    SDL_free(t);
    return buf;
}
