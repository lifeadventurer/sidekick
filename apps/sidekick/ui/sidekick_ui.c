#include "sidekick_ui.h"

#include "sidekick_log.h"
#include "sidekick_session.h"

#if defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
#include "tdl_display_manage.h"

#define SIDEKICK_COLOR_BG         sidekick_ui_color(0x10, 0x18, 0x28)
#define SIDEKICK_COLOR_CARD       sidekick_ui_color(0x1D, 0x29, 0x39)
#define SIDEKICK_COLOR_ACCENT     sidekick_ui_color(0x84, 0xCA, 0xFF)
#define SIDEKICK_COLOR_DIM        sidekick_ui_color(0x47, 0x55, 0x67)
#define SIDEKICK_COLOR_SELECTED   sidekick_ui_color(0xB7, 0xE4, 0xC7)
#define SIDEKICK_WORDMARK_LETTERS 8
#define SIDEKICK_KICK_LETTERS     4
#define SIDEKICK_GLYPH_WIDTH      5
#define SIDEKICK_GLYPH_SPACING    1
#define SIDEKICK_GLYPH_HEIGHT     7

typedef enum {
    SIDEKICK_UI_SCREEN_HOME = 0,
    SIDEKICK_UI_SCREEN_MODE,
} SIDEKICK_UI_SCREEN_E;

static TDL_DISP_HANDLE_T      s_display_handle = NULL;
static TDL_DISP_DEV_INFO_T    s_display_info;
static TDL_DISP_FRAME_BUFF_T *s_display_fb    = NULL;
static uint16_t               s_canvas_width  = 0;
static uint16_t               s_canvas_height = 0;
static bool                   s_rotate_canvas = false;
static bool                   s_flip_canvas   = true;
static SIDEKICK_UI_SCREEN_E   s_screen        = SIDEKICK_UI_SCREEN_HOME;
static SIDEKICK_TUTOR_MODE_E  s_drawn_mode    = SIDEKICK_TUTOR_MODE_HINT;

static uint32_t sidekick_ui_color(uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_display_info.fmt == TUYA_PIXEL_FMT_RGB888) {
        return ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
    }

    return (((uint32_t)red & 0xF8) << 8) | (((uint32_t)green & 0xFC) << 3) | ((uint32_t)blue >> 3);
}

