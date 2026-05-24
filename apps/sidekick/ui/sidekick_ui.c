/**
 * @file sidekick_ui.c
 * @brief SideKick display and touch UI.
 *
 * @copyright Copyright (c) 2026 SideKick Contributors. All Rights Reserved.
 *
 */
#include "sidekick_ui.h"

#include <string.h>

#include "sidekick_audio.h"
#include "sidekick_camera.h"
#include "sidekick_log.h"
#include "sidekick_session.h"
#include "tal_time_service.h"

#if defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
#include "tdl_display_manage.h"

#define SIDEKICK_COLOR_BG             sidekick_ui_color(0x10, 0x18, 0x28)
#define SIDEKICK_COLOR_CARD           sidekick_ui_color(0x1D, 0x29, 0x39)
#define SIDEKICK_COLOR_ACCENT         sidekick_ui_color(0x84, 0xCA, 0xFF)
#define SIDEKICK_COLOR_DIM            sidekick_ui_color(0x47, 0x55, 0x67)
#define SIDEKICK_COLOR_SELECTED       sidekick_ui_color(0xB7, 0xE4, 0xC7)
#define SIDEKICK_WORDMARK_LETTERS     8
#define SIDEKICK_KICK_LETTERS         4
#define SIDEKICK_TEST_LETTERS         4
#define SIDEKICK_END_LETTERS          3
#define SIDEKICK_GLYPH_WIDTH          5
#define SIDEKICK_GLYPH_SPACING        1
#define SIDEKICK_GLYPH_HEIGHT         7
#define SIDEKICK_LABEL_UNIT           4
#define SIDEKICK_START_LETTERS        5
#define SIDEKICK_MODE_COUNT           3
#define SIDEKICK_INTERVAL_COUNT       4
#define SIDEKICK_INTERVAL_LABEL_CHARS 3
#define SIDEKICK_INTERVAL_TITLE_CHARS 8
#define SIDEKICK_SESSION_END_GRACE_MS 3000

typedef enum {
    SIDEKICK_UI_SCREEN_HOME = 0,
    SIDEKICK_UI_SCREEN_KICK,
    SIDEKICK_UI_SCREEN_SETTINGS,
    SIDEKICK_UI_SCREEN_TESTS,
    SIDEKICK_UI_SCREEN_CAMERA,
} SIDEKICK_UI_SCREEN_E;

static TDL_DISP_HANDLE_T      s_display_handle = NULL;
static TDL_DISP_DEV_INFO_T    s_display_info;
static TDL_DISP_FRAME_BUFF_T *s_display_fb[2] = {NULL, NULL};
static uint8_t                s_draw_fb_index = 0;
static uint16_t               s_canvas_width  = 0;
static uint16_t               s_canvas_height = 0;
static bool                   s_rotate_canvas = false;
static bool                   s_flip_canvas   = true;
static SIDEKICK_UI_SCREEN_E   s_screen        = SIDEKICK_UI_SCREEN_HOME;

static uint32_t sidekick_ui_color(uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_display_info.fmt == TUYA_PIXEL_FMT_RGB888) {
        return ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
    }

    return (((uint32_t)red & 0xF8) << 8) | (((uint32_t)green & 0xFC) << 3) | ((uint32_t)blue >> 3);
}

static TDL_DISP_FRAME_BUFF_T *sidekick_ui_draw_fb(void)
{
    return s_display_fb[s_draw_fb_index];
}

static uint16_t sidekick_ui_rgb565_pixel(uint32_t color)
{
    uint16_t pixel = (uint16_t)(color & 0xFFFF);

    if (s_display_info.is_swap) {
        pixel = (uint16_t)((pixel << 8) | (pixel >> 8));
    }

    return pixel;
}

static void sidekick_ui_setup_fb(TDL_DISP_FRAME_BUFF_T *fb)
{
    fb->x_start = 0;
    fb->y_start = 0;
    fb->fmt     = s_display_info.fmt;
    fb->width   = s_display_info.width;
    fb->height  = s_display_info.height;
}

static void sidekick_ui_clear_screen(void)
{
    TDL_DISP_FRAME_BUFF_T *fb     = sidekick_ui_draw_fb();
    uint32_t               pixels = (uint32_t)fb->width * (uint32_t)fb->height;

    if (fb->fmt == TUYA_PIXEL_FMT_RGB565) {
        uint16_t  fill = sidekick_ui_rgb565_pixel(SIDEKICK_COLOR_BG);
        uint16_t *p    = (uint16_t *)fb->frame;

        for (uint32_t i = 0; i < pixels; i++) {
            p[i] = fill;
        }
        return;
    }

    tdl_disp_draw_fill_full(fb, SIDEKICK_COLOR_BG, s_display_info.is_swap);
}

static OPERATE_RET sidekick_ui_present(void)
{
    OPERATE_RET rt = OPRT_OK;

    TUYA_CALL_ERR_RETURN(tdl_disp_dev_flush(s_display_handle, sidekick_ui_draw_fb()));
    s_draw_fb_index ^= 1;
    return rt;
}

static OPERATE_RET sidekick_ui_fill_rect_raw(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint32_t color)
{
    TDL_DISP_FRAME_BUFF_T *fb = sidekick_ui_draw_fb();

    if ((fb == NULL) || (width == 0) || (height == 0)) {
        return OPRT_OK;
    }

    if ((x >= fb->width) || (y >= fb->height)) {
        return OPRT_OK;
    }

    if ((x + width) > fb->width) {
        width = fb->width - x;
    }

    if ((y + height) > fb->height) {
        height = fb->height - y;
    }

    TDL_DISP_RECT_T rect = {
        .x0 = x,
        .y0 = y,
        .x1 = x + width - 1,
        .y1 = y + height - 1,
    };

    return tdl_disp_draw_fill(fb, &rect, color, s_display_info.is_swap);
}

static OPERATE_RET sidekick_ui_fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint32_t color)
{
    if (s_flip_canvas) {
        x = s_canvas_width - x - width;
        y = s_canvas_height - y - height;
    }

    if (!s_rotate_canvas) {
        return sidekick_ui_fill_rect_raw(x, y, width, height, color);
    }

    return sidekick_ui_fill_rect_raw(s_canvas_height - y - height, x, height, width, color);
}

