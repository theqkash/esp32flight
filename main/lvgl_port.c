/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include "lvgl_port.h"

static const char *TAG = "lv_port";                      // Tag for logging
static SemaphoreHandle_t lvgl_mux;                       // LVGL mutex for synchronization
static TaskHandle_t lvgl_task_handle = NULL;             // Handle for the LVGL task

#if EXAMPLE_LVGL_PORT_ROTATION_DEGREE != 0
// Function to get the next frame buffer for double buffering
static void *get_next_frame_buffer(esp_lcd_panel_handle_t panel_handle)
{
    static void *next_fb = NULL;                          // Pointer to the next frame buffer
    static void *fb[2] = { NULL };                        // Array to hold two frame buffers
    if (next_fb == NULL) {
        ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &fb[0], &fb[1])); // Get the frame buffers
        next_fb = fb[1];                                  // Initialize to the second buffer
    } else {
        // Toggle between the two frame buffers
        next_fb = (next_fb == fb[0]) ? fb[1] : fb[0];
    }
    return next_fb;                                       // Return the next frame buffer
}

// Function to rotate and copy pixels from one buffer to another
IRAM_ATTR static void rotate_copy_pixel(const uint16_t *from, uint16_t *to, uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end, uint16_t w, uint16_t h, uint16_t rotation)
{
    int from_index = 0;                                   // Index for source buffer
    int to_index = 0;                                     // Index for destination buffer
    int to_index_const = 0;                               // Constant index for destination buffer

    switch (rotation) {
    case 90:
        to_index_const = (w - x_start - 1) * h;          // Calculate constant index for 90-degree rotation
        for (int from_y = y_start; from_y < y_end + 1; from_y++) {
            from_index = from_y * w + x_start;           // Calculate index in the source buffer
            to_index = to_index_const + from_y;          // Calculate index in the destination buffer
            for (int from_x = x_start; from_x < x_end + 1; from_x++) {
                *(to + to_index) = *(from + from_index);  // Copy pixel
                from_index += 1;                          // Move to the next pixel in the source
                to_index -= h;                            // Move to the next pixel in the destination
            }
        }
        break;
    case 180:
        to_index_const = h * w - x_start - 1;            // Calculate constant index for 180-degree rotation
        for (int from_y = y_start; from_y < y_end + 1; from_y++) {
            from_index = from_y * w + x_start;           // Calculate index in the source buffer
            to_index = to_index_const - from_y * w;      // Calculate index in the destination buffer
            for (int from_x = x_start; from_x < x_end + 1; from_x++) {
                *(to + to_index) = *(from + from_index);  // Copy pixel
                from_index += 1;                          // Move to the next pixel in the source
                to_index -= 1;                            // Move to the next pixel in the destination
            }
        }
        break;
    case 270:
        to_index_const = (x_start + 1) * h - 1;          // Calculate constant index for 270-degree rotation
        for (int from_y = y_start; from_y < y_end + 1; from_y++) {
            from_index = from_y * w + x_start;           // Calculate index in the source buffer
            to_index = to_index_const - from_y;          // Calculate index in the destination buffer
            for (int from_x = x_start; from_x < x_end + 1; from_x++) {
                *(to + to_index) = *(from + from_index);  // Copy pixel
                from_index += 1;                          // Move to the next pixel in the source
                to_index += h;                            // Move to the next pixel in the destination
            }
        }
        break;
    default:
        break;                                             // Do nothing for unsupported rotation angles
    }
}
#endif /* EXAMPLE_LVGL_PORT_ROTATION_DEGREE */

#if LVGL_PORT_AVOID_TEAR_ENABLE
#if LVGL_PORT_DIRECT_MODE
#if EXAMPLE_LVGL_PORT_ROTATION_DEGREE != 0

// Structure to store information about dirty areas that need refreshing
typedef struct {
    uint16_t inv_p;                                   // Number of invalid areas
    uint8_t inv_area_joined[LV_INV_BUF_SIZE];       // Array to track joined invalid areas
    lv_area_t inv_areas[LV_INV_BUF_SIZE];            // Array of invalid areas
} lv_port_dirty_area_t;

// Enumeration for flush status
typedef enum {
    FLUSH_STATUS_PART,                                // Partial flush
    FLUSH_STATUS_FULL                                 // Full flush
} lv_port_flush_status_t;

// Enumeration for flush probe results
typedef enum {
    FLUSH_PROBE_PART_COPY,                           // Probe result for partial copy
    FLUSH_PROBE_SKIP_COPY,                           // Probe result to skip copy
    FLUSH_PROBE_FULL_COPY,                           // Probe result for full copy
} lv_port_flush_probe_t;

