#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "scd4x.h"

#include "bus/dgx_spi_esp32.h"
#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_gauge.h"
#include "drivers/gc9a01.h"
#include "drivers/vscreen.h"

#include "app_fonts.h"
#include "app_mqtt.h"
#include "app_settings.h"
#include "app_webserver.h"
#include "provisioning.h"
#include "tzones.h"

/*

| Устройство | Вывод устройства | GPIO | Функция в ESP-IDF | Особые примечания |
| :--- | :--- | :--- | :--- | :--- |
| **TTP223** | OUT | **GPIO0** | `GPIO_MODE_INPUT` | `GPIO_PULLUP_DISABLE`, `GPIO_PULLDOWN_DISABLE`. TTP223 idles LOW,
поэтому уровень при boot стабилен. | | **SCD41** | SDA | **GPIO3** | `I2C_MODE_MASTER` | Внутренний pull-up включить
(`GPIO_PULLUP_ENABLE`) | | | SCL | **GPIO5** | `I2C_MODE_MASTER` | Аналогично. | | **GC9A01** | CLK | **GPIO4** |
`SPI2_HOST` (FSPICLK) | Аппаратный SPI. | | | MOSI | **GPIO6** | `SPI2_HOST` (FSPID) | Аппаратный SPI. MISO = `-1`. | |
| CS | **GPIO7** | `SPI2_HOST` (FSPICS0) | Аппаратный SPI. | | | DC | **GPIO1** | `GPIO_MODE_OUTPUT` | Обычный GPIO. DC
управляется MCU только после загрузки; состояние при старте не влияет на работу дисплея (CS не активен). | | | RST |
**GPIO2** | `GPIO_MODE_OUTPUT` | Strapping-пин (должен быть HIGH при boot). |

*/

#define MIN3(a, b, c) DGX_MIN(DGX_MIN((a), (b)), (c))
#define MAX3(a, b, c) DGX_MAX(DGX_MAX((a), (b)), (c))

#define BG_COLOR DGX_NAVY(DGX_RGB_16)

/* Define DEBUG_APP_TOUCH (here or as a build flag) to trace touch decisions on the
 * serial monitor. Useful to confirm whether double-tap is even allowed (it is gated
 * behind Wi-Fi provisioning) and which branch a gesture takes. */
#define DEBUG_APP_TOUCH
#if defined(DEBUG_APP_TOUCH)
#define TOUCH_LOGD(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define TOUCH_LOGD(...) ((void)0)
#endif

enum
{
    DISPLAY_MOSI           = GPIO_NUM_6,
    DISPLAY_SCLK           = GPIO_NUM_4,
    DISPLAY_CS             = GPIO_NUM_7,
    DISPLAY_DC             = GPIO_NUM_1,
    DISPLAY_RST            = GPIO_NUM_2,
    DISPLAY_DMA            = SPI_DMA_CH_AUTO,
    DISPLAY_CLOCK_SPEED_HZ = 40 * 1000 * 1000,
    DISPLAY_GAUGE_WIDTH    = 15,

    SCD41_I2C_SDA     = GPIO_NUM_3,
    SCD41_I2C_SCL     = GPIO_NUM_5,
    SCD41_I2C_PORT    = I2C_NUM_0,
    SCD41_I2C_FREQ_HZ = 100000,

    TOUCH_GPIO              = GPIO_NUM_0,
    TOUCH_ACTIVE_LEVEL      = 1,
    TOUCH_DOUBLE_TAP_US     = 450000,
    TOUCH_PROVISION_HOLD_US = 5000000,
    PROVISIONING_TIMEOUT_MS = 60000,

    VALUE_DIGIT_COUNT       = 4,
    VALUE_DIGIT_SCALE       = 4,
    VALUE_UNIT_GAP          = 4,
    VALUE_MORPH_DURATION_US = 350000,
    VALUE_MORPH_FRAME_US    = 33333,
    TEXT_CLEAR_PADDING      = 2,

    /* Gauge sweeps over this range; readings above GAUGE_MAX_PPM saturate the
     * arc but the numeric readout still shows the real value (SCD41 reads well
     * past 2000 ppm). */
    GAUGE_MIN_PPM = 0,
    GAUGE_MAX_PPM = 2000,

    /* Upper bound for the 4-digit numeric readout. */
    SENSOR_CO2_MAX_PPM = 9999,

    /* Any wall-clock value before 2024-01-01 is treated as "not synced yet". */
    TIME_SYNC_VALID_UNIX_TIME = 1704067200,
};

enum
{
    PALETTE_TRANSPARENT = 0,
    PALETTE_FOREGROUND  = 1,
    PALETTE_BACKGROUND  = 2,
};

static const char *TAG = "CO2Station";

typedef enum
{
    TouchActionNone,
    TouchActionToggleDisplay,
    TouchActionStartWebserver,
    TouchActionStartProvisioning,
} touch_action_t;

typedef enum
{
    SensorFault,
    SensorStarting,
    SensorReady,
} sensor_status_t;

typedef struct
{
    sensor_status_t status;
    uint16_t        co2_ppm;
    int16_t         temperature_deci_c;
    uint16_t        humidity_deci_rh;
} air_state_t;

typedef struct
{
    int x, y, w, h;
} rect_t;

typedef struct
{
    dgx_screen_t *scratch;
    dgx_font_t   *font;
    int           scale;
    int           x, y, w, h;
    int           baseline_offset;
} text_slot_t;

typedef struct
{
    dgx_screen_t            *scratch;
    dgx_screen_t            *point;
    dgx_font_t              *digit_font;
    dgx_font_t              *fallback_font;
    dgx_font_t              *unit_font;
    dgx_font_symbol_morph_t *morphs[VALUE_DIGIT_COUNT];
    uint16_t                 palette[256];
    char                     digits[VALUE_DIGIT_COUNT + 1];
    int                      x, y, w, h;
    int                      digit_field_x;
    int                      digit_field_width;
    int                      digit_baseline_offset;
    int                      fallback_baseline_offset;
    int                      unit_baseline_offset;
    int                      unit_width;
    int                      digit_scale;
    uint32_t                 text_color;
    uint32_t                 unit_color;
    uint32_t                 background_color;
    uint32_t                 palette_text_color;
    int64_t                  animation_start_us;
    int64_t                  last_frame_us;
    bool                     animation_active;
    bool                     animation_heap_logged_during;
    bool                     palette_text_color_valid;
    bool                     showing_digits;
} value_display_t;

typedef struct
{
    const char *visible_digits;
    int         text_x;
    int         unit_x;
    int         positions[VALUE_DIGIT_COUNT];
} value_layout_t;