static OPERATE_RET sidekick_ui_fill_rect_raw(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint32_t color)
{
    if ((s_display_fb == NULL) || (width == 0) || (height == 0)) {
        return OPRT_OK;
    }

    if ((x >= s_display_fb->width) || (y >= s_display_fb->height)) {
        return OPRT_OK;
    }

    if ((x + width) > s_display_fb->width) {
        width = s_display_fb->width - x;
    }

    if ((y + height) > s_display_fb->height) {
        height = s_display_fb->height - y;
    }

    TDL_DISP_RECT_T rect = {
        .x0 = x,
        .y0 = y,
        .x1 = x + width - 1,
        .y1 = y + height - 1,
    };

    return tdl_disp_draw_fill(s_display_fb, &rect, color, s_display_info.is_swap);
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
    static const uint8_t glyph_c[7] = {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F};
    static const uint8_t glyph_d[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    static const uint8_t glyph_e[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    static const uint8_t glyph_i[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    static const uint8_t glyph_k[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    static const uint8_t glyph_s[7] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};

    switch (letter) {
    case 'C':
        return glyph_c;
    case 'D':
        return glyph_d;
    case 'E':
        return glyph_e;
    case 'I':
        return glyph_i;
    case 'K':
        return glyph_k;
    case 'S':
        return glyph_s;
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
        sidekick_ui_draw_block_letter(*text, x, y, unit, color);
        x += unit * 6;
        text++;
    }
}

static uint16_t sidekick_ui_text_width(uint16_t letter_count, uint16_t unit)
{
    uint16_t text_units = letter_count * SIDEKICK_GLYPH_WIDTH + (letter_count - 1) * SIDEKICK_GLYPH_SPACING;

    return text_units * unit;
}

static uint32_t sidekick_ui_mode_color(SIDEKICK_TUTOR_MODE_E mode)
{
    return (mode == sidekick_session_mode()) ? SIDEKICK_COLOR_SELECTED : SIDEKICK_COLOR_DIM;
}

static OPERATE_RET sidekick_ui_draw_home_screen(void)
{
    OPERATE_RET rt       = OPRT_OK;
    uint16_t    width    = s_canvas_width;
    uint16_t    height   = s_canvas_height;
    uint16_t    unit     = height / 28;
    uint16_t    max_unit = (width > 16) ? ((width - 16) / (SIDEKICK_WORDMARK_LETTERS * SIDEKICK_GLYPH_WIDTH +
                                                        (SIDEKICK_WORDMARK_LETTERS - 1) * SIDEKICK_GLYPH_SPACING))
                                        : 1;
    uint16_t    word_w;
    uint16_t    word_x;
    uint16_t    word_y;
    uint16_t    kick_unit;
    uint16_t    kick_w;
    uint16_t    kick_x;
    uint16_t    kick_y;
    uint16_t    button_x = width / 4;
    uint16_t    button_y = height * 58 / 100;
    uint16_t    button_w = width / 2;
    uint16_t    button_h = height / 4;
    uint16_t    card_x   = width / 12;
    uint16_t    card_y   = height / 10;
    uint16_t    card_w   = width - (card_x * 2);
    uint16_t    card_h   = height - (card_y * 2);

    if ((max_unit > 0) && (unit > max_unit)) {
        unit = max_unit;
    }

    if (unit < 3) {
        unit = 3;
    }

    word_w = sidekick_ui_text_width(SIDEKICK_WORDMARK_LETTERS, unit);
    word_x = (width > word_w) ? ((width - word_w) / 2) : unit;
    word_y = card_y + unit * 2;

    kick_unit = (button_h / (SIDEKICK_GLYPH_HEIGHT + 2));
    kick_w    = sidekick_ui_text_width(SIDEKICK_KICK_LETTERS, kick_unit);
    if (kick_w > (button_w - unit * 2)) {
        kick_unit = (button_w - unit * 2) / (SIDEKICK_KICK_LETTERS * SIDEKICK_GLYPH_WIDTH +
                                             (SIDEKICK_KICK_LETTERS - 1) * SIDEKICK_GLYPH_SPACING);
        kick_w    = sidekick_ui_text_width(SIDEKICK_KICK_LETTERS, kick_unit);
    }

    if (kick_unit < 3) {
        kick_unit = 3;
        kick_w    = sidekick_ui_text_width(SIDEKICK_KICK_LETTERS, kick_unit);
    }

    kick_x = button_x + ((button_w > kick_w) ? ((button_w - kick_w) / 2) : unit);
    kick_y =
        button_y +
        ((button_h > kick_unit * SIDEKICK_GLYPH_HEIGHT) ? ((button_h - kick_unit * SIDEKICK_GLYPH_HEIGHT) / 2) : unit);

    TUYA_CALL_ERR_RETURN(tdl_disp_draw_fill_full(s_display_fb, SIDEKICK_COLOR_BG, s_display_info.is_swap));
    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(card_x, card_y, card_w, card_h, SIDEKICK_COLOR_CARD));
    sidekick_ui_draw_block_text("SIDEKICK", word_x, word_y, unit, SIDEKICK_COLOR_ACCENT);

    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(button_x, button_y, button_w, button_h, SIDEKICK_COLOR_SELECTED));
    sidekick_ui_draw_block_text("KICK", kick_x, kick_y, kick_unit, SIDEKICK_COLOR_BG);

    TUYA_CALL_ERR_RETURN(tdl_disp_dev_flush(s_display_handle, s_display_fb));
    s_drawn_mode = sidekick_session_mode();
    return OPRT_OK;
}

static OPERATE_RET sidekick_ui_draw_mode_screen(void)
{
    OPERATE_RET rt       = OPRT_OK;
    uint16_t    width    = s_canvas_width;
    uint16_t    height   = s_canvas_height;
    uint16_t    unit     = height / 28;
    uint16_t    max_unit = (width > 16) ? ((width - 16) / (SIDEKICK_WORDMARK_LETTERS * SIDEKICK_GLYPH_WIDTH +
                                                        (SIDEKICK_WORDMARK_LETTERS - 1) * SIDEKICK_GLYPH_SPACING))
                                        : 1;
    uint16_t    word_w;
    uint16_t    word_x;
    uint16_t    word_y;
    uint16_t    card_x = width / 12;
    uint16_t    card_y = height / 10;
    uint16_t    card_w = width - (card_x * 2);
    uint16_t    card_h = height - (card_y * 2);
    uint16_t    bar_y;
    uint16_t    bar_w;

    if ((max_unit > 0) && (unit > max_unit)) {
        unit = max_unit;
    }

    if (unit < 3) {
        unit = 3;
    }

    word_w = sidekick_ui_text_width(SIDEKICK_WORDMARK_LETTERS, unit);
    word_x = (width > word_w) ? ((width - word_w) / 2) : unit;
    word_y = card_y + unit * 2;
    bar_y  = card_y + card_h - (unit * 4);
    bar_w  = (card_w - (unit * 4)) / 3;

    TUYA_CALL_ERR_RETURN(tdl_disp_draw_fill_full(s_display_fb, SIDEKICK_COLOR_BG, s_display_info.is_swap));
    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(card_x, card_y, card_w, card_h, SIDEKICK_COLOR_CARD));
    sidekick_ui_draw_block_text("SIDEKICK", word_x, word_y, unit, SIDEKICK_COLOR_ACCENT);

    TUYA_CALL_ERR_RETURN(
        sidekick_ui_fill_rect(card_x + unit, bar_y, bar_w, unit, sidekick_ui_mode_color(SIDEKICK_TUTOR_MODE_ACTIVE)));
    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(card_x + unit * 2 + bar_w, bar_y, bar_w, unit,
                                               sidekick_ui_mode_color(SIDEKICK_TUTOR_MODE_HINT)));
    TUYA_CALL_ERR_RETURN(sidekick_ui_fill_rect(card_x + unit * 3 + bar_w * 2, bar_y, bar_w, unit,
                                               sidekick_ui_mode_color(SIDEKICK_TUTOR_MODE_SUMMARY)));

    TUYA_CALL_ERR_RETURN(tdl_disp_dev_flush(s_display_handle, s_display_fb));
    s_drawn_mode = sidekick_session_mode();
    return OPRT_OK;
}