static lv_port_dirty_area_t dirty_area;             // Instance of dirty area structure

// Function to save the current dirty area information
static void flush_dirty_save(lv_port_dirty_area_t *dirty_area)
{
    lv_disp_t *disp = _lv_refr_get_disp_refreshing(); // Get the currently refreshing display
    dirty_area->inv_p = disp->inv_p;                  // Save the number of invalid areas
    for (int i = 0; i < disp->inv_p; i++) {
        dirty_area->inv_area_joined[i] = disp->inv_area_joined[i]; // Save joined areas status
        dirty_area->inv_areas[i] = disp->inv_areas[i]; // Save invalid areas
    }
}

/**
 * @brief Probe dirty area to copy
 *
 * @note This function is used to avoid tearing effect, and only works with LVGL direct mode.
 *
 */
static lv_port_flush_probe_t flush_copy_probe(lv_disp_drv_t *drv)
{
    static lv_port_flush_status_t prev_status = FLUSH_STATUS_PART; // Previous flush status
    lv_port_flush_status_t cur_status;                            // Current flush status
    lv_port_flush_probe_t probe_result;                           // Result of the probe
    lv_disp_t *disp_refr = _lv_refr_get_disp_refreshing();       // Get the currently refreshing display

    uint32_t flush_ver = 0;                                       // Vertical size to flush
    uint32_t flush_hor = 0;                                       // Horizontal size to flush
    for (int i = 0; i < disp_refr->inv_p; i++) {
        if (disp_refr->inv_area_joined[i] == 0) {
            flush_ver = (disp_refr->inv_areas[i].y2 + 1 - disp_refr->inv_areas[i].y1); // Calculate vertical flush size
            flush_hor = (disp_refr->inv_areas[i].x2 + 1 - disp_refr->inv_areas[i].x1); // Calculate horizontal flush size
            break; // Exit the loop after finding the first unjoined area
        }
    }
    /* Check if the current full screen refreshes */
    cur_status = ((flush_ver == drv->ver_res) && (flush_hor == drv->hor_res)) ? (FLUSH_STATUS_FULL) : (FLUSH_STATUS_PART);

    // Determine the probe result based on previous and current status
    if (prev_status == FLUSH_STATUS_FULL) {
        if ((cur_status == FLUSH_STATUS_PART)) {
            probe_result = FLUSH_PROBE_FULL_COPY; // Full copy needed
        } else {
            probe_result = FLUSH_PROBE_SKIP_COPY; // Skip copy
        }
    } else {
        probe_result = FLUSH_PROBE_PART_COPY; // Partial copy needed
    }
    prev_status = cur_status; // Update previous status

    return probe_result; // Return the result of the probe
}

// Inline function to get the next buffer for flushing
static inline void *flush_get_next_buf(void *panel_handle)
{
    return get_next_frame_buffer(panel_handle); // Return the next frame buffer
}

/**
 * @brief Copy dirty area
 *
 * @note This function is used to avoid tearing effect, and only works with LVGL direct mode.
 *
 */
static void flush_dirty_copy(void *dst, void *src, lv_port_dirty_area_t *dirty_area)
{
    lv_coord_t x_start, x_end, y_start, y_end; // Coordinates for the area to be copied
    for (int i = 0; i < dirty_area->inv_p; i++) {
        /* Refresh the unjoined areas */
        if (dirty_area->inv_area_joined[i] == 0) {
            x_start = dirty_area->inv_areas[i].x1; // Start X coordinate
            x_end = dirty_area->inv_areas[i].x2;   // End X coordinate
            y_start = dirty_area->inv_areas[i].y1; // Start Y coordinate
            y_end = dirty_area->inv_areas[i].y2;   // End Y coordinate

            // Rotate and copy pixel data from source to destination buffer
            rotate_copy_pixel(src, dst, x_start, y_start, x_end, y_end, LV_HOR_RES, LV_VER_RES, EXAMPLE_LVGL_PORT_ROTATION_DEGREE);
        }
    }
}