typedef struct
{
    value_display_t value;
    text_slot_t     status;
    text_slot_t     measurement;
    text_slot_t     time;
    text_slot_t     web_address;
} dynamic_screen_t;

typedef struct
{
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    scd4x_t                *sensor;
    QueueHandle_t           queue;
    float                   applied_temperature_offset;
} scd41_runtime_t;

/* ---------- Air quality classification ---------- */

/* Called by local formatting and gauge helpers whenever a value must be clamped to a known range. */
static int clamp_int(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/* Called before gauge color/value updates so the gauge never exceeds its configured sweep. */
static int clamp_gauge_ppm(int ppm)
{
    return clamp_int(ppm, GAUGE_MIN_PPM, GAUGE_MAX_PPM);
}

/* Passed to dgx_gauge_init(); the gauge driver calls it whenever it redraws a colored arc segment. */
static uint32_t gauge_color(int ppm)
{
    ppm     = clamp_gauge_ppm(ppm);
    int idx = (ppm - GAUGE_MIN_PPM) * 255 / (GAUGE_MAX_PPM - GAUGE_MIN_PPM);
    return DGX_RGB_16(idx, 255 - idx, 0);
}

/* Called from draw_dynamic_screen() in SensorReady state to color the quality caption. */
static uint32_t quality_text_color(int ppm)
{
    if (ppm <= 800) return DGX_GREEN(DGX_RGB_16);
    if (ppm <= 1200) return DGX_YELLOW(DGX_RGB_16);
    if (ppm <= 1600) return DGX_ORANGE(DGX_RGB_16);
    return DGX_RED(DGX_RGB_16);
}

/* Called from draw_dynamic_screen() in SensorReady state to pick the air-quality caption. */
static const char *quality_label(int ppm)
{
    if (ppm <= 600) return "excellent";
    if (ppm <= 800) return "good";
    if (ppm <= 1200) return "fair";
    if (ppm <= 1600) return "poor";
    return "bad";
}

/* ---------- Geometry & color helpers ---------- */

/* Called during layout setup whenever text bounds are needed to center or reserve screen space. */
static rect_t text_box(const char *text, dgx_font_t *font, int x, int y_baseline, int scale)
{
    int16_t ycorner, height;
    int     width = dgx_font_string_bounds(text, font, &ycorner, &height);
    return (rect_t){
        .x = x,
        .y = y_baseline + ycorner * scale,
        .w = width * scale,
        .h = height * scale,
    };
}

/* Called by draw_static_screen() to place the title block inside the gauge opening. */
static int text_top_in_circle(int screen_height, int inner_radius, int text_width, int padding)
{
    int half = (text_width + 1) / 2;
    int sq   = inner_radius * inner_radius - half * half;
    if (sq <= 0) {
        return padding;
    }
    return screen_height / 2 - (int)floorf(sqrtf((float)sq)) + padding;
}

/* Called by value-layout code whenever the shared CO2 digit baseline must be recomputed. */
static int digits_baseline(const dgx_screen_t *screen)
{
    return screen->height / 2 - app_font_dashboard_value_unit()->yAdvance;
}

/* Called once per animated morph dot to interpolate between its start and end positions. */
static dgx_point_2d_t linear_point_move(float t, dgx_point_2d_t a, dgx_point_2d_t b)
{
    return (dgx_point_2d_t){
        .x = a.x + (int)lroundf(t * (float)(b.x - a.x)),
        .y = a.y + (int)lroundf(t * (float)(b.y - a.y)),
    };
}

/* Called before blitting 8-bit scratch buffers so glyph intensities map to the current 16-bit text color. */
static void build_color_lut(uint16_t *lut, uint32_t color)
{
    uint8_t r = DGX_R_FROM_16(color);
    uint8_t g = DGX_G_FROM_16(color);
    uint8_t b = DGX_B_FROM_16(color);
    for (int i = 0; i < 256; ++i) {
        lut[i] = dgx_rgb_to_16((uint8_t)(i * r / 255u), (uint8_t)(i * g / 255u), (uint8_t)(i * b / 255u));
    }
}

/* Passed into DGX font rendering so each scaled digit dot lands in the value scratch buffer. */
static void draw_dot_to_vscreen(dgx_screen_t *dst, int x, int y, dgx_font_sym8_params_t *params)
{
    dgx_vscreen_to_vscreen(dst, x, y, params->vpoint, true);
}

#if defined(DEBUG_APP_HEAP)
/* Called at morph start/during/end when DEBUG_APP_HEAP is enabled to snapshot heap usage. */
static void log_value_morph_heap(const char *phase)
{
    ESP_LOGI(TAG, "Value morph %s: free heap=%u bytes, min free heap=%u bytes", phase,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}

/* Called after morph structs are rebuilt when DEBUG_APP_HEAP is enabled to report allocation delta. */
static void log_value_morph_heap_setup(uint32_t heap_before_destroy, uint32_t heap_after_destroy,
                                       uint32_t heap_after_create)
{
    int reclaimed = (int)heap_after_destroy - (int)heap_before_destroy;
    int allocated = (int)heap_after_destroy - (int)heap_after_create;

    ESP_LOGI(TAG,
             "Value morph setup: before=%u bytes, after destroy=%u bytes, after create=%u bytes, reclaimed=%d bytes, "
             "allocated=%d bytes",
             (unsigned)heap_before_destroy, (unsigned)heap_after_destroy, (unsigned)heap_after_create, reclaimed,
             allocated);
}
#else
#define log_value_morph_heap(phase)                                                            ((void)0)
#define log_value_morph_heap_setup(heap_before_destroy, heap_after_destroy, heap_after_create) ((void)0)
#endif

/* ---------- Big value display (4 digits + "ppm", with morph animation) ---------- */

/* Called before drawing or morphing digits to center the visible number and position the ppm suffix. */
static void value_layout_compute(const value_display_t *v, const char *digits, value_layout_t *out)
{
    out->visible_digits = digits;
    while (*out->visible_digits == ' ' && out->visible_digits[1] != '\0') {
        ++out->visible_digits;
    }

    int digit_w = dgx_font_string_bounds(out->visible_digits, v->digit_font, NULL, NULL) * v->digit_scale;
    int cursor  = v->digit_field_x + (v->digit_field_width - digit_w) / 2;
    out->text_x = cursor;

    bool started = false;
    for (int i = 0; i < VALUE_DIGIT_COUNT; ++i) {
        out->positions[i] = cursor;
        if (!started && digits[i] == ' ') {
            continue;
        }
        started = true;
        int16_t advance;
        dgx_font_find_glyph((uint8_t)digits[i], v->digit_font, &advance);
        cursor += advance * v->digit_scale;
    }

    out->unit_x = cursor + VALUE_UNIT_GAP;
}

/* Called before status changes or new digit transitions to release any previous morph structs. */
static void value_destroy_morphs(value_display_t *v)
{
    for (int i = 0; i < VALUE_DIGIT_COUNT; ++i) {
        if (v->morphs[i] != NULL) {
            dgx_font_make_morph_struct_destroy(&v->morphs[i]);
        }
    }
}

/* Called before any animated value rendering to verify the scratch buffers were allocated at startup. */
static bool value_has_vscreens(const value_display_t *v)
{
    return v->scratch != NULL && v->point != NULL;
}

/* Called once from dynamic_screen_init() after the screen is ready to allocate value-rendering buffers and layout. */
static void value_init(value_display_t *v, dgx_screen_t *screen)
{
    memset(v, 0, sizeof(*v));

    int value_baseline = digits_baseline(screen);
    v->digit_font      = app_font_dashboard_value_digits();
    v->fallback_font   = app_font_dashboard_value_fallback();
    v->unit_font       = app_font_dashboard_value_unit();
    v->digit_scale     = VALUE_DIGIT_SCALE;

    rect_t digits   = text_box("8888", v->digit_font, 0, value_baseline, v->digit_scale);
    rect_t fallback = text_box("sensorig", v->fallback_font, 0, value_baseline, 1);
    rect_t unit     = text_box("ppm", v->unit_font, 0, 0, 1);

    int digit_left    = screen->width / 2 - digits.w / 2;
    int fallback_left = screen->width / 2 - fallback.w / 2;
    int unit_top      = digits.y + digits.h - unit.h - TEXT_CLEAR_PADDING;
    int unit_baseline = unit_top - unit.y;

    int top   = MIN3(digits.y, fallback.y, unit_top);
    int bot   = MAX3(digits.y + digits.h, fallback.y + fallback.h, unit_top + unit.h);
    int left  = DGX_MIN(digit_left, fallback_left);
    int right = DGX_MAX(digit_left + digits.w + VALUE_UNIT_GAP + unit.w, fallback_left + fallback.w);

    v->x = left;
    v->y = top;
    v->w = right - left;
    v->h = bot - top;

    v->digit_field_x            = digit_left - v->x;
    v->digit_field_width        = digits.w;
    v->digit_baseline_offset    = value_baseline - v->y;
    v->fallback_baseline_offset = value_baseline - v->y;
    v->unit_baseline_offset     = unit_baseline - v->y;
    v->unit_width               = unit.w;

    v->scratch = (v->w > 0 && v->h > 0) ? dgx_vscreen_init(v->w, v->h, 8, DgxScreenRGB) : NULL;
    v->point   = dgx_vscreen_init(v->digit_scale, v->digit_scale, 8, DgxScreenRGB);
    if (v->point != NULL) {
        dgx_font_make_point8(v->point);
    }
    if (!value_has_vscreens(v)) {
        ESP_LOGW(TAG, "Value display requires virtual screens; value rendering disabled");
    }

    memset(v->digits, ' ', VALUE_DIGIT_COUNT);
    v->digits[VALUE_DIGIT_COUNT] = '\0';
}

/* Called from the value drawing paths to stamp the ppm suffix into the value scratch buffer. */
static void value_draw_unit(value_display_t *v, const value_layout_t *layout)
{
    dgx_font_string_utf8_screen(v->scratch, layout->unit_x, v->unit_baseline_offset, "ppm", PALETTE_FOREGROUND,
                                DgxOutputNormal, 1, v->unit_font, NULL, NULL);
}

/* Called before digit/morph blits to rebuild the grayscale-to-color LUT only when the digit color changed. */
static void value_prepare_palette(value_display_t *v)
{
    if (!v->palette_text_color_valid || v->palette_text_color != v->text_color) {
        build_color_lut(v->palette, v->text_color);
        v->palette_text_color       = v->text_color;
        v->palette_text_color_valid = true;
    }

    v->palette[0]                  = (uint16_t)v->background_color;
    v->palette[PALETTE_FOREGROUND] = (uint16_t)v->unit_color;
}

/* Called when a morph finishes to render the settled digits and unit in one scratch-buffer pass. */
static void value_draw_digits(dgx_screen_t *screen, value_display_t *v)
{
    value_layout_t layout;
    value_layout_compute(v, v->digits, &layout);

    if (!value_has_vscreens(v)) {
        return;
    }

    dgx_font_sym8_params_t params = {
        .vpoint   = v->point,
        .lut      = NULL,
        .dot_func = draw_dot_to_vscreen,
    };

    value_prepare_palette(v);

    dgx_fill_rectangle(v->scratch, 0, 0, v->w, v->h, 0);
    dgx_font_string_utf8_screen(v->scratch, layout.text_x, v->digit_baseline_offset, layout.visible_digits, 0,
                                DgxOutputNormal, v->digit_scale, v->digit_font, NULL, &params);
    value_draw_unit(v, &layout);
    dgx_vscreen8_to_screen16(screen, v->x, v->y, v->scratch, v->palette, false);
}

/* Called at morph start and on each animation frame to render the in-between digit state. */
static void value_draw_morph(dgx_screen_t *screen, value_display_t *v, float t)
{
    if (!value_has_vscreens(v)) {
        return;
    }

    value_layout_t layout;
    value_layout_compute(v, v->digits, &layout);

    value_prepare_palette(v);

    dgx_fill_rectangle(v->scratch, 0, 0, v->w, v->h, 0);
    for (int i = 0; i < VALUE_DIGIT_COUNT; ++i) {
        dgx_font_symbol_morph_t *m = v->morphs[i];
        if (m == NULL) {
            continue;
        }
        if ((t == 0.0f && m->is_from_empty) || (t == 1.0f && m->is_to_empty)) {
            continue;
        }
        for (int d = 0; d < m->number_of_dots; ++d) {
            dgx_point_2d_t p = linear_point_move(t, m->m_start[d], m->m_end[d]);
            dgx_vscreen_to_vscreen(v->scratch, p.x, p.y, v->point, true);
        }
    }
    value_draw_unit(v, &layout);
    dgx_vscreen8_to_screen16(screen, v->x, v->y, v->scratch, v->palette, false);
}

/* Called from draw_dynamic_screen() when the sensor is not ready to render a non-numeric status word. */
static void value_show_status(dgx_screen_t *screen, value_display_t *v, const char *text, uint32_t text_color)
{
    value_destroy_morphs(v);
    v->animation_active = false;
    v->showing_digits   = false;
    v->text_color       = text_color;
    v->background_color = BG_COLOR;
    memset(v->digits, ' ', VALUE_DIGIT_COUNT);
    v->digits[VALUE_DIGIT_COUNT] = '\0';

    int text_w = dgx_font_string_bounds(text, v->fallback_font, NULL, NULL);
    int text_x = v->digit_field_x + v->digit_field_width / 2 - text_w / 2;

    if (v->scratch == NULL) {
        return;
    }

    uint16_t palette[256]       = {0};
    palette[PALETTE_FOREGROUND] = (uint16_t)text_color;
    palette[PALETTE_BACKGROUND] = (uint16_t)BG_COLOR;

    dgx_fill_rectangle(v->scratch, 0, 0, v->w, v->h, PALETTE_BACKGROUND);
    dgx_font_string_utf8_screen(v->scratch, text_x, v->fallback_baseline_offset, text, PALETTE_FOREGROUND,
                                DgxOutputNormal, 1, v->fallback_font, NULL, NULL);
    dgx_vscreen8_to_screen16(screen, v->x, v->y, v->scratch, palette, false);
}

/* Called from draw_dynamic_screen() in SensorReady state to arm a new morph when the CO2 digits change. */
static void value_show_digits(dgx_screen_t *screen, value_display_t *v, const char *digits, uint32_t text_color,
                              uint32_t unit_color)
{
    if (v->showing_digits && memcmp(v->digits, digits, VALUE_DIGIT_COUNT) == 0) {
        return;
    }

    v->text_color       = text_color;
    v->unit_color       = unit_color;
    v->background_color = BG_COLOR;

    if (!value_has_vscreens(v)) {
        return;
    }

    value_layout_t from_layout, to_layout;
    value_layout_compute(v, v->digits, &from_layout);
    value_layout_compute(v, digits, &to_layout);

#if defined(DEBUG_APP_HEAP)
    uint32_t heap_before_destroy = (uint32_t)esp_get_free_heap_size();
#endif
    value_destroy_morphs(v);
#if defined(DEBUG_APP_HEAP)
    uint32_t heap_after_destroy = (uint32_t)esp_get_free_heap_size();
#endif
    for (int i = 0; i < VALUE_DIGIT_COUNT; ++i) {
        uint32_t from_c = v->showing_digits ? (uint8_t)v->digits[i] : ' ';
        uint32_t to_c   = (uint8_t)digits[i];
        int      from_x = from_layout.positions[i];
        int      to_x   = to_layout.positions[i];

        if (!v->showing_digits || from_c == ' ') {
            from_x = to_x;
        }

        v->morphs[i] = dgx_font_make_morph_struct( //
            v->digit_font,                         //
            from_c,                                //
            to_c,                                  //
            from_x,                                //
            v->digit_baseline_offset,              //
            to_x,                                  //
            v->digit_baseline_offset,              //
            v->digit_scale                         //
        );
    }
#if defined(DEBUG_APP_HEAP)
    log_value_morph_heap_setup(heap_before_destroy, heap_after_destroy, (uint32_t)esp_get_free_heap_size());
#endif

    memcpy(v->digits, digits, VALUE_DIGIT_COUNT + 1);
    v->showing_digits               = true;
    v->animation_active             = true;
    v->animation_heap_logged_during = false;
    v->animation_start_us           = esp_timer_get_time();
    v->last_frame_us                = 0;

    log_value_morph_heap("start");
    value_draw_morph(screen, v, 0.0f);
}

/* Called once per main-loop tick while the display is active to advance any pending digit morph. */
static bool value_step_animation(dgx_screen_t *screen, value_display_t *v)
{
    if (!v->animation_active) {
        return false;
    }

    int64_t now = esp_timer_get_time();
    if (v->last_frame_us != 0 && now - v->last_frame_us < VALUE_MORPH_FRAME_US) {
        return false;
    }

    float t = (float)(now - v->animation_start_us) / (float)VALUE_MORPH_DURATION_US;
    if (t >= 1.0f) {
        v->animation_active = false;
        value_draw_digits(screen, v);
        log_value_morph_heap("end");
        return true;
    }

    v->last_frame_us = now;
    value_draw_morph(screen, v, t);
    if (!v->animation_heap_logged_during) {
        v->animation_heap_logged_during = true;
        log_value_morph_heap("during");
    }
    return true;
}

/* ---------- Small centered text rows ---------- */

/* Called once per dashboard text row during screen setup to allocate a centered scratch slot. */
static void text_slot_init(text_slot_t *s, dgx_screen_t *screen, dgx_font_t *font, const char *probe, int y_baseline,
                           int scale)
{
    rect_t r           = text_box(probe, font, 0, y_baseline, scale);
    s->font            = font;
    s->x               = screen->width / 2 - r.w / 2;
    s->y               = r.y;
    s->w               = r.w;
    s->h               = r.h;
    s->baseline_offset = y_baseline - r.y;
    s->scratch         = (r.w > 0 && r.h > 0) ? dgx_vscreen_init(r.w, r.h, 8, DgxScreenRGB) : NULL;
    s->scale           = scale;
}

/* Called during each dynamic redraw to update one centered text row. */
static void text_slot_draw(dgx_screen_t *screen, const text_slot_t *s, const char *text, uint32_t color)
{
    if (s->w <= 0 || s->h <= 0) {
        return;
    }

    int text_w = dgx_font_string_bounds(text, s->font, NULL, NULL) * s->scale;

    if (s->scratch == NULL) {
        dgx_fill_rectangle(screen, s->x, s->y, s->w, s->h, BG_COLOR);
        dgx_font_string_utf8_screen(screen, s->x + (s->w - text_w) / 2, s->y + s->baseline_offset, text, color,
                                    DgxOutputNormal, s->scale, s->font, NULL, NULL);
        return;
    }

    uint16_t palette[256]       = {0};
    palette[PALETTE_FOREGROUND] = (uint16_t)color;
    palette[PALETTE_BACKGROUND] = (uint16_t)BG_COLOR;

    dgx_fill_rectangle(s->scratch, 0, 0, s->w, s->h, PALETTE_BACKGROUND);
    dgx_font_string_utf8_screen(s->scratch, (s->w - text_w) / 2, s->baseline_offset, text, PALETTE_FOREGROUND,
                                DgxOutputNormal, s->scale, s->font, NULL, NULL);
    dgx_vscreen8_to_screen16(screen, s->x, s->y, s->scratch, palette, false);
}

/* ---------- Whole screen ---------- */

/* Called once from app_main() after the panel is initialized to size all dynamic dashboard elements. */
static void dynamic_screen_init(dynamic_screen_t *ds, dgx_screen_t *screen)
{
    dgx_font_t *label_font = app_font_dashboard_text();
    dgx_font_t *time_font  = app_font_dashboard_time();

    int value_y  = digits_baseline(screen);
    int status_y = value_y + label_font->yAdvance + label_font->yAdvance / 2;
    int meas_y   = status_y + time_font->yAdvance * 2 + label_font->yAdvance / 2 - 4;
    int time_y   = meas_y + time_font->yAdvance * 2 + label_font->yAdvance / 2 - 4;
    int web_y    = time_y + label_font->yAdvance + label_font->yAdvance / 2;
    value_init(&ds->value, screen);
    text_slot_init(&ds->status, screen, label_font, "Warming yg", status_y, 1);
    text_slot_init(&ds->measurement, screen, time_font, "-000.0℃ 000.0%", meas_y, 2);
    text_slot_init(&ds->time, screen, time_font, "00:00", time_y, 2);
    text_slot_init(&ds->web_address, screen, label_font, "255.255.255.255", web_y, 1);
}

/* Called whenever the full dashboard background/title needs repainting without sensor-specific text. */
static void draw_static_screen(dgx_screen_t *screen)
{
    dgx_font_t *title_font         = app_font_dashboard_title();
    dgx_font_t *label_font         = app_font_dashboard_subtitle();
    int         gauge_inner_radius = screen->width / 2 - DISPLAY_GAUGE_WIDTH;

    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, BG_COLOR);

    const char *title   = "AIR QUALITY";
    const char *content = "CO2 STATION";
    rect_t      tb      = text_box(title, title_font, 0, 0, 1);
    rect_t      cb      = text_box(content, label_font, 0, 0, 1);
    int         widest  = tb.w > cb.w ? tb.w : cb.w;
    int         t_top   = text_top_in_circle(screen->height, gauge_inner_radius, widest, 8);
    int         t_base  = t_top - tb.y;
    int         c_top   = t_top + tb.h + 4;
    int         c_base  = c_top - cb.y;

    dgx_font_string_utf8_screen(screen, screen->width / 2 - tb.w / 2, t_base, title, DGX_WHITE(DGX_RGB_16),
                                DgxOutputNormal, 1, title_font, NULL, NULL);
    dgx_font_string_utf8_screen(screen, screen->width / 2 - cb.w / 2, c_base, content, DGX_LIGHTGREY(DGX_RGB_16),
                                DgxOutputNormal, 1, label_font, NULL, NULL);
}

/* Called from draw_dynamic_screen() in SensorReady state to build the temperature/humidity line. */
static void format_measurement(char *buf, size_t buf_size, int16_t temp_dc, uint16_t hum_dr)
{
    int         abs_t = temp_dc < 0 ? -(int)temp_dc : (int)temp_dc;
    const char *sign  = temp_dc < 0 ? "-" : "";
    snprintf(buf, buf_size, "%s%d.%d℃ %u.%u%%", sign, abs_t / 10, abs_t % 10, hum_dr / 10, hum_dr % 10);
}

/* Called from draw_dynamic_screen() in SensorReady state to build the 4-character CO2 display field. */
static void format_co2_digits(char digits[VALUE_DIGIT_COUNT + 1], uint16_t ppm)
{
    unsigned value = (unsigned)clamp_int((int)ppm, 0, SENSOR_CO2_MAX_PPM);

    memset(digits, ' ', VALUE_DIGIT_COUNT);
    for (int i = VALUE_DIGIT_COUNT - 1; i >= 0; --i) {
        digits[i] = (char)('0' + (value % 10u));
        value /= 10u;
        if (value == 0) {
            break;
        }
    }
    digits[VALUE_DIGIT_COUNT] = '\0';
}

/* Called by clock helpers to treat pre-SNTP epoch values as an unsynced wall clock. */
static bool time_now_if_synced(time_t *now_out, struct tm *tm_out)
{
    time_t now = 0;
    time(&now);
    if (now < (time_t)TIME_SYNC_VALID_UNIX_TIME) {
        return false;
    }
    if (now_out != NULL) {
        *now_out = now;
    }
    if (tm_out != NULL && localtime_r(&now, tm_out) == NULL) {
        return false;
    }
    return true;
}

/* Called during dashboard redraws to build the optional time row once network time is available. */
static bool format_time_text(char *buf, size_t buf_size)
{
    struct tm tm_now;
    if (!time_now_if_synced(NULL, &tm_now)) {
        return false;
    }
    return strftime(buf, buf_size, "%H:%M", &tm_now) > 0;
}

/* Called from app_main() bookkeeping so the time row redraws once per minute after SNTP sync. */
static bool current_synced_minute(time_t *minute_out)
{
    time_t now = 0;
    if (minute_out == NULL || !time_now_if_synced(&now, NULL)) {
        return false;
    }
    *minute_out = now / 60;
    return true;
}

/* Called after provisioning bootstrap initializes NVS-backed settings so localtime() uses the configured timezone. */
static void apply_configured_timezone(void)
{
    app_settings_t settings;
    const char    *timezone_name;

    app_settings_get_snapshot(&settings);
    timezone_name = settings.timezone[0] != '\0' ? settings.timezone : "Etc/UTC";

    if (setupTZ(timezone_name)) {
        ESP_LOGI(TAG, "Timezone set to %s", timezone_name);
        return;
    }

    ESP_LOGW(TAG, "Unknown timezone '%s', falling back to Etc/UTC", timezone_name);
    (void)setupTZ("Etc/UTC");
}

/* Called by the SNTP stack when wall time becomes available so sync success is visible in the log. */
static void on_time_synced(struct timeval *tv)
{
    (void)tv;

    char time_buf[8];
    if (!format_time_text(time_buf, sizeof(time_buf))) {
        return;
    }
    ESP_LOGI(TAG, "Time synced: %s", time_buf);
}

/* Called once from app_main() after starting provisioned Wi-Fi so SNTP can sync in the background. */
static void time_sync_start(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.wait_for_sync     = false;
    config.sync_cb           = on_time_synced;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SNTP already initialized");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start SNTP: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SNTP started");
}