static const uint8_t *sidekick_ui_glyph(char letter)
{
    static const uint8_t glyph_0[7] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    static const uint8_t glyph_1[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    static const uint8_t glyph_2[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    static const uint8_t glyph_3[7] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    static const uint8_t glyph_5[7] = {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E};
    static const uint8_t glyph_a[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const uint8_t glyph_c[7] = {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F};
    static const uint8_t glyph_d[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    static const uint8_t glyph_e[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    static const uint8_t glyph_i[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    static const uint8_t glyph_k[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    static const uint8_t glyph_l[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    static const uint8_t glyph_m[7] = {0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11};
    static const uint8_t glyph_p[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t glyph_r[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    static const uint8_t glyph_s[7] = {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E};
    static const uint8_t glyph_t[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t glyph_h[7] = {0x11, 0x11, 0x1F, 0x11, 0x11, 0x11, 0x11};
    static const uint8_t glyph_n[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    static const uint8_t glyph_o[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_u[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_v[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
    static const uint8_t glyph_y[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};

    switch (letter) {
    case '0':
        return glyph_0;
    case '1':
        return glyph_1;
    case '2':
        return glyph_2;
    case '3':
        return glyph_3;
    case '5':
        return glyph_5;
    case 'A':
        return glyph_a;
    case 'C':
        return glyph_c;
    case 'D':
        return glyph_d;
    case 'E':
        return glyph_e;
    case 'H':
        return glyph_h;
    case 'I':
        return glyph_i;
    case 'K':
        return glyph_k;
    case 'L':
        return glyph_l;
    case 'M':
        return glyph_m;
    case 'N':
        return glyph_n;
    case 'O':
        return glyph_o;
    case 'P':
        return glyph_p;
    case 'R':
        return glyph_r;
    case 'S':
        return glyph_s;
    case 'T':
        return glyph_t;
    case 'U':
        return glyph_u;
    case 'V':
        return glyph_v;
    case 'Y':
        return glyph_y;
    default:
        return NULL;
    }
}

static void sidekick_ui_draw_block_letter(char letter, uint16_t x, uint16_t y, uint16_t unit, uint32_t color)
{
    OPERATE_RET    rt     = OPRT_OK;
    const uint8_t *glyph  = sidekick_ui_glyph(letter);
    uint16_t       stroke = unit;

    if (glyph == NULL) {
        return;
    }

    for (uint8_t row = 0; row < 7; row++) {
        for (uint8_t col = 0; col < 5; col++) {
            if ((glyph[row] & (0x10 >> col)) == 0) {
                continue;
            }

            TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + col * unit, y + row * unit, stroke, stroke, color));
        }
    }
}

static void sidekick_ui_draw_block_text(const char *text, uint16_t x, uint16_t y, uint16_t unit, uint32_t color)
{
    while (*text != '\0') {
        char letter = *text;

        if ((letter >= 'a') && (letter <= 'z')) {
            letter = (char)(letter - ('a' - 'A'));
        }

        sidekick_ui_draw_block_letter(letter, x, y, unit, color);
        x += unit * 6;
        text++;
    }
}

static uint16_t sidekick_ui_text_width(uint16_t letter_count, uint16_t unit)
{
    uint16_t text_units = letter_count * SIDEKICK_GLYPH_WIDTH + (letter_count - 1) * SIDEKICK_GLYPH_SPACING;

    return text_units * unit;
}

static void sidekick_ui_home_actions_layout(uint16_t *kick_x, uint16_t *test_x, uint16_t *button_y, uint16_t *button_w,
                                            uint16_t *button_h)
{
    uint16_t width  = s_canvas_width;
    uint16_t height = s_canvas_height;
    uint16_t margin = width / 16;
    uint16_t gap    = width / 20;

    *button_w = (width - margin * 2 - gap) / 2;
    *button_h = height / 3;
    *button_y = height * 57 / 100;
    *kick_x   = margin;
    *test_x   = margin + *button_w + gap;
}

static uint16_t sidekick_ui_fit_text_unit(uint16_t letter_count, uint16_t rect_w, uint16_t rect_h, uint16_t preferred)
{
    uint16_t text_units  = letter_count * SIDEKICK_GLYPH_WIDTH + (letter_count - 1) * SIDEKICK_GLYPH_SPACING;
    uint16_t width_unit  = (rect_w > 4) ? ((rect_w - 4) / text_units) : 1;
    uint16_t height_unit = (rect_h > 4) ? ((rect_h - 4) / SIDEKICK_GLYPH_HEIGHT) : 1;
    uint16_t unit        = preferred;

    if (unit > width_unit) {
        unit = width_unit;
    }

    if (unit > height_unit) {
        unit = height_unit;
    }

    if (unit == 0) {
        unit = 1;
    }

    return unit;
}

static void sidekick_ui_home_button_layout(uint16_t *home_x, uint16_t *home_y, uint16_t *home_size)
{
    *home_size = s_canvas_height / 5;
    *home_x    = s_canvas_width / 16;
    *home_y    = s_canvas_height / 12;
}

static void sidekick_ui_tests_layout(uint16_t *home_x, uint16_t *home_y, uint16_t *home_size, uint16_t *speaker_x,
                                     uint16_t *speaker_y, uint16_t *camera_x, uint16_t *camera_y, uint16_t *card_w,
                                     uint16_t *card_h)
{
    uint16_t width  = s_canvas_width;
    uint16_t height = s_canvas_height;

    sidekick_ui_home_button_layout(home_x, home_y, home_size);
    *card_w    = width / 3;
    *card_h    = height / 2;
    *speaker_x = width / 9;
    *speaker_y = height / 3;
    *camera_x  = width - *speaker_x - *card_w;
    *camera_y  = *speaker_y;
}

static bool sidekick_ui_point_in_rect(uint16_t x, uint16_t y, uint16_t rect_x, uint16_t rect_y, uint16_t rect_w,
                                      uint16_t rect_h)
{
    return (x >= rect_x) && (x < (rect_x + rect_w)) && (y >= rect_y) && (y < (rect_y + rect_h));
}

static void sidekick_ui_map_touch(uint16_t raw_x, uint16_t raw_y, uint16_t *canvas_x, uint16_t *canvas_y)
{
    uint16_t x = raw_x;
    uint16_t y = raw_y;

    if (s_rotate_canvas) {
        x = raw_y;
        y = s_canvas_height - raw_x - 1;
    }

    if (s_flip_canvas) {
        x = s_canvas_width - x - 1;
        y = s_canvas_height - y - 1;
    }

    *canvas_x = x;
    *canvas_y = y;
}

static void sidekick_ui_draw_home_icon(uint16_t x, uint16_t y, uint16_t size, uint32_t color)
{
    OPERATE_RET rt   = OPRT_OK;
    uint16_t    unit = size / 8;

    if (unit == 0) {
        unit = 1;
    }

    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 2, y + unit * 4, unit * 4, unit * 3, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 3, y + unit * 5, unit * 2, unit * 2, SIDEKICK_COLOR_CARD));

    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 3, y + unit, unit * 2, unit, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 2, y + unit * 2, unit * 4, unit, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit, y + unit * 3, unit * 6, unit, color));
}

static void sidekick_ui_draw_back_icon(uint16_t x, uint16_t y, uint16_t size, uint32_t color)
{
    OPERATE_RET rt   = OPRT_OK;
    uint16_t    unit = size / 8;
    uint16_t    nudge;

    if (unit == 0) {
        unit = 1;
    }

    nudge = (unit > 2) ? 4 : 3;

    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 2, y + unit * 3, unit * 5, unit * 2, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 5 - nudge, y + unit, unit, unit * 6, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 4 - nudge, y + unit * 2, unit, unit, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 4 - nudge, y + unit * 5, unit, unit, color));
}

static void sidekick_ui_draw_speaker_icon(uint16_t x, uint16_t y, uint16_t size, uint32_t color)
{
    OPERATE_RET rt   = OPRT_OK;
    uint16_t    unit = size / 10;

    if (unit == 0) {
        unit = 1;
    }

    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit, y + unit * 4, unit * 2, unit * 3, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 3, y + unit * 3, unit, unit * 5, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 4, y + unit * 2, unit, unit * 7, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 6, y + unit * 3, unit, unit * 5, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 8, y + unit * 2, unit, unit * 7, color));
}

static void sidekick_ui_draw_camera_icon(uint16_t x, uint16_t y, uint16_t size, uint32_t color)
{
    OPERATE_RET rt     = OPRT_OK;
    uint16_t    unit   = size / 10;
    uint32_t    cutout = SIDEKICK_COLOR_ACCENT;

    if (unit == 0) {
        unit = 1;
    }

    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit, y + unit * 3, unit * 8, unit * 5, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 3, y + unit * 2, unit * 2, unit, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 6, y + unit * 4, unit, unit, cutout));

    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 3, y + unit * 4, unit * 4, unit, cutout));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 2, y + unit * 5, unit * 6, unit * 2, cutout));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 3, y + unit * 7, unit * 4, unit, cutout));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(x + unit * 4, y + unit * 5, unit * 2, unit * 2, color));
}

