/**
 * @file sidekick_backend.c
 * @brief SideKick firmware backend upload client.
 *
 * @copyright Copyright (c) 2026 SideKick Contributors. All Rights Reserved.
 *
 */
#include "sidekick_backend.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "http_client_interface.h"
#include "netmgr.h"
#include "sidekick_audio.h"
#include "sidekick_camera.h"
#include "sidekick_config.h"
#include "sidekick_log.h"
#include "sidekick_session.h"
#include "mix_method.h"
#include "tal_api.h"

#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
#include "lwip_init.h"
#endif
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
#include "netconn_wired.h"
#endif

#define SIDEKICK_BACKEND_TAG            "backend"
#define SIDEKICK_BACKEND_PATH_MAX       160
#define SIDEKICK_BACKEND_MESSAGE_MAX    256
#define SIDEKICK_BACKEND_HTTP_HEADER_CT "Content-Type"
#define SIDEKICK_BACKEND_THREAD_STACK   (1024 * 8)

typedef enum {
    SIDEKICK_BACKEND_REQ_NONE = 0,
    SIDEKICK_BACKEND_REQ_FRAME,
    SIDEKICK_BACKEND_REQ_AUDIO,
    SIDEKICK_BACKEND_REQ_SUMMARY,
} SIDEKICK_BACKEND_REQ_E;

typedef struct {
    bool                   running;
    bool                   network_inited;
    bool                   busy;
    SEM_HANDLE             sem;
    MUTEX_HANDLE           mutex;
    THREAD_HANDLE          thread;
    SIDEKICK_BACKEND_REQ_E pending;
} SIDEKICK_BACKEND_STATE_T;

static SIDEKICK_BACKEND_STATE_T s_backend;
static bool                     s_backend_disabled_warned = false;

static bool sidekick_backend_enabled(void)
{
    return SIDEKICK_BACKEND_HOST[0] != '\0';
}

static OPERATE_RET sidekick_backend_network_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    if (s_backend.network_inited) {
        return OPRT_OK;
    }

#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif

    netmgr_type_e type = 0;
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    type |= NETCONN_WIFI;
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
    type |= NETCONN_WIRED;
#endif

    if (type == 0) {
        SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "no network transport enabled");
        return OPRT_NOT_SUPPORTED;
    }

    TUYA_CALL_ERR_RETURN(netmgr_init(type));

#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    if (SIDEKICK_WIFI_SSID[0] != '\0') {
        netconn_wifi_info_t wifi_info = {0};
        strncpy(wifi_info.ssid, SIDEKICK_WIFI_SSID, sizeof(wifi_info.ssid) - 1);
        strncpy(wifi_info.pswd, SIDEKICK_WIFI_PSWD, sizeof(wifi_info.pswd) - 1);
        TUYA_CALL_ERR_RETURN(netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &wifi_info));
        SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "wifi credentials configured for backend upload");
    } else {
        SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG,
                      "SIDEKICK_WIFI_SSID is empty; expecting network to be configured elsewhere");
    }
#endif

    s_backend.network_inited = true;
    return OPRT_OK;
}

static bool sidekick_backend_network_ready(void)
{
    netmgr_status_e status = NETMGR_LINK_DOWN;

    if (netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &status) != OPRT_OK) {
        return false;
    }

    return (status == NETMGR_LINK_UP) || (status == NETMGR_LINK_UP_SWITH);
}

static OPERATE_RET sidekick_backend_post_content(const char *path, const uint8_t *body, size_t body_len,
                                                 const char *content_type, http_client_response_t *response)
{
    http_client_header_t headers[] = {
        {.key = SIDEKICK_BACKEND_HTTP_HEADER_CT, .value = content_type},
    };

    http_client_status_t http_rt = http_client_request(
        &(const http_client_request_t){
            .host          = SIDEKICK_BACKEND_HOST,
            .port          = SIDEKICK_BACKEND_PORT,
            .method        = "POST",
            .path          = path,
            .headers       = headers,
            .headers_count = sizeof(headers) / sizeof(headers[0]),
            .body          = body,
            .body_length   = body_len,
            .timeout_ms    = SIDEKICK_BACKEND_TIMEOUT_MS,
        },
        response);

    if (http_rt != HTTP_CLIENT_SUCCESS) {
        SIDEKICK_LOGE(SIDEKICK_BACKEND_TAG, "http request failed: %d", (int)http_rt);
        return OPRT_COM_ERROR;
    }

    if ((response->status_code < 200) || (response->status_code >= 300)) {
        SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "backend returned http=%u", response->status_code);
        return OPRT_COM_ERROR;
    }

    return OPRT_OK;
}