/* Called whenever fresh sensor state or display wakeup requires the value and text rows to be redrawn. */
static void draw_dynamic_screen(dgx_screen_t *screen, dynamic_screen_t *ds, air_state_t state)
{
    char        meas_buf[32];
    char        time_buf[8];
    char        web_buf[16];
    const char *meas_text    = "--.-℃ --.-%";
    const char *status_text  = "";
    const char *time_text    = "";
    const char *web_text     = "";
    uint32_t    status_color = DGX_LIGHTGREY(DGX_RGB_16);

    switch (state.status) {
    case SensorFault:
        status_text = "offline";
        value_show_status(screen, &ds->value, "sensor", DGX_RED(DGX_RGB_16));
        break;
    case SensorStarting:
        status_text = "warming up";
        value_show_status(screen, &ds->value, "wait", DGX_CYAN(DGX_RGB_16));
        break;
    case SensorReady: {
        char digits[VALUE_DIGIT_COUNT + 1];
        format_co2_digits(digits, state.co2_ppm);
        format_measurement(meas_buf, sizeof(meas_buf), state.temperature_deci_c, state.humidity_deci_rh);
        meas_text    = meas_buf;
        status_text  = quality_label(state.co2_ppm);
        status_color = quality_text_color(state.co2_ppm);
        value_show_digits(screen, &ds->value, digits, DGX_WHITE(DGX_RGB_16), DGX_LIGHTGREY(DGX_RGB_16));
        break;
    }
    }

    if (format_time_text(time_buf, sizeof(time_buf))) {
        time_text = time_buf;
    }
    if (web_is_running() && app_wifi_get_ip_text(web_buf, sizeof(web_buf))) {
        web_text = web_buf;
    }

    text_slot_draw(screen, &ds->status, status_text, status_color);
    text_slot_draw(screen, &ds->measurement, meas_text, DGX_LIGHTGREY(DGX_RGB_16));
    text_slot_draw(screen, &ds->time, time_text, DGX_LIGHTGREY(DGX_RGB_16));
    text_slot_draw(screen, &ds->web_address, web_text, DGX_LIGHTGREY(DGX_RGB_16));
}