static void sidekick_ui_settings_button_layout(uint16_t *settings_x, uint16_t *settings_y, uint16_t *settings_size)
{
    sidekick_ui_home_button_layout(settings_x, settings_y, settings_size);
    *settings_x = s_canvas_width - *settings_x - *settings_size;
}

static void sidekick_ui_settings_layout(uint16_t *back_x, uint16_t *back_y, uint16_t *back_size, uint16_t *home_x,
                                        uint16_t *home_y, uint16_t *home_size, uint16_t *option_x, uint16_t *option_y,
                                        uint16_t *option_w, uint16_t *option_h, uint16_t *option_gap)
{
    uint16_t width  = s_canvas_width;
    uint16_t height = s_canvas_height;
    uint16_t margin = width / 16;

    sidekick_ui_home_button_layout(back_x, back_y, back_size);
    *home_size  = *back_size;
    *home_x     = width - *back_x - *home_size;
    *home_y     = *back_y;
    *option_gap = width / 24;
    *option_x   = margin;
    *option_y   = height * 35 / 100;
    *option_w   = (width - (margin * 2) - *option_gap) / 2;
    *option_h   = height / 5;
}

static void sidekick_ui_draw_settings_icon(uint16_t x, uint16_t y, uint16_t size, uint32_t color)
{
    OPERATE_RET rt     = OPRT_OK;
    uint16_t    unit   = size / 12;
    uint32_t    cutout = SIDEKICK_COLOR_CARD;
    uint16_t    icon_x;
    uint16_t    icon_y;

    if (unit == 0) {
        unit = 1;
    }

    icon_x = x + ((size > unit * 12) ? ((size - unit * 12) / 2) : 0);
    icon_y = y + ((size > unit * 12) ? ((size - unit * 12) / 2) : 0);

    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(icon_x + unit * 5, icon_y + unit, unit * 2, unit * 2, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(icon_x + unit * 5, icon_y + unit * 9, unit * 2, unit * 2, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(icon_x + unit, icon_y + unit * 5, unit * 2, unit * 2, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(icon_x + unit * 9, icon_y + unit * 5, unit * 2, unit * 2, color));

    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(icon_x + unit * 2, icon_y + unit * 2, unit * 3, unit * 3, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(icon_x + unit * 7, icon_y + unit * 2, unit * 3, unit * 3, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(icon_x + unit * 2, icon_y + unit * 7, unit * 3, unit * 3, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(icon_x + unit * 7, icon_y + unit * 7, unit * 3, unit * 3, color));

    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(icon_x + unit * 3, icon_y + unit * 4, unit * 6, unit * 4, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(icon_x + unit * 4, icon_y + unit * 3, unit * 4, unit * 6, color));
    TUYA_CALL_ERR_LOG(sidekick_ui_fill_rect(icon_x + unit * 5, icon_y + unit * 5, unit * 2, unit * 2, cutout));
}

static const uint32_t s_interval_options[SIDEKICK_INTERVAL_COUNT] = {10, 15, 20, 30};
static const char    *s_interval_labels[SIDEKICK_INTERVAL_COUNT]  = {"10S", "15S", "20S", "30S"};

static void sidekick_ui_draw_centered_text(const char *text, uint16_t letter_count, uint16_t rect_x, uint16_t rect_y,
                                           uint16_t rect_w, uint16_t rect_h, uint16_t unit, uint32_t color)
{
    uint16_t text_w = sidekick_ui_text_width(letter_count, unit);
    uint16_t text_h = SIDEKICK_GLYPH_HEIGHT * unit;
    uint16_t text_x = rect_x + ((rect_w > text_w) ? ((rect_w - text_w) / 2) : 0);
    uint16_t text_y = rect_y + ((rect_h > text_h) ? ((rect_h - text_h) / 2) : 0);

    sidekick_ui_draw_block_text(text, text_x, text_y, unit, color);
}

static void sidekick_ui_draw_label_in_rect(const char *text, uint16_t rect_x, uint16_t rect_y, uint16_t rect_w,
                                           uint16_t rect_h, uint32_t fg)
{
    uint16_t letter_count = (uint16_t)strlen(text);
    uint16_t unit         = sidekick_ui_fit_text_unit(letter_count, rect_w, rect_h, SIDEKICK_LABEL_UNIT);
    uint16_t text_w       = sidekick_ui_text_width(letter_count, unit);
    uint16_t text_h       = SIDEKICK_GLYPH_HEIGHT * unit;
    uint16_t text_x       = rect_x + ((rect_w > text_w) ? ((rect_w - text_w) / 2) : 0);
    uint16_t text_y       = rect_y + ((rect_h > text_h) ? ((rect_h - text_h) / 2) : 0);

    sidekick_ui_draw_block_text(text, text_x, text_y, unit, fg);
}

static const char *sidekick_ui_mode_label(SIDEKICK_TUTOR_MODE_E mode)
{
    switch (mode) {
    case SIDEKICK_TUTOR_MODE_ACTIVE:
        return "ACTIVE";
    case SIDEKICK_TUTOR_MODE_HINT:
        return "HINT";
    case SIDEKICK_TUTOR_MODE_SUMMARY:
        return "SUMMARY";
    default:
        return "?";
    }
}

static uint32_t sidekick_ui_mode_fill(SIDEKICK_TUTOR_MODE_E mode)
{
    return (mode == sidekick_session_mode()) ? SIDEKICK_COLOR_SELECTED : SIDEKICK_COLOR_CARD;
}

static void sidekick_ui_kick_layout(uint16_t *home_x, uint16_t *home_y, uint16_t *home_size, uint16_t *start_x,
                                    uint16_t *start_y, uint16_t *start_w, uint16_t *start_h, uint16_t *mode_x,
                                    uint16_t *mode_y, uint16_t *mode_w, uint16_t *mode_h, uint16_t *mode_gap)
{
    uint16_t width  = s_canvas_width;
    uint16_t height = s_canvas_height;
    uint16_t margin = width / 16;

    sidekick_ui_home_button_layout(home_x, home_y, home_size);
    *start_x  = margin;
    *start_w  = width - (margin * 2);
    *start_h  = height * 22 / 100;
    *start_y  = height * 28 / 100;
    *mode_x   = margin;
    *mode_w   = width - (margin * 2);
    *mode_gap = 4;
    *mode_h   = (height - *start_y - *start_h - (margin * 2) - (*mode_gap * 2)) / SIDEKICK_MODE_COUNT;
    *mode_y   = *start_y + *start_h + margin + *mode_gap;
}

static void sidekick_ui_kick_active_layout(uint16_t *end_x, uint16_t *end_y, uint16_t *end_w, uint16_t *end_h,
                                           uint16_t *preview_x, uint16_t *preview_y, uint16_t *preview_w,
                                           uint16_t *preview_h)
{
    uint16_t width     = s_canvas_width;
    uint16_t height    = s_canvas_height;
    uint16_t margin    = width / 20;
    uint16_t header_h  = height / 7;
    uint16_t min_hit_h = height / 9;
    uint16_t min_hit_w = width / 5;
    uint16_t text_unit = sidekick_ui_fit_text_unit(SIDEKICK_END_LETTERS, min_hit_w, header_h, header_h / 10);
    uint16_t text_w    = sidekick_ui_text_width(SIDEKICK_END_LETTERS, text_unit);
    uint16_t target_w  = text_w + margin;

    if (header_h < min_hit_h) {
        header_h = min_hit_h;
    }

    if (target_w < min_hit_w) {
        target_w = min_hit_w;
    }

    *end_w     = target_w;
    *end_h     = header_h;
    *end_x     = width - margin - *end_w;
    *end_y     = 0;
    *preview_x = 0;
    *preview_y = header_h;
    *preview_w = width;
    *preview_h = height - header_h;
}

static OPERATE_RET sidekick_ui_start_session_preview(void)
{
    uint16_t end_x;
    uint16_t end_y;
    uint16_t end_w;
    uint16_t end_h;
    uint16_t preview_x;
    uint16_t preview_y;
    uint16_t preview_w;
    uint16_t preview_h;

    sidekick_ui_kick_active_layout(&end_x, &end_y, &end_w, &end_h, &preview_x, &preview_y, &preview_w, &preview_h);
    return sidekick_camera_preview_start_with_header(s_canvas_width, s_canvas_height, s_rotate_canvas, s_flip_canvas,
                                                     preview_y, end_x, end_y, end_w, end_h);
}

static OPERATE_RET sidekick_ui_draw_home_screen(void)
{
    OPERATE_RET rt     = OPRT_OK;
    uint16_t    width  = s_canvas_width;
    uint16_t    height = s_canvas_height;
    uint16_t    unit   = height / 28;
    uint16_t    wordmark_w =
        SIDEKICK_WORDMARK_LETTERS * SIDEKICK_GLYPH_WIDTH + (SIDEKICK_WORDMARK_LETTERS - 1) * SIDEKICK_GLYPH_SPACING;
    uint16_t max_unit = (width > 16) ? ((width - 16) / wordmark_w) : 1;
    uint16_t word_w;
    uint16_t word_x;
    uint16_t word_y;
    uint16_t button_unit;
    uint16_t kick_x;
    uint16_t test_x;
    uint16_t button_y;
    uint16_t button_w;
    uint16_t button_h;

    if ((max_unit > 0) && (unit > max_unit)) {
        unit = max_unit;
    }

    if (unit < 3) {
        unit = 3;
    }

    word_w = sidekick_ui_text_width(SIDEKICK_WORDMARK_LETTERS, unit);
    word_x = (width > word_w) ? ((width - word_w) / 2) : unit;
    word_y = height / 12;

    sidekick_ui_home_actions_layout(&kick_x, &test_x, &button_y, &button_w, &button_h);
    button_unit = sidekick_ui_fit_text_unit(SIDEKICK_KICK_LETTERS, button_w, button_h, button_h / 8);

    sidekick_ui_clear_screen();
    sidekick_ui_draw_block_text("SIDEKICK", word_x, word_y, unit, SIDEKICK_COLOR_ACCENT);

    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(kick_x, button_y, button_w, button_h, SIDEKICK_COLOR_SELECTED));
    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(test_x, button_y, button_w, button_h, SIDEKICK_COLOR_ACCENT));
    sidekick_ui_draw_centered_text("KICK", SIDEKICK_KICK_LETTERS, kick_x, button_y, button_w, button_h, button_unit,
                                   SIDEKICK_COLOR_BG);
    sidekick_ui_draw_centered_text("TEST", SIDEKICK_TEST_LETTERS, test_x, button_y, button_w, button_h, button_unit,
                                   SIDEKICK_COLOR_BG);

    return sidekick_ui_present();
}

static OPERATE_RET sidekick_ui_draw_kick_modes(uint16_t mode_x, uint16_t mode_y, uint16_t mode_w, uint16_t mode_h,
                                               uint16_t mode_gap)
{
    OPERATE_RET rt = OPRT_OK;
    uint16_t    row_y;
    uint32_t    label_fg;

    row_y = mode_y;
    for (SIDEKICK_TUTOR_MODE_E mode = SIDEKICK_TUTOR_MODE_ACTIVE; mode <= SIDEKICK_TUTOR_MODE_SUMMARY; mode++) {
        uint32_t fill = sidekick_ui_mode_fill(mode);

        label_fg = (fill == SIDEKICK_COLOR_SELECTED) ? SIDEKICK_COLOR_BG : SIDEKICK_COLOR_ACCENT;
        TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(mode_x, row_y, mode_w, mode_h, fill));
        sidekick_ui_draw_label_in_rect(sidekick_ui_mode_label(mode), mode_x, row_y, mode_w, mode_h, label_fg);
        row_y += mode_h + mode_gap;
    }

    return rt;
}

static OPERATE_RET sidekick_ui_draw_kick_active_screen(void)
{
    OPERATE_RET rt = OPRT_OK;
    uint16_t    end_x;
    uint16_t    end_y;
    uint16_t    end_w;
    uint16_t    end_h;
    uint16_t    preview_x;
    uint16_t    preview_y;
    uint16_t    preview_w;
    uint16_t    preview_h;
    uint16_t    text_unit;

    sidekick_ui_kick_active_layout(&end_x, &end_y, &end_w, &end_h, &preview_x, &preview_y, &preview_w, &preview_h);
    text_unit = sidekick_ui_fit_text_unit(SIDEKICK_END_LETTERS, end_w, end_h, end_h / 8);

    sidekick_ui_clear_screen();
    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(preview_x, preview_y, preview_w, preview_h, SIDEKICK_COLOR_CARD));
    sidekick_ui_draw_centered_text("END", SIDEKICK_END_LETTERS, end_x, end_y, end_w, end_h, text_unit,
                                   SIDEKICK_COLOR_SELECTED);

    return sidekick_ui_present();
}

