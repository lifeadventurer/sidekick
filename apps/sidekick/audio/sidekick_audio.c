#include "sidekick_audio.h"

#include "sidekick_log.h"

#if defined(ENABLE_AUDIO_CODECS) && (ENABLE_AUDIO_CODECS == 1)
#include "tdl_audio_manage.h"

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

uint32_t sidekick_audio_frame_count(void)
{
#if defined(ENABLE_AUDIO_CODECS) && (ENABLE_AUDIO_CODECS == 1)
    return s_audio_frames;
#else
    return 0;
#endif
}