/* Called whenever sensor state changes or the whole screen is rebuilt to push the latest CO2 value into the gauge. */
static void update_gauge(dgx_gauge_t *gauge, air_state_t state)
{
    dgx_gauge_set_value(gauge, state.status == SensorReady ? clamp_gauge_ppm(state.co2_ppm) : GAUGE_MIN_PPM);
}

/* Called for full dashboard redraws when both the static background and dynamic readings must be refreshed. */
static void render_main(dgx_screen_t *screen, dgx_gauge_t *gauge, dynamic_screen_t *ds, air_state_t state)
{
    draw_static_screen(screen);
    update_gauge(gauge, state);
    draw_dynamic_screen(screen, ds, state);
}

/* ---------- SCD41 sensor task ---------- */

/* SCD4x factory temperature offset, subtracted from the raw reading to compensate
 * self-heating. The configurable "temperature_offset" setting is expressed
 * display-style (a positive value raises the reported temperature), so it is
 * mapped onto this register as (default - setting). */
#define SCD4X_DEFAULT_TEMP_OFFSET_C 4.0f
#define SCD4X_TEMP_OFFSET_EPSILON_C 0.05f

/* Writes the configured temperature offset into the sensor's self-heating
 * register, but only when it differs from the value already stored on the
 * sensor. The SCD4x must be in idle mode when this is called. */