static OPERATE_RET sidekick_ui_draw_kick_screen(void)
{
    OPERATE_RET rt = OPRT_OK;
    uint16_t    home_x;
    uint16_t    home_y;
    uint16_t    home_size;
    uint16_t    settings_x;
    uint16_t    settings_y;
    uint16_t    settings_size;
    uint16_t    start_x;
    uint16_t    start_y;
    uint16_t    start_w;
    uint16_t    start_h;
    uint16_t    mode_x;
    uint16_t    mode_y;
    uint16_t    mode_w;
    uint16_t    mode_h;
    uint16_t    mode_gap;

    if (sidekick_session_is_active()) {
        return sidekick_ui_draw_kick_active_screen();
    }

    sidekick_ui_kick_layout(&home_x, &home_y, &home_size, &start_x, &start_y, &start_w, &start_h, &mode_x, &mode_y,
                            &mode_w, &mode_h, &mode_gap);
    sidekick_ui_settings_button_layout(&settings_x, &settings_y, &settings_size);

    sidekick_ui_clear_screen();
    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(home_x, home_y, home_size, home_size, SIDEKICK_COLOR_CARD));
    sidekick_ui_draw_home_icon(home_x + home_size / 8, home_y + home_size / 8, home_size * 3 / 4,
                               SIDEKICK_COLOR_ACCENT);
    TUYA_CALL_ERR_RETURN(
        sidekick_ui_fill_rect(settings_x, settings_y, settings_size, settings_size, SIDEKICK_COLOR_CARD));
    sidekick_ui_draw_settings_icon(settings_x + settings_size / 8, settings_y + settings_size / 8,
                                   settings_size * 3 / 4, SIDEKICK_COLOR_ACCENT);

    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(start_x, start_y, start_w, start_h, SIDEKICK_COLOR_SELECTED));
    sidekick_ui_draw_label_in_rect("START", start_x, start_y, start_w, start_h, SIDEKICK_COLOR_BG);
    TUYA_CALL_ERR_RETURN(sidekick_ui_draw_kick_modes(mode_x, mode_y, mode_w, mode_h, mode_gap));

    return sidekick_ui_present();
}