static OPERATE_RET sidekick_backend_post(const char *path, const uint8_t *body, size_t body_len,
                                         http_client_response_t *response)
{
    const char *content_type = (body_len > 0) ? "image/jpeg" : "application/json";

    return sidekick_backend_post_content(path, body, body_len, content_type, response);
}

static const uint8_t *sidekick_backend_wav_payload(const uint8_t *audio, size_t audio_len, size_t *payload_len)
{
    size_t pos = 12;

    *payload_len = audio_len;
    if ((audio_len < 44) || (memcmp(audio, "RIFF", 4) != 0) || (memcmp(audio + 8, "WAVE", 4) != 0)) {
        return audio;
    }

    while ((pos + 8) <= audio_len) {
        uint32_t chunk_len = (uint32_t)audio[pos + 4] | ((uint32_t)audio[pos + 5] << 8) |
                             ((uint32_t)audio[pos + 6] << 16) | ((uint32_t)audio[pos + 7] << 24);

        if (memcmp(audio + pos, "data", 4) == 0) {
            pos += 8;
            if ((pos + chunk_len) > audio_len) {
                chunk_len = audio_len - pos;
            }
            *payload_len = chunk_len;
            return audio + pos;
        }

        pos += 8 + chunk_len + (chunk_len & 1U);
    }

    return audio;
}

static OPERATE_RET sidekick_backend_speak(const char *message)
{
    OPERATE_RET            rt       = OPRT_OK;
    cJSON                 *root     = NULL;
    char                  *body     = NULL;
    http_client_response_t response = {0};
    size_t                 pcm_len  = 0;
    const uint8_t         *pcm      = NULL;

    if ((message == NULL) || (message[0] == '\0')) {
        return OPRT_OK;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        return OPRT_MALLOC_FAILED;
    }

    if ((cJSON_AddStringToObject(root, "text", message) == NULL) ||
        (cJSON_AddStringToObject(root, "format", SIDEKICK_TTS_OUTPUT_FORMAT) == NULL)) {
        cJSON_Delete(root);
        return OPRT_MALLOC_FAILED;
    }

    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return OPRT_MALLOC_FAILED;
    }

    SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "requesting TTS len=%u", (unsigned int)strlen(message));
    rt = sidekick_backend_post_content("/sidekick/tts", (const uint8_t *)body, strlen(body), "application/json",
                                       &response);
    cJSON_free(body);
    if (rt != OPRT_OK) {
        http_client_free(&response);
        return rt;
    }

    if ((response.body == NULL) || (response.body_length == 0)) {
        SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "TTS response was empty");
        http_client_free(&response);
        return OPRT_COM_ERROR;
    }

    pcm = sidekick_backend_wav_payload(response.body, response.body_length, &pcm_len);
    SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "TTS audio bytes=%u pcm_bytes=%u", (unsigned int)response.body_length,
                  (unsigned int)pcm_len);
    rt = sidekick_audio_play_pcm(pcm, (uint32_t)pcm_len);

    http_client_free(&response);
    return rt;
}

static void sidekick_backend_log_transcript(const http_client_response_t *response)
{
    if ((response == NULL) || (response->body == NULL) || (response->body_length == 0)) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)response->body, response->body_length);
    if (root == NULL) {
        return;
    }

    cJSON *transcript = cJSON_GetObjectItem(root, "transcript");
    if (cJSON_IsString(transcript) && (transcript->valuestring != NULL) && (transcript->valuestring[0] != '\0')) {
        SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "mic transcript: %s", transcript->valuestring);
    }
    cJSON_Delete(root);
}