static esp_err_t scd41_write_temperature_offset(scd4x_t *sensor, float user_offset)
{
    float desired = SCD4X_DEFAULT_TEMP_OFFSET_C - user_offset;
    if (desired < 0.0f) {
        desired = 0.0f;
    } else if (desired > 175.0f) {
        desired = 175.0f;
    }

    float     current = 0.0f;
    esp_err_t err     = scd4x_get_temperature_offset(sensor, &current);
    if (err != ESP_OK) {
        return err;
    }
    if (fabsf(current - desired) <= SCD4X_TEMP_OFFSET_EPSILON_C) {
        return ESP_OK;
    }

    err = scd4x_set_temperature_offset(sensor, desired);
    if (err != ESP_OK) {
        return err;
    }
    err = scd4x_persist_settings(sensor);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "SCD41 temperature offset updated: setting=%.2f C, sensor register=%.2f C", user_offset, desired);
    return ESP_OK;
}

/* Re-applies the temperature offset while periodic measurement is running by
 * briefly returning the sensor to idle mode. Called from the task loop when the
 * configured offset changes (e.g. after a Web-Server save). */
static void scd41_refresh_temperature_offset(scd41_runtime_t *s, float user_offset)
{
    esp_err_t err = scd4x_stop_periodic_measurement(s->sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SCD41 stop for offset update failed: %s", esp_err_to_name(err));
    } else {
        esp_err_t set_err = scd41_write_temperature_offset(s->sensor, user_offset);
        if (set_err == ESP_OK) {
            s->applied_temperature_offset = user_offset;
        } else {
            ESP_LOGW(TAG, "SCD41 temperature offset update failed: %s", esp_err_to_name(set_err));
        }
    }

    err = scd4x_start_periodic_measurement(s->sensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SCD41 restart after offset update failed: %s", esp_err_to_name(err));
    }
}

