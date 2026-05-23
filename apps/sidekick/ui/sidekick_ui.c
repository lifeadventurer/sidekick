#include "sidekick_ui.h"

#include "sidekick_log.h"
#include "sidekick_session.h"

#if defined(ENABLE_TP) && (ENABLE_TP == 1)
#include "tdl_tp_manage.h"

#define SIDEKICK_UI_MAX_TOUCH_POINTS 2

static TDL_TP_HANDLE_T s_tp_handle  = NULL;
static bool            s_touch_down = false;
#endif

OPERATE_RET sidekick_ui_start(void)
{
#if defined(ENABLE_TP) && (ENABLE_TP == 1)
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
    SIDEKICK_LOGI("ui", "touch x=%d y=%d; cycling tutor mode", points[0].x, points[0].y);
    sidekick_session_next_mode();
#endif
}