static void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data; // Get the panel handle from driver user data
    const int offsetx1 = area->x1; // Start X coordinate of the area to flush
    const int offsetx2 = area->x2; // End X coordinate of the area to flush
    const int offsety1 = area->y1; // Start Y coordinate of the area to flush
    const int offsety2 = area->y2; // End Y coordinate of the area to flush
    void *next_fb = NULL; // Pointer for the next frame buffer
    lv_port_flush_probe_t probe_result = FLUSH_PROBE_PART_COPY; // Default probe result
    lv_disp_t *disp = lv_disp_get_default(); // Get the default display

    /* Action after last area refresh */
    if (lv_disp_flush_is_last(drv)) {
        /* Check if the `full_refresh` flag has been triggered */
        if (drv->full_refresh) {
            /* Reset flag */
            drv->full_refresh = 0;

            // Rotate and copy data from the whole screen LVGL's buffer to the next frame buffer
            next_fb = flush_get_next_buf(panel_handle);
            rotate_copy_pixel((uint16_t *)color_map, next_fb, offsetx1, offsety1, offsetx2, offsety2, LV_HOR_RES, LV_VER_RES, EXAMPLE_LVGL_PORT_ROTATION_DEGREE);

            /* Switch the current RGB frame buffer to `next_fb` */
            esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, next_fb);

            /* Wait for the current frame buffer to complete transmission */
            ulTaskNotifyValueClear(NULL, ULONG_MAX);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            /* Synchronously update the dirty area for another frame buffer */
            flush_dirty_copy(flush_get_next_buf(panel_handle), color_map, &dirty_area);
            flush_get_next_buf(panel_handle);
        } else {
            /* Probe the copy method for the current dirty area */
            probe_result = flush_copy_probe(drv);

            if (probe_result == FLUSH_PROBE_FULL_COPY) {
                /* Save current dirty area for the next frame buffer */
                flush_dirty_save(&dirty_area);

                /* Set LVGL full-refresh flag and set flush ready in advance */
                drv->full_refresh = 1; // Indicate that a full refresh is required
                disp->rendering_in_progress = false; // Mark rendering as not in progress
                lv_disp_flush_ready(drv); // Mark flush as ready

                /* Force to refresh the whole screen, will invoke `flush_callback` recursively */
                lv_refr_now(_lv_refr_get_disp_refreshing());
            } else {
                /* Update current dirty area for the next frame buffer */
                next_fb = flush_get_next_buf(panel_handle);
                flush_dirty_save(&dirty_area);
                flush_dirty_copy(next_fb, color_map, &dirty_area);

                /* Switch the current RGB frame buffer to `next_fb` */
                esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, next_fb);

                /* Wait for the current frame buffer to complete transmission */
                ulTaskNotifyValueClear(NULL, ULONG_MAX);
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

                if (probe_result == FLUSH_PROBE_PART_COPY) {
                    /* Synchronously update the dirty area for another frame buffer */
                    flush_dirty_save(&dirty_area);
                    flush_dirty_copy(flush_get_next_buf(panel_handle), color_map, &dirty_area);
                    flush_get_next_buf(panel_handle);
                }
            }
        }
    }

    lv_disp_flush_ready(drv); // Mark the display flush as complete
}

#else

static void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data; // Get the panel handle from driver user data
    const int offsetx1 = area->x1; // Start X coordinate of the area to flush
    const int offsetx2 = area->x2; // End X coordinate of the area to flush
    const int offsety1 = area->y1; // Start Y coordinate of the area to flush
    const int offsety2 = area->y2; // End Y coordinate of the area to flush

    /* Action after last area refresh */
    if (lv_disp_flush_is_last(drv)) {
        /* Switch the current RGB frame buffer to `color_map` */
        esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);

        /* Wait for the last frame buffer to complete transmission */
        ulTaskNotifyValueClear(NULL, ULONG_MAX);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    lv_disp_flush_ready(drv); // Mark the display flush as complete
}
#endif /* EXAMPLE_LVGL_PORT_ROTATION_DEGREE */

#elif LVGL_PORT_FULL_REFRESH && LVGL_PORT_LCD_RGB_BUFFER_NUMS == 2

static void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data; // Get the panel handle from driver user data
    const int offsetx1 = area->x1; // Start X coordinate of the area to flush
    const int offsetx2 = area->x2; // End X coordinate of the area to flush
    const int offsety1 = area->y1; // Start Y coordinate of the area to flush
    const int offsety2 = area->y2; // End Y coordinate of the area to flush

    /* Switch the current RGB frame buffer to `color_map` */
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);

    /* Wait for the last frame buffer to complete transmission */
    ulTaskNotifyValueClear(NULL, ULONG_MAX);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    lv_disp_flush_ready(drv); // Mark the display flush as complete
}

#elif LVGL_PORT_FULL_REFRESH && LVGL_PORT_LCD_RGB_BUFFER_NUMS == 3

