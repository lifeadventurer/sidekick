/**
 * @file sidekick_session.c
 * @brief SideKick tutoring session state and capture cadence.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 *
 */
#include "sidekick_session.h"

#include "sidekick_audio.h"
#include "sidekick_backend.h"
#include "sidekick_config.h"
#include "sidekick_log.h"

static SIDEKICK_SESSION_STATE_E s_state      = SIDEKICK_SESSION_IDLE;
static SIDEKICK_TUTOR_MODE_E    s_mode       = SIDEKICK_DEFAULT_TUTOR_MODE;
static uint32_t                 s_tick_count = 0;

OPERATE_RET sidekick_session_init(void)
{
    s_state      = SIDEKICK_SESSION_IDLE;
    s_mode       = SIDEKICK_DEFAULT_TUTOR_MODE;
    s_tick_count = 0;
    SIDEKICK_LOGI("tutor", "session initialized mode=%s", sidekick_session_mode_name(s_mode));
    return OPRT_OK;
}

void sidekick_session_tick(void)
{
    if (s_state == SIDEKICK_SESSION_IDLE) {
        return;
    }

    s_tick_count++;

    if ((s_tick_count % 5) == 0) {
        SIDEKICK_LOGI("tutor", "mode=%s state=%d audio_frames=%u", sidekick_session_mode_name(s_mode), (int)s_state,
                      (unsigned int)sidekick_audio_frame_count());
    }

    if ((s_tick_count % SIDEKICK_FRAME_UPLOAD_INTERVAL_SEC) == 0) {
        sidekick_backend_request_frame();
    }
}

SIDEKICK_SESSION_STATE_E sidekick_session_state(void)
{
    return s_state;
}

bool sidekick_session_is_active(void)
{
    return s_state != SIDEKICK_SESSION_IDLE;
}

SIDEKICK_TUTOR_MODE_E sidekick_session_mode(void)
{
    return s_mode;
}

const char *sidekick_session_mode_name(SIDEKICK_TUTOR_MODE_E mode)
{
    switch (mode) {
    case SIDEKICK_TUTOR_MODE_ACTIVE:
        return "active";
    case SIDEKICK_TUTOR_MODE_HINT:
        return "hint";
    case SIDEKICK_TUTOR_MODE_SUMMARY:
        return "summary";
    default:
        return "unknown";
    }
}

void sidekick_session_start(void)
{
    if (sidekick_session_is_active()) {
        return;
    }

    if (s_mode == SIDEKICK_TUTOR_MODE_SUMMARY) {
        s_mode = SIDEKICK_TUTOR_MODE_HINT;
        SIDEKICK_LOGI("tutor", "summary mode is for session end only; using hint");
    }

    s_state = SIDEKICK_SESSION_OBSERVING;
    if (SIDEKICK_FRAME_UPLOAD_INTERVAL_SEC > 1) {
        s_tick_count = SIDEKICK_FRAME_UPLOAD_INTERVAL_SEC - 1;
    } else {
        s_tick_count = 0;
    }
    SIDEKICK_LOGI("tutor", "session started mode=%s", sidekick_session_mode_name(s_mode));
}

void sidekick_session_end(void)
{
    if (!sidekick_session_is_active()) {
        return;
    }

    s_state      = SIDEKICK_SESSION_IDLE;
    s_tick_count = 0;
    SIDEKICK_LOGI("tutor", "session ended");
    sidekick_backend_end_session();
}

void sidekick_session_set_mode(SIDEKICK_TUTOR_MODE_E mode)
{
    if ((mode < SIDEKICK_TUTOR_MODE_ACTIVE) || (mode > SIDEKICK_TUTOR_MODE_SUMMARY)) {
        SIDEKICK_LOGW("tutor", "ignore invalid mode=%d", (int)mode);
        return;
    }

    if (s_mode == mode) {
        return;
    }

    s_mode = mode;
    SIDEKICK_LOGI("tutor", "mode changed to %s", sidekick_session_mode_name(s_mode));
}

void sidekick_session_next_mode(void)
{
    SIDEKICK_TUTOR_MODE_E next = SIDEKICK_TUTOR_MODE_HINT;

    switch (s_mode) {
    case SIDEKICK_TUTOR_MODE_ACTIVE:
        next = SIDEKICK_TUTOR_MODE_HINT;
        break;
    case SIDEKICK_TUTOR_MODE_HINT:
        next = SIDEKICK_TUTOR_MODE_SUMMARY;
        break;
    case SIDEKICK_TUTOR_MODE_SUMMARY:
    default:
        next = SIDEKICK_TUTOR_MODE_ACTIVE;
        break;
    }

    sidekick_session_set_mode(next);
}
