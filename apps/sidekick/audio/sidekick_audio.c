#include "sidekick_audio.h"

#include "sidekick_log.h"

#if defined(ENABLE_AUDIO_CODECS) && (ENABLE_AUDIO_CODECS == 1)
#include "tdl_audio_manage.h"

#define SIDEKICK_CHIME_FRAME_SAMPLES 320
#define SIDEKICK_CHIME_REPEAT_FRAMES 2

static TDL_AUDIO_HANDLE_T s_audio_handle = NULL;
static uint32_t           s_audio_frames = 0;

static void sidekick_audio_frame_cb(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status, uint8_t *data,
                                    uint32_t len)
{
    (void)type;
    (void)status;
    (void)data;
    (void)len;

    s_audio_frames++;
}

static void sidekick_audio_fill_chime(int16_t *samples, uint32_t sample_count)
{
    for (uint32_t i = 0; i < sample_count; i++) {
        samples[i] = ((i / 10) % 2) ? 12000 : -12000;
    }
}
#endif

OPERATE_RET sidekick_audio_input_start(void)
{
#if defined(ENABLE_AUDIO_CODECS) && (ENABLE_AUDIO_CODECS == 1)
    OPERATE_RET rt = OPRT_OK;

    TUYA_CALL_ERR_RETURN(tdl_audio_find(AUDIO_CODEC_NAME, &s_audio_handle));
    TUYA_CALL_ERR_RETURN(tdl_audio_open(s_audio_handle, sidekick_audio_frame_cb));
    SIDEKICK_LOGI("audio", "microphone input started");
    return OPRT_OK;
#else
    SIDEKICK_LOGW("audio", "audio codec support not enabled in config");
    return OPRT_OK;
#endif
}

OPERATE_RET sidekick_audio_play_startup_chime(void)
{
#if defined(ENABLE_AUDIO_CODECS) && (ENABLE_AUDIO_CODECS == 1)
    OPERATE_RET    rt = OPRT_OK;
    static int16_t chime[SIDEKICK_CHIME_FRAME_SAMPLES];

    if (s_audio_handle == NULL) {
        SIDEKICK_LOGW("audio", "startup chime skipped; audio is not open");
        return OPRT_OK;
    }

    sidekick_audio_fill_chime(chime, SIDEKICK_CHIME_FRAME_SAMPLES);
    TUYA_CALL_ERR_LOG(tdl_audio_volume_set(s_audio_handle, 80));

    for (uint8_t i = 0; i < SIDEKICK_CHIME_REPEAT_FRAMES; i++) {
        rt = tdl_audio_play(s_audio_handle, (uint8_t *)chime, sizeof(chime));
        if (rt != OPRT_OK) {
            SIDEKICK_LOGW("audio", "startup chime failed rt=%d", rt);
            return rt;
        }
    }

    SIDEKICK_LOGI("audio", "startup chime played");
    return OPRT_OK;
#else
    return OPRT_OK;
#endif
}

uint32_t sidekick_audio_frame_count(void)
{
#if defined(ENABLE_AUDIO_CODECS) && (ENABLE_AUDIO_CODECS == 1)
    return s_audio_frames;
#else
    return 0;
#endif
}