#if EXAMPLE_LVGL_PORT_ROTATION_DEGREE == 0
static void *lvgl_port_rgb_last_buf = NULL; // Pointer for the last RGB buffer
static void *lvgl_port_rgb_next_buf = NULL; // Pointer for the next RGB buffer
static void *lvgl_port_flush_next_buf = NULL; // Pointer for the flush next buffer
#endif

void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data; // Get the panel handle from driver user data
    const int offsetx1 = area->x1; // Start X coordinate of the area to flush
    const int offsetx2 = area->x2; // End X coordinate of the area to flush
    const int offsety1 = area->y1; // Start Y coordinate of the area to flush
    const int offsety2 = area->y2; // End Y coordinate of the area to flush

#if EXAMPLE_LVGL_PORT_ROTATION_DEGREE != 0
    void *next_fb = get_next_frame_buffer(panel_handle); // Get the next frame buffer

    /* Rotate and copy dirty area from the current LVGL's buffer to the next RGB frame buffer */
    rotate_copy_pixel((uint16_t *)color_map, next_fb, offsetx1, offsety1, offsetx2, offsety2, LV_HOR_RES, LV_VER_RES, EXAMPLE_LVGL_PORT_ROTATION_DEGREE);

    /* Switch the current RGB frame buffer to `next_fb` */
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, next_fb);
#else
    drv->draw_buf->buf1 = color_map; // Set buffer 1 to color_map
    drv->draw_buf->buf2 = lvgl_port_flush_next_buf; // Set buffer 2 to the next flush buffer
    lvgl_port_flush_next_buf = color_map; // Update the flush next buffer to color_map

    /* Switch the current RGB frame buffer to `color_map` */
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);

    lvgl_port_rgb_next_buf = color_map; // Update the next RGB buffer
#endif

    lv_disp_flush_ready(drv); // Mark the display flush as complete
}
#endif

#else

void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data; // Get the panel handle from driver user data
    const int offsetx1 = area->x1; // Start X coordinate of the area to flush
    const int offsetx2 = area->x2; // End X coordinate of the area to flush
    const int offsety1 = area->y1; // Start Y coordinate of the area to flush
    const int offsety2 = area->y2; // End Y coordinate of the area to flush

    /* Just copy data from the color map to the RGB frame buffer */
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);

    lv_disp_flush_ready(drv); // Mark the display flush as complete
}

#endif /* LVGL_PORT_AVOID_TEAR_ENABLE */

static lv_disp_t *display_init(esp_lcd_panel_handle_t panel_handle)
{
    assert(panel_handle); // Ensure the panel handle is valid

    static lv_disp_draw_buf_t disp_buf = { 0 };     // Contains internal graphic buffer(s) called draw buffer(s)
    static lv_disp_drv_t disp_drv = { 0 };          // Contains LCD panel handle and callback functions

    // Allocate draw buffers used by LVGL
    void *buf1 = NULL; // Pointer for the first buffer
    void *buf2 = NULL; // Pointer for the second buffer
    int buffer_size = 0; // Size of the buffer

    ESP_LOGD(TAG, "Malloc memory for LVGL buffer");
#if LVGL_PORT_AVOID_TEAR_ENABLE
    // To avoid tearing effect, at least two frame buffers are needed: one for LVGL rendering and another for RGB output
    buffer_size = LVGL_PORT_H_RES * LVGL_PORT_V_RES;
#if (LVGL_PORT_LCD_RGB_BUFFER_NUMS == 3) && (EXAMPLE_LVGL_PORT_ROTATION_DEGREE == 0) && LVGL_PORT_FULL_REFRESH
    // With three buffers and full-refresh, one buffer is always available for rendering
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 3, &lvgl_port_rgb_last_buf, &buf1, &buf2));
    lvgl_port_rgb_next_buf = lvgl_port_rgb_last_buf; // Set the next RGB buffer
    lvgl_port_flush_next_buf = buf2; // Set the flush next buffer
#elif (LVGL_PORT_LCD_RGB_BUFFER_NUMS == 3) && (EXAMPLE_LVGL_PORT_ROTATION_DEGREE != 0)
    // Using three frame buffers, one for LVGL rendering and two for RGB driver (one used for rotation)
    void *fbs[3];
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 3, &fbs[0], &fbs[1], &fbs[2]));
    buf1 = fbs[2]; // Set buf1 to the third frame buffer