static OPERATE_RET sidekick_backend_upload_microphone(void)
{
    OPERATE_RET rt      = OPRT_OK;
    uint8_t    *pcm     = NULL;
    uint32_t    pcm_len = 0;
    uint32_t    min_len = (SIDEKICK_MIC_SAMPLE_RATE * SIDEKICK_MIC_CHANNELS * (SIDEKICK_MIC_BITS_PER_SAMPLE / 8) *
                           SIDEKICK_MIC_UPLOAD_MIN_MS) /
                          1000;
    http_client_response_t response = {0};
    char                   path[SIDEKICK_BACKEND_PATH_MAX];

    if (!sidekick_backend_enabled() || !s_backend.network_inited || !sidekick_backend_network_ready()) {
        return OPRT_OK;
    }

    TUYA_CALL_ERR_RETURN(sidekick_audio_drain_pcm(&pcm, &pcm_len));
    if ((pcm == NULL) || (pcm_len < min_len)) {
        if (pcm != NULL) {
            tal_free(pcm);
        }
        return OPRT_OK;
    }

    snprintf(path, sizeof(path), "/sidekick/audio?session=%s&sample_rate=%u&channels=%u&bits=%u",
             SIDEKICK_BACKEND_SESSION_ID, (unsigned int)SIDEKICK_MIC_SAMPLE_RATE, (unsigned int)SIDEKICK_MIC_CHANNELS,
             (unsigned int)SIDEKICK_MIC_BITS_PER_SAMPLE);

    SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "uploading mic pcm bytes=%u", (unsigned int)pcm_len);
    rt = sidekick_backend_post_content(path, pcm, pcm_len, "audio/L16", &response);
    if (rt == OPRT_OK) {
        sidekick_backend_log_transcript(&response);
    }

    tal_free(pcm);
    http_client_free(&response);
    return rt;
}

static bool sidekick_backend_handle_json(const http_client_response_t *response, char *message, size_t message_len,
                                         uint8_t **audio_out, size_t *audio_len_out)
{
    bool should_speak = false;

    if ((response == NULL) || (response->body == NULL) || (response->body_length == 0)) {
        return false;
    }

    if ((message == NULL) || (message_len == 0)) {
        return false;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)response->body, response->body_length);
    if (root == NULL) {
        SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "failed to parse backend json");
        return false;
    }

    cJSON *should_respond = cJSON_GetObjectItem(root, "should_respond");
    cJSON *message_item   = cJSON_GetObjectItem(root, "message");

    bool respond = cJSON_IsTrue(should_respond);
    if (respond && cJSON_IsString(message_item) && (message_item->valuestring != NULL)) {
        strncpy(message, message_item->valuestring, message_len - 1);
        should_speak = message[0] != '\0';
        SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "AI: %s", message);
    } else {
        SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "AI: no action respond=%d has_message=%d", respond ? 1 : 0,
                      cJSON_IsString(message_item) ? 1 : 0);
    }

    /* Parse inline TTS audio if present. */
    if (should_speak && (audio_out != NULL) && (audio_len_out != NULL)) {
        cJSON *audio_b64 = cJSON_GetObjectItem(root, "audio_base64");

        if (cJSON_IsString(audio_b64) && (audio_b64->valuestring != NULL) && (audio_b64->valuestring[0] != '\0')) {
            size_t   b64_len     = strlen(audio_b64->valuestring);
            size_t   max_decoded = b64_len; /* decoded is always smaller than base64 */
            uint8_t *decoded     = (uint8_t *)tal_malloc(max_decoded);

            if (decoded != NULL) {
                int decoded_len = tuya_base64_decode(audio_b64->valuestring, decoded);

                if (decoded_len > 0) {
                    *audio_out     = decoded;
                    *audio_len_out = (size_t)decoded_len;
                    SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "inline audio decoded base64=%u pcm=%d", (unsigned int)b64_len,
                                  decoded_len);
                } else {
                    tal_free(decoded);
                    SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "failed to decode inline audio base64");
                }
            } else {
                SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "failed to allocate buffer for inline audio");
            }
        }
    }

    cJSON_Delete(root);

    return should_speak;
}

static OPERATE_RET sidekick_backend_upload_frame(void)
{
    OPERATE_RET            rt       = OPRT_OK;
    uint8_t               *jpeg     = NULL;
    uint32_t               jpeg_len = 0;
    http_client_response_t response = {0};
    char                   path[SIDEKICK_BACKEND_PATH_MAX];
    char                   message[SIDEKICK_BACKEND_MESSAGE_MAX] = {0};
    bool                   should_speak                          = false;
    uint8_t               *audio                                 = NULL;
    size_t                 audio_len                             = 0;
    const char            *mode                                  = sidekick_session_mode_name(sidekick_session_mode());

    snprintf(path, sizeof(path), "/sidekick/frame?mode=%s&session=%s", mode, SIDEKICK_BACKEND_SESSION_ID);

    TUYA_CALL_ERR_GOTO(sidekick_camera_capture_jpeg(&jpeg, &jpeg_len), done);
    SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "captured jpeg len=%u mode=%s", (unsigned int)jpeg_len, mode);

    if (!sidekick_backend_enabled()) {
        SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "backend disabled; captured frame locally only");
        goto done;
    }

    if (!s_backend.network_inited) {
        SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "network not initialized; captured frame locally only");
        goto done;
    }

    if (!sidekick_backend_network_ready()) {
        SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "network not ready; captured frame locally only");
        goto done;
    }

    TUYA_CALL_ERR_LOG(sidekick_backend_upload_microphone());

    rt = sidekick_backend_post(path, jpeg, jpeg_len, &response);
    if (rt == OPRT_OK) {
        should_speak = sidekick_backend_handle_json(&response, message, sizeof(message), &audio, &audio_len);
    }