/* Called once by scd41_task() at task start to open the I2C bus and start periodic measurements. */
static esp_err_t scd41_open(scd41_runtime_t *s)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port                     = SCD41_I2C_PORT,
        .sda_io_num                   = SCD41_I2C_SDA,
        .scl_io_num                   = SCD41_I2C_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SCD4X_I2C_ADDR,
        .scl_speed_hz    = SCD41_I2C_FREQ_HZ,
    };

    s->bus    = NULL;
    s->dev    = NULL;
    s->sensor = NULL;

    esp_err_t err = i2c_new_master_bus(&bus_config, &s->bus);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_bus_add_device(s->bus, &dev_config, &s->dev);
    if (err != ESP_OK) {
        i2c_del_master_bus(s->bus);
        s->bus = NULL;
        return err;
    }
    s->sensor = scd4x_init(s->dev);
    if (s->sensor == NULL) {
        i2c_master_bus_rm_device(s->dev);
        i2c_del_master_bus(s->bus);
        s->dev = NULL;
        s->bus = NULL;
        return ESP_FAIL;
    }

    /* The sensor is idle right after init, so apply the configured temperature
     * offset before periodic measurement starts. */
    app_settings_t settings;
    app_settings_get_snapshot(&settings);
    esp_err_t offset_err = scd41_write_temperature_offset(s->sensor, settings.temperature_offset);
    if (offset_err != ESP_OK) {
        ESP_LOGW(TAG, "SCD41 temperature offset setup failed: %s", esp_err_to_name(offset_err));
    }
    s->applied_temperature_offset = settings.temperature_offset;

    return scd4x_start_periodic_measurement(s->sensor);
}