#else
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &buf1, &buf2)); // Get two frame buffers
#endif
#else
    // Normally, for RGB LCD, just one buffer is used for LVGL rendering
    buffer_size = LVGL_PORT_H_RES * LVGL_PORT_BUFFER_HEIGHT; // Calculate buffer size
    buf1 = heap_caps_malloc(buffer_size * sizeof(lv_color_t), LVGL_PORT_BUFFER_MALLOC_CAPS); // Allocate memory
    assert(buf1); // Ensure allocation succeeded
    ESP_LOGI(TAG, "LVGL buffer size: %dKB", buffer_size * sizeof(lv_color_t) / 1024); // Log buffer size
#endif /* LVGL_PORT_AVOID_TEAR_ENABLE */

    // Initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buffer_size); // Initialize the draw buffer

    ESP_LOGD(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv); // Initialize the display driver
#if EXAMPLE_LVGL_PORT_ROTATION_90 || EXAMPLE_LVGL_PORT_ROTATION_270
    disp_drv.hor_res = LVGL_PORT_V_RES; // Set horizontal resolution for rotation
    disp_drv.ver_res = LVGL_PORT_H_RES; // Set vertical resolution for rotation
#else
    disp_drv.hor_res = LVGL_PORT_H_RES; // Set horizontal resolution
    disp_drv.ver_res = LVGL_PORT_V_RES; // Set vertical resolution
#endif
    disp_drv.flush_cb = flush_callback; // Set the flush callback
    disp_drv.draw_buf = &disp_buf; // Set the draw buffer
    disp_drv.user_data = panel_handle; // Set user data to panel handle
#if LVGL_PORT_FULL_REFRESH
    disp_drv.full_refresh = 1; // Enable full refresh
#elif LVGL_PORT_DIRECT_MODE
    disp_drv.direct_mode = 1; // Enable direct mode
#endif
    return lv_disp_drv_register(&disp_drv); // Register the display driver
}

static void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)indev_drv->user_data; // Get touchpad handle from user data
    assert(tp); // Ensure touchpad handle is valid

    uint16_t touchpad_x; // Variable for X coordinate
    uint16_t touchpad_y; // Variable for Y coordinate
    uint8_t touchpad_cnt = 0; // Variable for touch count

    /* Read data from touch controller into memory */
    esp_lcd_touch_read_data(tp); // Read data from touch controller

    /* Read data from touch controller */
    bool touchpad_pressed = esp_lcd_touch_get_coordinates(tp, &touchpad_x, &touchpad_y, NULL, &touchpad_cnt, 1); // Get touch coordinates
    if (touchpad_pressed && touchpad_cnt > 0) {
        data->point.x = touchpad_x; // Set the X coordinate
        data->point.y = touchpad_y; // Set the Y coordinate
        data->state = LV_INDEV_STATE_PRESSED; // Set state to pressed
        ESP_LOGD(TAG, "Touch position: %d,%d", touchpad_x, touchpad_y); // Log touch position
    } else {
        data->state = LV_INDEV_STATE_RELEASED; // Set state to released
    }
}

static lv_indev_t *indev_init(esp_lcd_touch_handle_t tp)
{
    assert(tp); // Ensure the touch panel handle is valid

    static lv_indev_drv_t indev_drv_tp; // Static input device driver

    /* Register a touchpad input device */
    lv_indev_drv_init(&indev_drv_tp); // Initialize the input device driver
    indev_drv_tp.type = LV_INDEV_TYPE_POINTER; // Set the device type to pointer (touchpad)
    indev_drv_tp.read_cb = touchpad_read; // Set the read callback function
    indev_drv_tp.user_data = tp; // Set user data to the touch panel handle

    return lv_indev_drv_register(&indev_drv_tp); // Register the input device driver
}

static void tick_increment(void *arg)
{
    /* Tell LVGL how many milliseconds have elapsed */
    lv_tick_inc(LVGL_PORT_TICK_PERIOD_MS); // Increment the LVGL tick count
}

static esp_err_t tick_init(void)
{
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &tick_increment, // Set the callback function for the timer
        .name = "LVGL tick" // Name of the timer
    };
    esp_timer_handle_t lvgl_tick_timer = NULL; // Timer handle
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer)); // Create the timer
    return esp_timer_start_periodic(lvgl_tick_timer, LVGL_PORT_TICK_PERIOD_MS * 1000); // Start the timer
}