static OPERATE_RET sidekick_ui_draw_settings_screen(void)
{
    OPERATE_RET rt = OPRT_OK;
    uint16_t    back_x;
    uint16_t    back_y;
    uint16_t    back_size;
    uint16_t    home_x;
    uint16_t    home_y;
    uint16_t    home_size;
    uint16_t    option_x;
    uint16_t    option_y;
    uint16_t    option_w;
    uint16_t    option_h;
    uint16_t    option_gap;
    uint16_t    unit;
    uint16_t    title_y;
    uint32_t    selected_interval = sidekick_session_frame_interval_sec();

    sidekick_ui_settings_layout(&back_x, &back_y, &back_size, &home_x, &home_y, &home_size, &option_x, &option_y,
                                &option_w, &option_h, &option_gap);
    unit = sidekick_ui_fit_text_unit(SIDEKICK_INTERVAL_LABEL_CHARS, option_w, option_h, option_h / 7);

    sidekick_ui_clear_screen();
    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(back_x, back_y, back_size, back_size, SIDEKICK_COLOR_CARD));
    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(home_x, home_y, home_size, home_size, SIDEKICK_COLOR_CARD));
    sidekick_ui_draw_back_icon(back_x + back_size / 8, back_y + back_size / 8, back_size * 3 / 4,
                               SIDEKICK_COLOR_ACCENT);
    sidekick_ui_draw_home_icon(home_x + home_size / 8, home_y + home_size / 8, home_size * 3 / 4,
                               SIDEKICK_COLOR_ACCENT);

    for (uint8_t i = 0; i < SIDEKICK_INTERVAL_COUNT; i++) {
        uint16_t row    = i / 2;
        uint16_t col    = i % 2;
        uint16_t rect_x = option_x + col * (option_w + option_gap);
        uint16_t rect_y = option_y + row * (option_h + option_gap);
        uint32_t fill   = (s_interval_options[i] == selected_interval) ? SIDEKICK_COLOR_SELECTED : SIDEKICK_COLOR_CARD;
        uint32_t fg     = (fill == SIDEKICK_COLOR_SELECTED) ? SIDEKICK_COLOR_BG : SIDEKICK_COLOR_ACCENT;

        TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(rect_x, rect_y, option_w, option_h, fill));
        sidekick_ui_draw_centered_text(s_interval_labels[i], SIDEKICK_INTERVAL_LABEL_CHARS, rect_x, rect_y, option_w,
                                       option_h, unit, fg);
    }

    title_y = option_y + option_h * 2 + option_gap + option_gap;
    sidekick_ui_draw_centered_text("INTERVAL", SIDEKICK_INTERVAL_TITLE_CHARS, option_x, title_y,
                                   s_canvas_width - option_x * 2, option_h / 2, SIDEKICK_LABEL_UNIT,
                                   SIDEKICK_COLOR_DIM);

    return sidekick_ui_present();
}