done:
    if (jpeg != NULL) {
        (void)sidekick_camera_free_jpeg(&jpeg);
    }
    http_client_free(&response);

    if (should_speak) {
        if ((audio != NULL) && (audio_len > 0)) {
            SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "playing inline TTS audio pcm_bytes=%u", (unsigned int)audio_len);
            TUYA_CALL_ERR_LOG(sidekick_audio_play_pcm(audio, (uint32_t)audio_len));
        } else {
            SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "no inline audio; requesting TTS separately");
            TUYA_CALL_ERR_LOG(sidekick_backend_speak(message));
        }
    }

    if (audio != NULL) {
        tal_free(audio);
    }
    return rt;
}

static OPERATE_RET sidekick_backend_request_summary(void)
{
    http_client_response_t response = {0};
    char                   path[SIDEKICK_BACKEND_PATH_MAX];
    char                   message[SIDEKICK_BACKEND_MESSAGE_MAX] = {0};
    bool                   should_speak                          = false;
    uint8_t               *audio                                 = NULL;
    size_t                 audio_len                             = 0;
    OPERATE_RET            rt                                    = OPRT_OK;

    snprintf(path, sizeof(path), "/sidekick/session/end?session=%s", SIDEKICK_BACKEND_SESSION_ID);
    rt = sidekick_backend_post(path, NULL, 0, &response);
    if (rt == OPRT_OK) {
        should_speak = sidekick_backend_handle_json(&response, message, sizeof(message), &audio, &audio_len);
    }

    http_client_free(&response);

    if (should_speak) {
        if ((audio != NULL) && (audio_len > 0)) {
            SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "playing inline summary TTS audio pcm_bytes=%u",
                          (unsigned int)audio_len);
            TUYA_CALL_ERR_LOG(sidekick_audio_play_pcm(audio, (uint32_t)audio_len));
        } else {
            SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "no inline audio; requesting TTS separately");
            TUYA_CALL_ERR_LOG(sidekick_backend_speak(message));
        }
    } else if (rt == OPRT_OK) {
        SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "summary response had no speakable message");
    }

    if (audio != NULL) {
        tal_free(audio);
    }
    return rt;
}

static SIDEKICK_BACKEND_REQ_E sidekick_backend_take_pending(void)
{
    SIDEKICK_BACKEND_REQ_E req = SIDEKICK_BACKEND_REQ_NONE;

    tal_mutex_lock(s_backend.mutex);
    req               = s_backend.pending;
    s_backend.pending = SIDEKICK_BACKEND_REQ_NONE;
    tal_mutex_unlock(s_backend.mutex);

    return req;
}

static void sidekick_backend_set_busy(bool busy)
{
    tal_mutex_lock(s_backend.mutex);
    s_backend.busy = busy;
    tal_mutex_unlock(s_backend.mutex);
}

