/**
 * @file sidekick_audio.c
 * @brief SideKick audio input and speaker playback helpers.
 *
 * @copyright Copyright (c) 2026 SideKick Contributors. All Rights Reserved.
 *
 */
#include "sidekick_audio.h"

#include <stdbool.h>
#include <string.h>

#include "sidekick_config.h"
#include "sidekick_log.h"
#include "tal_api.h"

#if defined(ENABLE_AUDIO_CODECS) && (ENABLE_AUDIO_CODECS == 1)
#include "tdl_audio_manage.h"

#define SIDEKICK_CHIME_FRAME_SAMPLES  320
#define SIDEKICK_CHIME_REPEAT_FRAMES  2
#define SIDEKICK_PCM_PLAY_CHUNK_BYTES 640
#define SIDEKICK_MIC_BYTES_PER_SEC                                                                                     \
    (SIDEKICK_MIC_SAMPLE_RATE * SIDEKICK_MIC_CHANNELS * (SIDEKICK_MIC_BITS_PER_SAMPLE / 8))
#define SIDEKICK_MIC_BUFFER_BYTES (SIDEKICK_MIC_BYTES_PER_SEC * SIDEKICK_MIC_CAPTURE_SECONDS)

static TDL_AUDIO_HANDLE_T s_audio_handle  = NULL;
static uint32_t           s_audio_frames  = 0;
static uint8_t           *s_mic_buffer    = NULL;
static uint32_t           s_mic_write     = 0;
static uint32_t           s_mic_len       = 0;
static MUTEX_HANDLE       s_mic_mutex     = NULL;
static bool               s_audio_playing = false;

static void sidekick_audio_set_playing(bool playing)
{
    if (s_mic_mutex == NULL) {
        s_audio_playing = playing;
        return;
    }

    tal_mutex_lock(s_mic_mutex);
    s_audio_playing = playing;
    tal_mutex_unlock(s_mic_mutex);
}

static void sidekick_audio_ring_append(const uint8_t *data, uint32_t len)
{
    if ((s_mic_buffer == NULL) || (s_mic_mutex == NULL) || (data == NULL) || (len == 0)) {
        return;
    }

    tal_mutex_lock(s_mic_mutex);
    if (s_audio_playing) {
        tal_mutex_unlock(s_mic_mutex);
        return;
    }
    if (len >= SIDEKICK_MIC_BUFFER_BYTES) {
        uint32_t keep = SIDEKICK_MIC_BUFFER_BYTES;
        memcpy(s_mic_buffer, data + len - keep, keep);
        s_mic_write = 0;
        s_mic_len   = keep;
        tal_mutex_unlock(s_mic_mutex);
        return;
    }

    uint32_t first = SIDEKICK_MIC_BUFFER_BYTES - s_mic_write;
    if (first > len) {
        first = len;
    }
    memcpy(s_mic_buffer + s_mic_write, data, first);
    if (len > first) {
        memcpy(s_mic_buffer, data + first, len - first);
    }

    s_mic_write = (s_mic_write + len) % SIDEKICK_MIC_BUFFER_BYTES;
    if ((SIDEKICK_MIC_BUFFER_BYTES - s_mic_len) < len) {
        s_mic_len = SIDEKICK_MIC_BUFFER_BYTES;
    } else {
        s_mic_len += len;
    }
    tal_mutex_unlock(s_mic_mutex);
}

static void sidekick_audio_frame_cb(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status, uint8_t *data,
                                    uint32_t len)
{
    s_audio_frames++;
    if ((type != TDL_AUDIO_FRAME_FORMAT_PCM) || (status != TDL_AUDIO_STATUS_RECEIVING)) {
        return;
    }
    sidekick_audio_ring_append(data, len);
}

static void sidekick_audio_fill_chime(int16_t *samples, uint32_t sample_count)
{
    for (uint32_t i = 0; i < sample_count; i++) {
        samples[i] = ((i / 10) % 2) ? 12000 : -12000;
    }
}

static void sidekick_audio_clear_mic_buffer(void)
{
    if ((s_mic_buffer == NULL) || (s_mic_mutex == NULL)) {
        return;
    }

    tal_mutex_lock(s_mic_mutex);
    s_mic_write = 0;
    s_mic_len   = 0;
    tal_mutex_unlock(s_mic_mutex);
}

static void sidekick_audio_wait_for_playback(uint32_t pcm_bytes)
{
    uint32_t bytes_per_ms = SIDEKICK_MIC_BYTES_PER_SEC / 1000;
    uint32_t wait_ms      = 0;

    if ((pcm_bytes == 0) || (bytes_per_ms == 0)) {
        return;
    }

    wait_ms = (pcm_bytes + bytes_per_ms - 1) / bytes_per_ms;
    wait_ms += 80;
    tal_system_sleep(wait_ms);
}
#endif