static OPERATE_RET sidekick_ui_draw_tests_screen(void)
{
    OPERATE_RET rt = OPRT_OK;
    uint16_t    home_x;
    uint16_t    home_y;
    uint16_t    home_size;
    uint16_t    speaker_x;
    uint16_t    speaker_y;
    uint16_t    camera_x;
    uint16_t    camera_y;
    uint16_t    card_w;
    uint16_t    card_h;
    uint16_t    icon_size;

    sidekick_ui_tests_layout(&home_x, &home_y, &home_size, &speaker_x, &speaker_y, &camera_x, &camera_y, &card_w,
                             &card_h);
    icon_size = (card_h < card_w) ? (card_h * 3 / 4) : (card_w * 3 / 4);

    sidekick_ui_clear_screen();
    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(home_x, home_y, home_size, home_size, SIDEKICK_COLOR_CARD));
    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(speaker_x, speaker_y, card_w, card_h, SIDEKICK_COLOR_SELECTED));
    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(camera_x, camera_y, card_w, card_h, SIDEKICK_COLOR_ACCENT));

    sidekick_ui_draw_home_icon(home_x + home_size / 8, home_y + home_size / 8, home_size * 3 / 4,
                               SIDEKICK_COLOR_ACCENT);
    sidekick_ui_draw_speaker_icon(speaker_x + ((card_w - icon_size) / 2), speaker_y + ((card_h - icon_size) / 2),
                                  icon_size, SIDEKICK_COLOR_BG);
    sidekick_ui_draw_camera_icon(camera_x + ((card_w - icon_size) / 2), camera_y + ((card_h - icon_size) / 2),
                                 icon_size, SIDEKICK_COLOR_BG);

    return sidekick_ui_present();
}

static OPERATE_RET sidekick_ui_draw_screen(void)
{
    if (s_screen == SIDEKICK_UI_SCREEN_KICK) {
        return sidekick_ui_draw_kick_screen();
    }

    if (s_screen == SIDEKICK_UI_SCREEN_SETTINGS) {
        return sidekick_ui_draw_settings_screen();
    }

    if (s_screen == SIDEKICK_UI_SCREEN_TESTS) {
        return sidekick_ui_draw_tests_screen();
    }

    return sidekick_ui_draw_home_screen();
}

static uint32_t sidekick_ui_frame_len(const TDL_DISP_DEV_INFO_T *info)
{
    uint8_t  bpp = tdl_disp_get_fmt_bpp(info->fmt);
    uint32_t bytes_per_pixel;
    uint32_t pixels_per_byte;

    if (bpp == 0) {
        return 0;
    }

    if (bpp < 8) {
        pixels_per_byte = 8 / bpp;
        return ((info->width + pixels_per_byte - 1) / pixels_per_byte) * info->height;
    }

    bytes_per_pixel = (bpp + 7) / 8;
    return info->width * info->height * bytes_per_pixel;
}

static OPERATE_RET sidekick_ui_display_start(void)
{
    OPERATE_RET rt        = OPRT_OK;
    uint32_t    frame_len = 0;

    s_display_handle = tdl_disp_find_dev(DISPLAY_NAME);
    if (s_display_handle == NULL) {
        SIDEKICK_LOGW("ui", "display device %s not found", DISPLAY_NAME);
        return OPRT_OK;
    }

    TUYA_CALL_ERR_RETURN(tdl_disp_dev_get_info(s_display_handle, &s_display_info));
    TUYA_CALL_ERR_RETURN(tdl_disp_dev_open(s_display_handle));
    TUYA_CALL_ERR_RETURN(tdl_disp_set_brightness(s_display_handle, 100));

    frame_len = sidekick_ui_frame_len(&s_display_info);
    if (frame_len == 0) {
        SIDEKICK_LOGW("ui", "unsupported display pixel format=%d", s_display_info.fmt);
        return OPRT_OK;
    }

    for (uint8_t i = 0; i < 2; i++) {
        s_display_fb[i] = tdl_disp_create_frame_buff(DISP_FB_TP_PSRAM, frame_len);
        if (s_display_fb[i] == NULL) {
            SIDEKICK_LOGW("ui", "display frame buffer %u allocation failed", i);
            return OPRT_OK;
        }

        sidekick_ui_setup_fb(s_display_fb[i]);
    }

    s_draw_fb_index = 0;
    s_rotate_canvas = s_display_info.height > s_display_info.width;
    s_canvas_width  = s_rotate_canvas ? s_display_info.height : s_display_info.width;
    s_canvas_height = s_rotate_canvas ? s_display_info.width : s_display_info.height;

    TUYA_CALL_ERR_RETURN(sidekick_ui_draw_screen());
    SIDEKICK_LOGI("ui", "SideKick home screen displayed %ux%u", s_display_info.width, s_display_info.height);
    return OPRT_OK;
}
#endif

