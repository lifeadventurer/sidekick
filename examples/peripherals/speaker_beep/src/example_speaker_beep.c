/**
 * @file example_speaker_beep.c
 * @brief T5AI speaker beep validation using the board audio driver.
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "tuya_cloud_types.h"

#include "tal_api.h"
#include "tkl_output.h"

#include "board_com_api.h"
#include "tdl_audio_manage.h"

#define SPEAKER_BEEP_FRAME_SAMPLES 320
#define SPEAKER_BEEP_REPEAT_FRAMES 25
#define SPEAKER_BEEP_PAUSE_MS      1000

static TDL_AUDIO_HANDLE_T sg_audio_hdl = NULL;

static void __speaker_beep_audio_frame_cb(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status, uint8_t *data,
                                          uint32_t len)
{
    (void)type;
    (void)status;
    (void)data;
    (void)len;
}

static OPERATE_RET __speaker_beep_audio_open(void)
{
    OPERATE_RET rt = OPRT_OK;

    TUYA_CALL_ERR_RETURN(tdl_audio_find(AUDIO_CODEC_NAME, &sg_audio_hdl));
    TUYA_CALL_ERR_RETURN(tdl_audio_open(sg_audio_hdl, __speaker_beep_audio_frame_cb));
    TUYA_CALL_ERR_RETURN(tdl_audio_volume_set(sg_audio_hdl, 100));

    PR_NOTICE("speaker audio open success");
    return OPRT_OK;
}

static void __speaker_beep_fill_tone(int16_t *tone, uint32_t sample_count)
{
    for (uint32_t i = 0; i < sample_count; i++) {
        tone[i] = ((i / 8) % 2) ? 24000 : -24000;
    }
}

static void user_main(void)
{
    OPERATE_RET    rt = OPRT_OK;
    static int16_t tone[SPEAKER_BEEP_FRAME_SAMPLES];

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("Application information:");
    PR_NOTICE("Project name:        %s", PROJECT_NAME);
    PR_NOTICE("App version:         %s", PROJECT_VERSION);
    PR_NOTICE("Compile time:        %s", __DATE__);
    PR_NOTICE("TuyaOpen version:    %s", OPEN_VERSION);
    PR_NOTICE("TuyaOpen commit-id:  %s", OPEN_COMMIT);
    PR_NOTICE("Platform chip:       %s", PLATFORM_CHIP);
    PR_NOTICE("Platform board:      %s", PLATFORM_BOARD);
    PR_NOTICE("Platform commit-id:  %s", PLATFORM_COMMIT);

    TUYA_CALL_ERR_LOG(board_register_hardware());
    TUYA_CALL_ERR_LOG(__speaker_beep_audio_open());
    __speaker_beep_fill_tone(tone, SPEAKER_BEEP_FRAME_SAMPLES);

    while (1) {
        PR_NOTICE("speaker beep test via tdl_audio_play");
        for (uint8_t i = 0; i < SPEAKER_BEEP_REPEAT_FRAMES; i++) {
            TUYA_CALL_ERR_LOG(tdl_audio_play(sg_audio_hdl, (uint8_t *)tone, sizeof(tone)));
        }
        tal_system_sleep(SPEAKER_BEEP_PAUSE_MS);
    }
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    user_main();
}
#else
static THREAD_HANDLE s_speaker_beep_thread = NULL;

static void speaker_beep_thread_entry(void *arg)
{
    (void)arg;
    user_main();
    tal_thread_delete(s_speaker_beep_thread);
    s_speaker_beep_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thread_cfg = {0};
    thread_cfg.stackDepth   = 1024 * 4;
    thread_cfg.priority     = THREAD_PRIO_1;
    thread_cfg.thrdname     = "speaker_beep";
    tal_thread_create_and_start(&s_speaker_beep_thread, NULL, NULL, speaker_beep_thread_entry, NULL, &thread_cfg);
}
#endif