static OPERATE_RET sidekick_ui_draw_screen(void)
{
    if (s_screen == SIDEKICK_UI_SCREEN_MODE) {
        return sidekick_ui_draw_mode_screen();
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

    s_display_fb = tdl_disp_create_frame_buff(DISP_FB_TP_PSRAM, frame_len);
    if (s_display_fb == NULL) {
        SIDEKICK_LOGW("ui", "display frame buffer allocation failed");
        return OPRT_OK;
    }

    s_display_fb->x_start = 0;
    s_display_fb->y_start = 0;
    s_display_fb->fmt     = s_display_info.fmt;
    s_display_fb->width   = s_display_info.width;
    s_display_fb->height  = s_display_info.height;
    s_rotate_canvas       = s_display_info.height > s_display_info.width;
    s_canvas_width        = s_rotate_canvas ? s_display_info.height : s_display_info.width;
    s_canvas_height       = s_rotate_canvas ? s_display_info.width : s_display_info.height;

    TUYA_CALL_ERR_RETURN(sidekick_ui_draw_screen());
    SIDEKICK_LOGI("ui", "SideKick home screen displayed %ux%u", s_display_info.width, s_display_info.height);
    return OPRT_OK;
}
#endif

#if defined(ENABLE_TP) && (ENABLE_TP == 1)
#include "tdl_tp_manage.h"

#define SIDEKICK_UI_MAX_TOUCH_POINTS 2

static TDL_TP_HANDLE_T s_tp_handle  = NULL;
static bool            s_touch_down = false;
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
        s_touch_down = false;
        return;
    }

    if (s_touch_down) {
        return;
    }

    s_touch_down = true;
    if (s_screen == SIDEKICK_UI_SCREEN_HOME) {
        SIDEKICK_LOGI("ui", "touch x=%d y=%d; starting SideKick", points[0].x, points[0].y);
        s_screen = SIDEKICK_UI_SCREEN_MODE;
        sidekick_session_set_mode(SIDEKICK_TUTOR_MODE_ACTIVE);
    } else {
        SIDEKICK_LOGI("ui", "touch x=%d y=%d; cycling tutor mode", points[0].x, points[0].y);
        sidekick_session_next_mode();
    }
#if defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
    TUYA_CALL_ERR_LOG(sidekick_ui_draw_screen());
#endif
#elif defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
    if ((s_screen == SIDEKICK_UI_SCREEN_MODE) && (s_drawn_mode != sidekick_session_mode())) {
        TUYA_CALL_ERR_LOG(sidekick_ui_draw_screen());
    }
#endif
}