#if defined(ENABLE_TP) && (ENABLE_TP == 1)
#include "tdl_tp_manage.h"

#define SIDEKICK_UI_MAX_TOUCH_POINTS 2

static TDL_TP_HANDLE_T s_tp_handle              = NULL;
static bool            s_touch_down             = false;
static bool            s_touch_armed            = false;
static SYS_TICK_T      s_session_end_allowed_ms = 0;
#endif

OPERATE_RET sidekick_ui_start(void)
{
    OPERATE_RET rt = OPRT_OK;

#if defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
    TUYA_CALL_ERR_LOG(sidekick_ui_display_start());
#else
    SIDEKICK_LOGW("ui", "display support not enabled in config");
#endif

#if defined(ENABLE_TP) && (ENABLE_TP == 1)
    s_tp_handle = tdl_tp_find_dev(DISPLAY_NAME);
    if (s_tp_handle == NULL) {
        SIDEKICK_LOGW("ui", "touch panel %s not found; mode switch disabled", DISPLAY_NAME);
        return OPRT_OK;
    }

    TUYA_CALL_ERR_RETURN(tdl_tp_dev_open(s_tp_handle));
    SIDEKICK_LOGI("ui", "touch mode switch enabled");
#else
    SIDEKICK_LOGW("ui", "touch panel support not enabled in config");
#endif

    return rt;
}

