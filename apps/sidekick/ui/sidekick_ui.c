#include "sidekick_ui.h"

#include "sidekick_log.h"
#include "sidekick_session.h"

#if defined(ENABLE_LIBLVGL) && (ENABLE_LIBLVGL == 1)
#include "lv_vendor.h"
#include "lvgl.h"

static lv_obj_t              *s_mode_label = NULL;
static SIDEKICK_TUTOR_MODE_E  s_drawn_mode = SIDEKICK_TUTOR_MODE_HINT;

static void sidekick_ui_update_mode_label(void)
{
    SIDEKICK_TUTOR_MODE_E mode = sidekick_session_mode();

    if (s_mode_label == NULL) {
        return;
    }

    lv_label_set_text_fmt(s_mode_label, "Mode: %s", sidekick_session_mode_name(mode));
    s_drawn_mode = mode;
}

static void sidekick_ui_touch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    sidekick_session_next_mode();
    sidekick_ui_update_mode_label();
}

static void sidekick_ui_build_home_screen(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101828), LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, lv_color_white(), LV_PART_MAIN);

    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, LV_PCT(86), LV_PCT(78));
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 28, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1D2939), LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, sidekick_ui_touch_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *logo = lv_label_create(card);
    lv_label_set_text(logo, "SK");
    lv_obj_set_style_text_color(logo, lv_color_hex(0x84CAFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(logo, &lv_font_montserrat_48, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "SideKick");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, LV_PART_MAIN);

    lv_obj_t *subtitle = lv_label_create(card);
    lv_label_set_text(subtitle, "AI tutor ready");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0xD0D5DD), LV_PART_MAIN);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_20, LV_PART_MAIN);

    s_mode_label = lv_label_create(card);
    lv_obj_set_style_text_color(s_mode_label, lv_color_hex(0xB7E4C7), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_mode_label, &lv_font_montserrat_20, LV_PART_MAIN);
    sidekick_ui_update_mode_label();

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "Tap to switch Active / Hint / Summary");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x98A2B3), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_pad_top(hint, 16, LV_PART_MAIN);
}
#elif defined(ENABLE_TP) && (ENABLE_TP == 1)
#include "tdl_tp_manage.h"

#define SIDEKICK_UI_MAX_TOUCH_POINTS 2

static TDL_TP_HANDLE_T s_tp_handle  = NULL;
static bool            s_touch_down = false;
#endif

OPERATE_RET sidekick_ui_start(void)
{
#if defined(ENABLE_LIBLVGL) && (ENABLE_LIBLVGL == 1)
    lv_vendor_init(DISPLAY_NAME);
    lv_vendor_start(5, 1024 * 8);

    lv_vendor_disp_lock();
    sidekick_ui_build_home_screen();
    lv_vendor_disp_unlock();

    SIDEKICK_LOGI("ui", "SideKick home screen displayed");
    return OPRT_OK;
#elif defined(ENABLE_TP) && (ENABLE_TP == 1)
    OPERATE_RET rt = OPRT_OK;

    s_tp_handle = tdl_tp_find_dev(DISPLAY_NAME);
    if (s_tp_handle == NULL) {
        SIDEKICK_LOGW("ui", "touch panel %s not found; mode switch disabled", DISPLAY_NAME);
        return OPRT_OK;
    }

    TUYA_CALL_ERR_RETURN(tdl_tp_dev_open(s_tp_handle));
    SIDEKICK_LOGI("ui", "touch mode switch enabled");
    return OPRT_OK;
#else
    SIDEKICK_LOGW("ui", "touch panel support not enabled in config");
    return OPRT_OK;
#endif
}

void sidekick_ui_poll(void)
{
#if defined(ENABLE_LIBLVGL) && (ENABLE_LIBLVGL == 1)
    if (s_drawn_mode == sidekick_session_mode()) {
        return;
    }

    lv_vendor_disp_lock();
    sidekick_ui_update_mode_label();
    lv_vendor_disp_unlock();
#elif defined(ENABLE_TP) && (ENABLE_TP == 1)
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
    SIDEKICK_LOGI("ui", "touch x=%d y=%d; cycling tutor mode", points[0].x, points[0].y);
    sidekick_session_next_mode();
#endif
}