static void sidekick_backend_worker(void *arg)
{
    OPERATE_RET rt = OPRT_OK;

    (void)arg;

    while (s_backend.running) {
        if (tal_semaphore_wait(s_backend.sem, SEM_WAIT_FOREVER) != OPRT_OK) {
            continue;
        }

        SIDEKICK_BACKEND_REQ_E req = sidekick_backend_take_pending();
        if (req == SIDEKICK_BACKEND_REQ_NONE) {
            continue;
        }

        sidekick_backend_set_busy(true);
        if (req == SIDEKICK_BACKEND_REQ_FRAME) {
            TUYA_CALL_ERR_LOG(sidekick_backend_upload_frame());
        } else if (req == SIDEKICK_BACKEND_REQ_AUDIO) {
            if (!s_backend.network_inited) {
                SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "network not initialized; skip microphone request");
                sidekick_backend_set_busy(false);
                continue;
            }

            if (!sidekick_backend_network_ready()) {
                SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "network not ready; skip microphone request");
                sidekick_backend_set_busy(false);
                continue;
            }

            TUYA_CALL_ERR_LOG(sidekick_backend_upload_microphone());
        } else if (req == SIDEKICK_BACKEND_REQ_SUMMARY) {
            if (!s_backend.network_inited) {
                SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "network not initialized; skip summary request");
                sidekick_backend_set_busy(false);
                continue;
            }

            if (!sidekick_backend_network_ready()) {
                SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "network not ready; skip summary request");
                sidekick_backend_set_busy(false);
                continue;
            }

            TUYA_CALL_ERR_LOG(sidekick_backend_request_summary());
        }
        sidekick_backend_set_busy(false);
    }

    tal_thread_delete(s_backend.thread);
    s_backend.thread = NULL;
}

static void sidekick_backend_schedule(SIDEKICK_BACKEND_REQ_E req)
{
    if (!s_backend.running || (s_backend.sem == NULL) || (s_backend.mutex == NULL)) {
        return;
    }

    tal_mutex_lock(s_backend.mutex);
    if ((req == SIDEKICK_BACKEND_REQ_AUDIO) && (s_backend.busy || (s_backend.pending != SIDEKICK_BACKEND_REQ_NONE))) {
        tal_mutex_unlock(s_backend.mutex);
        SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "drop mic request; backend worker busy");
        return;
    }
    if ((req == SIDEKICK_BACKEND_REQ_FRAME) && (s_backend.pending == SIDEKICK_BACKEND_REQ_SUMMARY)) {
        tal_mutex_unlock(s_backend.mutex);
        SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "drop frame request; summary pending");
        return;
    }
    s_backend.pending = req;
    tal_mutex_unlock(s_backend.mutex);
    tal_semaphore_post(s_backend.sem);
}

OPERATE_RET sidekick_backend_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    if (s_backend.mutex == NULL) {
        TUYA_CALL_ERR_RETURN(tal_mutex_create_init(&s_backend.mutex));
    }

    if (s_backend.sem == NULL) {
        TUYA_CALL_ERR_RETURN(tal_semaphore_create_init(&s_backend.sem, 0, 1));
    }

    if (s_backend.thread == NULL) {
        THREAD_CFG_T cfg  = {0};
        cfg.stackDepth    = SIDEKICK_BACKEND_THREAD_STACK;
        cfg.priority      = THREAD_PRIO_2;
        cfg.thrdname      = "sk_backend";
        s_backend.running = true;
        TUYA_CALL_ERR_RETURN(
            tal_thread_create_and_start(&s_backend.thread, NULL, NULL, sidekick_backend_worker, NULL, &cfg));
    }

    if (sidekick_backend_enabled()) {
        SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "backend ready host=%s port=%u (wifi on first upload)",
                      SIDEKICK_BACKEND_HOST, (unsigned int)SIDEKICK_BACKEND_PORT);
    } else {
        SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "backend host empty; frame capture will run without upload");
    }
    return rt;
}

static void sidekick_backend_prepare_network(void)
{
    OPERATE_RET rt = OPRT_OK;

    if (!sidekick_backend_enabled() || s_backend.network_inited) {
        return;
    }

    rt = sidekick_backend_network_init();
    if (rt != OPRT_OK) {
        SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "network init failed rt=%d", rt);
    }
}

void sidekick_backend_request_frame(void)
{
    if (!sidekick_backend_enabled()) {
        if (!s_backend_disabled_warned) {
            SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "capture frame without upload; set SIDEKICK_BACKEND_HOST and rebuild");
            s_backend_disabled_warned = true;
        }
    } else {
        sidekick_backend_prepare_network();
    }

    sidekick_backend_schedule(SIDEKICK_BACKEND_REQ_FRAME);
}

void sidekick_backend_request_microphone(void)
{
    if (!sidekick_backend_enabled()) {
        return;
    }

    sidekick_backend_prepare_network();
    sidekick_backend_schedule(SIDEKICK_BACKEND_REQ_AUDIO);
}

void sidekick_backend_end_session(void)
{
    if (!sidekick_backend_enabled()) {
        return;
    }

    sidekick_backend_prepare_network();
    sidekick_backend_schedule(SIDEKICK_BACKEND_REQ_SUMMARY);
}