void sidekick_ui_poll(void)
{
#if defined(ENABLE_TP) && (ENABLE_TP == 1)
    OPERATE_RET  rt = OPRT_OK;
    TDL_TP_POS_T points[SIDEKICK_UI_MAX_TOUCH_POINTS];
    uint8_t      point_count = 0;

    if (s_tp_handle == NULL) {
        return;
    }

    rt = tdl_tp_dev_read(s_tp_handle, SIDEKICK_UI_MAX_TOUCH_POINTS, points, &point_count);
    if (rt != OPRT_OK) {
        SIDEKICK_LOGW("ui", "touch read failed rt=%d", rt);
        return;
    }

    if (point_count == 0) {
        s_touch_down  = false;
        s_touch_armed = true;
        return;
    }

    if (!s_touch_armed) {
        return;
    }

    if (s_touch_down) {
        return;
    }

    s_touch_down = true;
    if (s_screen == SIDEKICK_UI_SCREEN_HOME) {
#if defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
        uint16_t canvas_x = 0;
        uint16_t canvas_y = 0;
        uint16_t kick_x   = 0;
        uint16_t test_x   = 0;
        uint16_t button_y = 0;
        uint16_t button_w = 0;
        uint16_t button_h = 0;

        sidekick_ui_map_touch(points[0].x, points[0].y, &canvas_x, &canvas_y);
        sidekick_ui_home_actions_layout(&kick_x, &test_x, &button_y, &button_w, &button_h);

        if (sidekick_ui_point_in_rect(canvas_x, canvas_y, kick_x, button_y, button_w, button_h)) {
            SIDEKICK_LOGI("ui", "kick");
            s_screen = SIDEKICK_UI_SCREEN_KICK;
            TUYA_CALL_ERR_LOG(sidekick_ui_draw_screen());
            return;
        }

        if (sidekick_ui_point_in_rect(canvas_x, canvas_y, test_x, button_y, button_w, button_h)) {
            SIDEKICK_LOGI("ui", "test menu");
            s_screen = SIDEKICK_UI_SCREEN_TESTS;
            TUYA_CALL_ERR_LOG(sidekick_ui_draw_screen());
            return;
        }
#endif
        return;
    }

    if (s_screen == SIDEKICK_UI_SCREEN_CAMERA) {
        SIDEKICK_LOGI("ui", "touch x=%d y=%d; stopping camera test", points[0].x, points[0].y);
        TUYA_CALL_ERR_LOG(sidekick_camera_preview_stop());
        s_screen = SIDEKICK_UI_SCREEN_TESTS;
#if defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
        TUYA_CALL_ERR_LOG(sidekick_ui_draw_screen());
#endif
        return;
    }

#if defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
    uint16_t canvas_x  = 0;
    uint16_t canvas_y  = 0;
    uint16_t home_x    = 0;
    uint16_t home_y    = 0;
    uint16_t home_size = 0;
    uint16_t speaker_x = 0;
    uint16_t speaker_y = 0;
    uint16_t camera_x  = 0;
    uint16_t camera_y  = 0;
    uint16_t card_w    = 0;
    uint16_t card_h    = 0;

    sidekick_ui_map_touch(points[0].x, points[0].y, &canvas_x, &canvas_y);
    sidekick_ui_tests_layout(&home_x, &home_y, &home_size, &speaker_x, &speaker_y, &camera_x, &camera_y, &card_w,
                             &card_h);

    if (s_screen == SIDEKICK_UI_SCREEN_KICK) {
        uint16_t start_x;
        uint16_t start_y;
        uint16_t start_w;
        uint16_t start_h;
        uint16_t settings_x;
        uint16_t settings_y;
        uint16_t settings_size;
        uint16_t mode_x;
        uint16_t mode_y;
        uint16_t mode_w;
        uint16_t mode_h;
        uint16_t mode_gap;
        uint16_t row_y;

        if (sidekick_session_is_active()) {
            uint16_t end_x;
            uint16_t end_y;
            uint16_t end_w;
            uint16_t end_h;
            uint16_t preview_x;
            uint16_t preview_y;
            uint16_t preview_w;
            uint16_t preview_h;

            sidekick_ui_kick_active_layout(&end_x, &end_y, &end_w, &end_h, &preview_x, &preview_y, &preview_w,
                                           &preview_h);
            if (sidekick_ui_point_in_rect(canvas_x, canvas_y, end_x, end_y, end_w, end_h)) {
                if (tal_time_get_posix_ms() < s_session_end_allowed_ms) {
                    SIDEKICK_LOGI("ui", "session end ignored (release finger and wait a moment)");
                    return;
                }
                SIDEKICK_LOGI("ui", "session end");
                TUYA_CALL_ERR_LOG(sidekick_camera_preview_stop());
                sidekick_session_end();
                TUYA_CALL_ERR_LOG(sidekick_ui_draw_screen());
                return;
            }
            return;
        }

        sidekick_ui_kick_layout(&home_x, &home_y, &home_size, &start_x, &start_y, &start_w, &start_h, &mode_x, &mode_y,
                                &mode_w, &mode_h, &mode_gap);
        sidekick_ui_settings_button_layout(&settings_x, &settings_y, &settings_size);

        if (sidekick_ui_point_in_rect(canvas_x, canvas_y, home_x, home_y, home_size, home_size)) {
            SIDEKICK_LOGI("ui", "home");
            s_screen = SIDEKICK_UI_SCREEN_HOME;
            TUYA_CALL_ERR_LOG(sidekick_ui_draw_screen());
            return;
        }

        if (sidekick_ui_point_in_rect(canvas_x, canvas_y, settings_x, settings_y, settings_size, settings_size)) {
            SIDEKICK_LOGI("ui", "settings");
            s_screen = SIDEKICK_UI_SCREEN_SETTINGS;
            TUYA_CALL_ERR_LOG(sidekick_ui_draw_screen());
            return;
        }

        if (sidekick_ui_point_in_rect(canvas_x, canvas_y, start_x, start_y, start_w, start_h)) {
            SIDEKICK_LOGI("ui", "session start");
            sidekick_session_start();
            s_session_end_allowed_ms = tal_time_get_posix_ms() + SIDEKICK_SESSION_END_GRACE_MS;
            s_touch_down             = false;
            s_touch_armed            = false;
            TUYA_CALL_ERR_LOG(sidekick_ui_draw_screen());
            TUYA_CALL_ERR_LOG(sidekick_ui_start_session_preview());
            return;
        }

        row_y = mode_y;
        for (SIDEKICK_TUTOR_MODE_E mode = SIDEKICK_TUTOR_MODE_ACTIVE; mode <= SIDEKICK_TUTOR_MODE_SUMMARY; mode++) {
            if (sidekick_ui_point_in_rect(canvas_x, canvas_y, mode_x, row_y, mode_w, mode_h)) {
                if (mode != sidekick_session_mode()) {
                    sidekick_session_set_mode(mode);
                    SIDEKICK_LOGI("ui", "mode=%s", sidekick_session_mode_name(mode));
                    TUYA_CALL_ERR_LOG(sidekick_ui_draw_kick_screen());
                }
                return;
            }
            row_y += mode_h + mode_gap;
        }
        return;
    }

    if (s_screen == SIDEKICK_UI_SCREEN_SETTINGS) {
        uint16_t back_x;
        uint16_t back_y;
        uint16_t back_size;
        uint16_t option_x;
        uint16_t option_y;
        uint16_t option_w;
        uint16_t option_h;
        uint16_t option_gap;

        sidekick_ui_settings_layout(&back_x, &back_y, &back_size, &home_x, &home_y, &home_size, &option_x, &option_y,
                                    &option_w, &option_h, &option_gap);

        if (sidekick_ui_point_in_rect(canvas_x, canvas_y, back_x, back_y, back_size, back_size)) {
            SIDEKICK_LOGI("ui", "settings back");
            s_screen = SIDEKICK_UI_SCREEN_KICK;
            TUYA_CALL_ERR_LOG(sidekick_ui_draw_screen());
            return;
        }

        if (sidekick_ui_point_in_rect(canvas_x, canvas_y, home_x, home_y, home_size, home_size)) {
            SIDEKICK_LOGI("ui", "home");
            s_screen = SIDEKICK_UI_SCREEN_HOME;
            TUYA_CALL_ERR_LOG(sidekick_ui_draw_screen());
            return;
        }

        for (uint8_t i = 0; i < SIDEKICK_INTERVAL_COUNT; i++) {
            uint16_t row    = i / 2;
            uint16_t col    = i % 2;
            uint16_t rect_x = option_x + col * (option_w + option_gap);
            uint16_t rect_y = option_y + row * (option_h + option_gap);

            if (sidekick_ui_point_in_rect(canvas_x, canvas_y, rect_x, rect_y, option_w, option_h)) {
                sidekick_session_set_frame_interval_sec(s_interval_options[i]);
                SIDEKICK_LOGI("ui", "frame interval=%us", (unsigned int)s_interval_options[i]);
                TUYA_CALL_ERR_LOG(sidekick_ui_draw_settings_screen());
                return;
            }
        }

        return;
    }

    if (sidekick_ui_point_in_rect(canvas_x, canvas_y, home_x, home_y, home_size, home_size)) {
        SIDEKICK_LOGI("ui", "home");
        s_screen = SIDEKICK_UI_SCREEN_HOME;
        TUYA_CALL_ERR_LOG(sidekick_ui_draw_screen());
        return;
    }

    if (sidekick_ui_point_in_rect(canvas_x, canvas_y, speaker_x, speaker_y, card_w, card_h)) {
        SIDEKICK_LOGI("ui", "speaker test");
        TUYA_CALL_ERR_LOG(sidekick_audio_play_startup_chime());
        return;
    }

    if (sidekick_ui_point_in_rect(canvas_x, canvas_y, camera_x, camera_y, card_w, card_h)) {
        SIDEKICK_LOGI("ui", "camera test");
        s_screen = SIDEKICK_UI_SCREEN_CAMERA;
        TUYA_CALL_ERR_LOG(sidekick_camera_preview_start());
        return;
    }
#endif
#endif
}