static void lvgl_port_task(void *arg)
{
    ESP_LOGD(TAG, "Starting LVGL task"); // Log the task start

    uint32_t task_delay_ms = LVGL_PORT_TASK_MAX_DELAY_MS; // Set initial task delay
    while (1) {
        if (lvgl_port_lock(-1)) { // Try to lock the LVGL mutex
            task_delay_ms = lv_timer_handler(); // Handle LVGL timer events
            lvgl_port_unlock(); // Unlock the mutex
        }
        // Ensure the delay time is within limits
        if (task_delay_ms > LVGL_PORT_TASK_MAX_DELAY_MS) {
            task_delay_ms = LVGL_PORT_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < LVGL_PORT_TASK_MIN_DELAY_MS) {
            task_delay_ms = LVGL_PORT_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms)); // Delay the task for the calculated time
    }
}

esp_err_t lvgl_port_init(esp_lcd_panel_handle_t lcd_handle, esp_lcd_touch_handle_t tp_handle)
{
    lv_init(); // Initialize LVGL
    ESP_ERROR_CHECK(tick_init()); // Initialize the tick timer

    lv_disp_t *disp = display_init(lcd_handle); // Initialize the display
    assert(disp); // Ensure the display initialization was successful

    if (tp_handle) {
        lv_indev_t *indev = indev_init(tp_handle); // Initialize the touchpad input device
        assert(indev); // Ensure the input device initialization was successful

        // Set touch panel orientation based on rotation
#if EXAMPLE_LVGL_PORT_ROTATION_90
        esp_lcd_touch_set_swap_xy(tp_handle, true); // Swap X and Y coordinates
        esp_lcd_touch_set_mirror_y(tp_handle, true); // Mirror Y coordinates
#elif EXAMPLE_LVGL_PORT_ROTATION_180
        esp_lcd_touch_set_mirror_x(tp_handle, true); // Mirror X coordinates
        esp_lcd_touch_set_mirror_y(tp_handle, true); // Mirror Y coordinates
#elif EXAMPLE_LVGL_PORT_ROTATION_270
        esp_lcd_touch_set_swap_xy(tp_handle, true); // Swap X and Y coordinates
        esp_lcd_touch_set_mirror_x(tp_handle, true); // Mirror X coordinates
#endif
    }

    lvgl_mux = xSemaphoreCreateRecursiveMutex(); // Create a recursive mutex for LVGL
    assert(lvgl_mux); // Ensure mutex creation was successful

    ESP_LOGI(TAG, "Create LVGL task"); // Log task creation
    BaseType_t core_id = (LVGL_PORT_TASK_CORE < 0) ? tskNO_AFFINITY : LVGL_PORT_TASK_CORE; // Determine core ID for the task
    BaseType_t ret = xTaskCreatePinnedToCore(lvgl_port_task, "lvgl", LVGL_PORT_TASK_STACK_SIZE, NULL,
                                             LVGL_PORT_TASK_PRIORITY, &lvgl_task_handle, core_id); // Create the LVGL task
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LVGL task"); // Log error if task creation fails
        return ESP_FAIL; // Return failure
    }

    return ESP_OK; // Return success
}

bool lvgl_port_lock(int timeout_ms)
{
    assert(lvgl_mux && "lvgl_port_init must be called first"); // Ensure the mutex is initialized

    const TickType_t timeout_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms); // Convert timeout to ticks
    return xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks) == pdTRUE; // Try to take the mutex
}

void lvgl_port_unlock(void)
{
    assert(lvgl_mux && "lvgl_port_init must be called first"); // Ensure the mutex is initialized
    xSemaphoreGiveRecursive(lvgl_mux); // Release the mutex
}

bool lvgl_port_notify_rgb_vsync(void)
{
    BaseType_t need_yield = pdFALSE; // Flag to check if a yield is needed
#if LVGL_PORT_FULL_REFRESH && (LVGL_PORT_LCD_RGB_BUFFER_NUMS == 3) && (EXAMPLE_LVGL_PORT_ROTATION_DEGREE == 0)
    if (lvgl_port_rgb_next_buf != lvgl_port_rgb_last_buf) {
        lvgl_port_flush_next_buf = lvgl_port_rgb_last_buf; // Set next buffer for flushing
        lvgl_port_rgb_last_buf = lvgl_port_rgb_next_buf; // Update the last buffer
    }
#elif LVGL_PORT_AVOID_TEAR_ENABLE
    // Notify that the current RGB frame buffer has been transmitted
    xTaskNotifyFromISR(lvgl_task_handle, ULONG_MAX, eNoAction, &need_yield); // Notify the LVGL task
#endif
    return (need_yield == pdTRUE); // Return whether a yield is needed
}
