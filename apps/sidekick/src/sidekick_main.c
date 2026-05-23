#include "sidekick_audio.h"
#include "sidekick_camera.h"
#include "sidekick_config.h"
#include "sidekick_hardware.h"
#include "sidekick_log.h"
#include "sidekick_session.h"
#include "sidekick_ui.h"

#include "tal_api.h"
#include "tkl_output.h"

static THREAD_HANDLE s_sidekick_thread = NULL;

static void sidekick_user_main(void)
{
    OPERATE_RET rt = OPRT_OK;

    (void)tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    SIDEKICK_LOGI("main", "%s boot", SIDEKICK_APP_NAME);
    SIDEKICK_LOGI("main", "project=%s version=%s", PROJECT_NAME, PROJECT_VERSION);
    SIDEKICK_LOGI("main", "platform=%s board=%s", PLATFORM_CHIP, PLATFORM_BOARD);

    TUYA_CALL_ERR_LOG(sidekick_hardware_init());
    TUYA_CALL_ERR_LOG(sidekick_camera_preview_start());
    TUYA_CALL_ERR_LOG(sidekick_audio_input_start());
    TUYA_CALL_ERR_LOG(sidekick_session_init());
    TUYA_CALL_ERR_LOG(sidekick_ui_start());

    while (1) {
        sidekick_ui_poll();
        sidekick_session_tick();
        tal_system_sleep(SIDEKICK_TUTOR_TICK_MS);
    }
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    sidekick_user_main();
}
#else
static void sidekick_thread_entry(void *arg)
{
    (void)arg;
    sidekick_user_main();
    tal_thread_delete(s_sidekick_thread);
    s_sidekick_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thread_cfg = {0};
    thread_cfg.stackDepth   = 1024 * 6;
    thread_cfg.priority     = THREAD_PRIO_1;
    thread_cfg.thrdname     = "sidekick";
    tal_thread_create_and_start(&s_sidekick_thread, NULL, NULL, sidekick_thread_entry, NULL, &thread_cfg);
}
#endif