/* Created once from app_main(); it owns the sensor and publishes measurements into the queue forever. */
static void scd41_task(void *arg)
{
    scd41_runtime_t *s     = arg;
    air_state_t      state = {
             .status  = SensorFault,
             .co2_ppm = GAUGE_MIN_PPM,
    };
#define LQ_FILTER_SIZE 4
    static air_state_t lq_filter[LQ_FILTER_SIZE] = {0};
    static size_t      lq_index                  = 0;
    static size_t      lq_capacity               = 0;

    esp_err_t err = scd41_open(s);
    if (err != ESP_OK) {
        (void)xQueueOverwrite(s->queue, &state);
        ESP_LOGE(TAG, "SCD41 init failed (SDA=%d SCL=%d): %s", SCD41_I2C_SDA, SCD41_I2C_SCL, esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SCD41 ready (SDA=%d SCL=%d)", SCD41_I2C_SDA, SCD41_I2C_SCL);
    for (;;) {
        app_settings_t settings;
        app_settings_get_snapshot(&settings);

        if (settings.temperature_offset != s->applied_temperature_offset) {
            scd41_refresh_temperature_offset(s, settings.temperature_offset);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        scd4x_measurement_t m;
        err = scd4x_read_measurement(s->sensor, &m);
        if (err == ESP_OK) {
            int8_t rssi_dbm;
            bool   has_rssi_dbm;

            float co2  = m.co2 + settings.co2_offset;
            float temp = m.temperature;
            float hum  = m.humidity + settings.humidity_offset;
            if (hum < 0.0f) hum = 0.0f;
            if (hum > 100.0f) hum = 100.0f;

            state.status             = SensorReady;
            state.co2_ppm            = (uint16_t)clamp_int((int)lroundf(co2), 0, SENSOR_CO2_MAX_PPM);
            state.temperature_deci_c = (int16_t)lroundf(temp * 10.0f);
            state.humidity_deci_rh   = (uint16_t)lroundf(hum * 10.0f);
            // ESP_LOGI(TAG, "RAW [corrected] measurement: CO2=%u ppm, T=%.2f C, RH=%.2f %%", (unsigned)state.co2_ppm,
            // temp, hum);
            lq_filter[lq_index] = state;
            lq_index            = (lq_index + 1) % LQ_FILTER_SIZE;
            if (lq_capacity < LQ_FILTER_SIZE) {
                lq_capacity++;
            }
            state.co2_ppm = 0;
            for (size_t i = 0; i < lq_capacity; ++i) {
                state.co2_ppm += lq_filter[i].co2_ppm;
            }
            state.co2_ppm /= lq_capacity;
            has_rssi_dbm = app_wifi_get_rssi_dbm(&rssi_dbm);
            (void)xQueueOverwrite(s->queue, &state);
            app_mqtt_publish_measurement(&settings, state.co2_ppm, state.temperature_deci_c, state.humidity_deci_rh,
                                         has_rssi_dbm, rssi_dbm);
            if (has_rssi_dbm) {
                ESP_LOGI(TAG, "CO2=%u ppm, T=%.2f C, RH=%.2f %%, RSSI=%d dBm", (unsigned)state.co2_ppm, temp, hum,
                         (int)rssi_dbm);
            } else {
                ESP_LOGI(TAG, "CO2=%u ppm, T=%.2f C, RH=%.2f %%", (unsigned)state.co2_ppm, temp, hum);
            }
        } else if (err != ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "SCD41 read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------- Touch input ---------- */

/* Called once from app_main() after display setup to configure the TTP223 input pin. */
static esp_err_t touch_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << TOUCH_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

/* Called on each main-loop iteration when provisioning is idle to decode touches into high-level actions. */
static touch_action_t touch_poll(bool allow_double)
{
    static bool    was_pressed         = false;
    static bool    long_press_sent     = false;
    static bool    single_tap_pending  = false;
    static bool    double_tap_armed    = false;
    static int64_t press_start_us      = 0;
    static int64_t double_tap_deadline = 0;

    const bool    pressed = gpio_get_level(TOUCH_GPIO) == TOUCH_ACTIVE_LEVEL;
    const int64_t now     = esp_timer_get_time();

    /* Deferred single tap: release the saved tap once the double-tap window expires
     * (or immediately if double-tap detection is disabled). */
    if (single_tap_pending && !double_tap_armed && !pressed && (!allow_double || now >= double_tap_deadline)) {
        single_tap_pending = false;
        TOUCH_LOGD("single-tap window expired -> toggle");
        return TouchActionToggleDisplay;
    }

    if (pressed && !was_pressed) {
        if (allow_double && single_tap_pending && now < double_tap_deadline) {
            double_tap_armed = true;
            TOUCH_LOGD("touch down: double-tap armed");
        } else {
            TOUCH_LOGD("touch down (allow_double=%d, single_pending=%d)", (int)allow_double, (int)single_tap_pending);
        }
        press_start_us  = now;
        long_press_sent = false;
        was_pressed     = true;
        return TouchActionNone;
    }

    if (pressed && !long_press_sent && press_start_us != 0 && now - press_start_us >= TOUCH_PROVISION_HOLD_US) {
        long_press_sent    = true;
        single_tap_pending = false;
        double_tap_armed   = false;
        TOUCH_LOGD("long press -> provisioning");
        return TouchActionStartProvisioning;
    }

    if (!pressed && was_pressed) {
        const bool short_press = !long_press_sent && press_start_us != 0;
        was_pressed            = false;
        long_press_sent        = false;
        press_start_us         = 0;

        if (!short_press) {
            double_tap_armed = false;
            return TouchActionNone;
        }
        if (!allow_double) {
            double_tap_armed = false;
            TOUCH_LOGD("short tap -> toggle (double-tap disabled: Wi-Fi not provisioned)");
            return TouchActionToggleDisplay;
        }
        if (double_tap_armed) {
            double_tap_armed   = false;
            single_tap_pending = false;
            TOUCH_LOGD("double tap -> webserver");
            return TouchActionStartWebserver;
        }
        if (single_tap_pending && now < double_tap_deadline) {
            single_tap_pending = false;
            double_tap_armed   = false;
            TOUCH_LOGD("double tap (release-window) -> webserver");
            return TouchActionStartWebserver;
        }
        single_tap_pending  = true;
        double_tap_armed    = false;
        double_tap_deadline = now + TOUCH_DOUBLE_TAP_US;
        TOUCH_LOGD("first tap registered, waiting %dms for a second tap", (int)(TOUCH_DOUBLE_TAP_US / 1000));
    }

    return TouchActionNone;
}

/* Called in the main loop before redrawing so unchanged sensor samples do not trigger extra work. */
static bool air_state_equal(air_state_t a, air_state_t b)
{
    return a.status == b.status && a.co2_ppm == b.co2_ppm && a.temperature_deci_c == b.temperature_deci_c &&
           a.humidity_deci_rh == b.humidity_deci_rh;
}

/* Called after any full-screen interruption that discarded dashboard pixels to repaint the full dashboard. */
static void on_display_turned_on(dgx_screen_t *screen, dgx_gauge_t *gauge, dynamic_screen_t *ds, air_state_t shown)
{
    draw_static_screen(screen);
    dgx_gauge_redraw(gauge);
    ds->value.showing_digits = false;
    draw_dynamic_screen(screen, ds, shown);
}

/* Called before any action that needs visible UI, and when a touch toggles the display back on. */
static void ensure_display_on(dgx_screen_t *screen, bool *enabled, dgx_gauge_t *gauge, dynamic_screen_t *ds,
                              air_state_t shown)
{
    if (!*enabled) {
        dgx_gc9a01_display_on(screen);
        *enabled = true;
        on_display_turned_on(screen, gauge, ds, shown);
    }
}

/* Called by ESP-IDF after boot to initialize hardware, start tasks, and run the UI/event loop forever. */
void app_main(void)
{
    air_state_t shown = {
        .status  = SensorStarting,
        .co2_ppm = GAUGE_MIN_PPM,
    };

    QueueHandle_t queue = xQueueCreate(1, sizeof(shown));
    if (queue == NULL) {
        ESP_LOGE(TAG, "Failed to create measurement queue");
        return;
    }

    dgx_bus_protocols_t *bus = dgx_spi_init(SPI2_HOST, DISPLAY_DMA, DISPLAY_MOSI, -1, DISPLAY_SCLK, DISPLAY_CS,
                                            DISPLAY_DC, DISPLAY_CLOCK_SPEED_HZ, 0);
    if (bus == NULL) {
        return;
    }
    dgx_screen_t *screen = dgx_gc9a01_init(bus, DISPLAY_RST, 16, DgxScreenBGR);
    if (screen == NULL) {
        return;
    }
    dgx_gc9a01_orientation(screen, DgxScreenLeftRight, DgxScreenTopBottom, false);

    if (touch_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure TTP223 touch input");
        return;
    }

    if (app_wifi_start_station_if_provisioned() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi startup unavailable, continuing without network");
    } else {
        apply_configured_timezone();
        if (app_wifi_is_provisioned()) {
            time_sync_start();
        }
    }

    static scd41_runtime_t scd41;
    scd41.queue = queue;
    if (xTaskCreate(scd41_task, "scd41", 4096, &scd41, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start SCD41 task");
        vQueueDelete(queue);
        return;
    }

    dgx_gauge_t gauge;
    dgx_gauge_init(&gauge, screen, screen->width / 2, screen->height / 2, screen->width / 2 - DISPLAY_GAUGE_WIDTH,
                   DISPLAY_GAUGE_WIDTH, 225.0f, 270, GAUGE_MIN_PPM, GAUGE_MAX_PPM, BG_COLOR, gauge_color);

    dynamic_screen_t ds;
    dynamic_screen_init(&ds, screen);

    render_main(screen, &gauge, &ds, shown);

    bool   display_enabled     = true;
    bool   provisioning_active = false;
    time_t shown_clock_minute  = -1;
    (void)current_synced_minute(&shown_clock_minute);

    for (;;) {
        air_state_t state;
        if (xQueueReceive(queue, &state, 1) == pdPASS && !air_state_equal(state, shown)) {
            if (display_enabled && !provisioning_active) {
                update_gauge(&gauge, state);
                draw_dynamic_screen(screen, &ds, state);
            }
            shown = state;
        }

        if (display_enabled && !provisioning_active) {
            time_t clock_minute = 0;
            if (current_synced_minute(&clock_minute) && clock_minute != shown_clock_minute) {
                shown_clock_minute = clock_minute;
                draw_dynamic_screen(screen, &ds, shown);
            }
            (void)value_step_animation(screen, &ds.value);
        }

        if (provisioning_active) {
            switch (app_wifi_provisioning_step()) {
            case AppWifiProvisioningFinishedRestart:
                ESP_LOGI(TAG, "Provisioning updated Wi-Fi credentials, restarting");
                esp_restart();
                break;
            case AppWifiProvisioningFinishedNoChange:
            case AppWifiProvisioningFinishedError:
                provisioning_active = false;
                on_display_turned_on(screen, &gauge, &ds, shown);
                break;
            default:
                break;
            }
            continue;
        }

        switch (touch_poll(app_wifi_is_provisioned())) {
        case TouchActionToggleDisplay:
            if (display_enabled) {
                ESP_LOGI(TAG, "Turning display off");
                dgx_gc9a01_display_off(screen);
                display_enabled = false;
            } else {
                ESP_LOGI(TAG, "Turning display on");
                ensure_display_on(screen, &display_enabled, &gauge, &ds, shown);
            }
            break;
        case TouchActionStartWebserver:
            ensure_display_on(screen, &display_enabled, &gauge, &ds, shown);
            if (web_is_running()) {
                if (web_stop() == ESP_OK) {
                    ESP_LOGI(TAG, "Double touch detected, settings webserver stopped");
                    draw_dynamic_screen(screen, &ds, shown);
                } else {
                    ESP_LOGE(TAG, "Failed to stop settings webserver");
                }
            } else {
                if (web_start() == ESP_OK) {
                    ESP_LOGI(TAG, "Double touch detected, settings webserver is available");
                    draw_dynamic_screen(screen, &ds, shown);
                } else {
                    ESP_LOGE(TAG, "Failed to start settings webserver");
                }
            }
            break;
        case TouchActionStartProvisioning:
            ensure_display_on(screen, &display_enabled, &gauge, &ds, shown);
            ESP_LOGI(TAG, "Touch held for %d ms, showing provisioning screen", TOUCH_PROVISION_HOLD_US / 1000);
            if (app_wifi_provisioning_begin(screen, PROVISIONING_TIMEOUT_MS) == ESP_OK) {
                provisioning_active = true;
            } else {
                ESP_LOGE(TAG, "Failed to start provisioning mode");
                render_main(screen, &gauge, &ds, shown);
            }
            break;
        case TouchActionNone:
            break;
        }
    }
}