OPERATE_RET sidekick_audio_input_start(void)
{
#if defined(ENABLE_AUDIO_CODECS) && (ENABLE_AUDIO_CODECS == 1)
    OPERATE_RET rt = OPRT_OK;

    if (s_mic_mutex == NULL) {
        TUYA_CALL_ERR_RETURN(tal_mutex_create_init(&s_mic_mutex));
    }
    if (s_mic_buffer == NULL) {
        s_mic_buffer = (uint8_t *)tal_malloc(SIDEKICK_MIC_BUFFER_BYTES);
        if (s_mic_buffer == NULL) {
            return OPRT_MALLOC_FAILED;
        }
        memset(s_mic_buffer, 0, SIDEKICK_MIC_BUFFER_BYTES);
    }

    TUYA_CALL_ERR_RETURN(tdl_audio_find(AUDIO_CODEC_NAME, &s_audio_handle));
    TUYA_CALL_ERR_RETURN(tdl_audio_open(s_audio_handle, sidekick_audio_frame_cb));
    SIDEKICK_LOGI("audio", "microphone input started buffer=%u bytes", (unsigned int)SIDEKICK_MIC_BUFFER_BYTES);
    return OPRT_OK;
#else
    SIDEKICK_LOGW("audio", "audio codec support not enabled in config");
    return OPRT_OK;
#endif
}

OPERATE_RET sidekick_audio_drain_pcm(uint8_t **pcm, uint32_t *len)
{
    uint32_t copy_len = 0;
    uint32_t start    = 0;

    if (pcm != NULL) {
        *pcm = NULL;
    }
    if (len != NULL) {
        *len = 0;
    }
    if ((pcm == NULL) || (len == NULL)) {
        return OPRT_INVALID_PARM;
    }

#if defined(ENABLE_AUDIO_CODECS) && (ENABLE_AUDIO_CODECS == 1)
    if ((s_mic_buffer == NULL) || (s_mic_mutex == NULL)) {
        return OPRT_OK;
    }

    tal_mutex_lock(s_mic_mutex);
    copy_len = s_mic_len;
    if (copy_len == 0) {
        tal_mutex_unlock(s_mic_mutex);
        return OPRT_OK;
    }

    *pcm = (uint8_t *)tal_malloc(copy_len);
    if (*pcm == NULL) {
        tal_mutex_unlock(s_mic_mutex);
        return OPRT_MALLOC_FAILED;
    }

    start = (s_mic_write + SIDEKICK_MIC_BUFFER_BYTES - copy_len) % SIDEKICK_MIC_BUFFER_BYTES;
    if ((start + copy_len) <= SIDEKICK_MIC_BUFFER_BYTES) {
        memcpy(*pcm, s_mic_buffer + start, copy_len);
    } else {
        uint32_t first = SIDEKICK_MIC_BUFFER_BYTES - start;
        memcpy(*pcm, s_mic_buffer + start, first);
        memcpy(*pcm + first, s_mic_buffer, copy_len - first);
    }
    s_mic_write = 0;
    s_mic_len   = 0;
    tal_mutex_unlock(s_mic_mutex);

    *len = copy_len;
    return OPRT_OK;
#else
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

    sidekick_audio_set_playing(true);
    for (uint8_t i = 0; i < SIDEKICK_CHIME_REPEAT_FRAMES; i++) {
        rt = tdl_audio_play(s_audio_handle, (uint8_t *)chime, sizeof(chime));
        if (rt != OPRT_OK) {
            SIDEKICK_LOGW("audio", "startup chime failed rt=%d", rt);
            sidekick_audio_set_playing(false);
            return rt;
        }
    }
    sidekick_audio_wait_for_playback(sizeof(chime) * SIDEKICK_CHIME_REPEAT_FRAMES);
    sidekick_audio_set_playing(false);
    sidekick_audio_clear_mic_buffer();

    SIDEKICK_LOGI("audio", "startup chime played");
    return OPRT_OK;
#else
    return OPRT_OK;
#endif
}

OPERATE_RET sidekick_audio_play_pcm(const uint8_t *pcm, uint32_t len)
{
#if defined(ENABLE_AUDIO_CODECS) && (ENABLE_AUDIO_CODECS == 1)
    OPERATE_RET rt     = OPRT_OK;
    uint32_t    offset = 0;

    if ((pcm == NULL) || (len < 2)) {
        return OPRT_OK;
    }

    if (s_audio_handle == NULL) {
        SIDEKICK_LOGW("audio", "pcm playback skipped; audio is not open");
        return OPRT_OK;
    }

    len &= ~1U;
    TUYA_CALL_ERR_LOG(tdl_audio_volume_set(s_audio_handle, 80));

    sidekick_audio_set_playing(true);
    while (offset < len) {
        uint32_t chunk = len - offset;

        if (chunk > SIDEKICK_PCM_PLAY_CHUNK_BYTES) {
            chunk = SIDEKICK_PCM_PLAY_CHUNK_BYTES;
        }

        rt = tdl_audio_play(s_audio_handle, (uint8_t *)(pcm + offset), chunk);
        if (rt != OPRT_OK) {
            SIDEKICK_LOGW("audio", "pcm playback failed rt=%d", rt);
            sidekick_audio_set_playing(false);
            return rt;
        }
        offset += chunk;
        tal_system_sleep(10);
    }
    sidekick_audio_wait_for_playback(len);
    sidekick_audio_set_playing(false);
    sidekick_audio_clear_mic_buffer();

    SIDEKICK_LOGI("audio", "pcm playback queued len=%u", (unsigned int)len);
    return OPRT_OK;
#else
    (void)pcm;
    (void)len;
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
