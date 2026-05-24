#include "sidekick_backend.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "http_client_interface.h"
#include "netmgr.h"
#include "sidekick_camera.h"
#include "sidekick_config.h"
#include "sidekick_log.h"
#include "sidekick_session.h"
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

#define SIDEKICK_BACKEND_TAG          "backend"
#define SIDEKICK_BACKEND_PATH_MAX     160
#define SIDEKICK_BACKEND_MESSAGE_MAX  256
#define SIDEKICK_BACKEND_THREAD_STACK (1024 * 6)

typedef enum {
    SIDEKICK_BACKEND_REQ_NONE = 0,
    SIDEKICK_BACKEND_REQ_FRAME,
    SIDEKICK_BACKEND_REQ_SUMMARY,
} SIDEKICK_BACKEND_REQ_E;

typedef struct {
    bool                   running;
    bool                   network_inited;
    SEM_HANDLE             sem;
    MUTEX_HANDLE           mutex;
    THREAD_HANDLE          thread;
    SIDEKICK_BACKEND_REQ_E pending;
} SIDEKICK_BACKEND_STATE_T;

static SIDEKICK_BACKEND_STATE_T s_backend;

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

static OPERATE_RET sidekick_backend_post(const char *path, const uint8_t *body, size_t body_len,
                                         http_client_response_t *response)
{
    http_client_header_t headers[] = {
        {.key = "Content-Type", .value = body_len > 0 ? "image/jpeg" : "application/json"},
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

static void sidekick_backend_handle_json(const http_client_response_t *response)
{
    char message[SIDEKICK_BACKEND_MESSAGE_MAX] = {0};

    if ((response == NULL) || (response->body == NULL) || (response->body_length == 0)) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)response->body, response->body_length);
    if (root == NULL) {
        SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "failed to parse backend json");
        return;
    }

    cJSON *should_respond = cJSON_GetObjectItem(root, "should_respond");
    cJSON *message_item   = cJSON_GetObjectItem(root, "message");

    bool respond = cJSON_IsTrue(should_respond);
    if (respond && cJSON_IsString(message_item) && (message_item->valuestring != NULL)) {
        strncpy(message, message_item->valuestring, sizeof(message) - 1);
        SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "AI: %s", message);
    } else {
        SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "AI: no action");
    }

    cJSON_Delete(root);
}

static OPERATE_RET sidekick_backend_upload_frame(void)
{
    OPERATE_RET            rt       = OPRT_OK;
    uint8_t               *jpeg     = NULL;
    uint32_t               jpeg_len = 0;
    http_client_response_t response = {0};
    char                   path[SIDEKICK_BACKEND_PATH_MAX];
    const char            *mode = sidekick_session_mode_name(sidekick_session_mode());

    snprintf(path, sizeof(path), "/sidekick/frame?mode=%s&session=%s", mode, SIDEKICK_BACKEND_SESSION_ID);

    TUYA_CALL_ERR_GOTO(sidekick_camera_capture_jpeg(&jpeg, &jpeg_len), done);
    SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "upload jpeg len=%u mode=%s", (unsigned int)jpeg_len, mode);

    rt = sidekick_backend_post(path, jpeg, jpeg_len, &response);
    if (rt == OPRT_OK) {
        sidekick_backend_handle_json(&response);
    }

done:
    if (jpeg != NULL) {
        (void)sidekick_camera_free_jpeg(&jpeg);
    }
    http_client_free(&response);
    return rt;
}

static OPERATE_RET sidekick_backend_request_summary(void)
{
    http_client_response_t response = {0};
    char                   path[SIDEKICK_BACKEND_PATH_MAX];
    OPERATE_RET            rt = OPRT_OK;

    snprintf(path, sizeof(path), "/sidekick/session/end?session=%s", SIDEKICK_BACKEND_SESSION_ID);
    rt = sidekick_backend_post(path, NULL, 0, &response);
    if (rt == OPRT_OK) {
        sidekick_backend_handle_json(&response);
    }

    http_client_free(&response);
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

        if (!sidekick_backend_network_ready()) {
            SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "network not ready; skip backend request");
            continue;
        }

        if (req == SIDEKICK_BACKEND_REQ_FRAME) {
            TUYA_CALL_ERR_LOG(sidekick_backend_upload_frame());
        } else if (req == SIDEKICK_BACKEND_REQ_SUMMARY) {
            TUYA_CALL_ERR_LOG(sidekick_backend_request_summary());
        }
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
    s_backend.pending = req;
    tal_mutex_unlock(s_backend.mutex);
    tal_semaphore_post(s_backend.sem);
}

OPERATE_RET sidekick_backend_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    if (!sidekick_backend_enabled()) {
        SIDEKICK_LOGW(SIDEKICK_BACKEND_TAG, "backend disabled; set SIDEKICK_BACKEND_HOST");
        return OPRT_OK;
    }

    TUYA_CALL_ERR_RETURN(sidekick_backend_network_init());

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

    SIDEKICK_LOGI(SIDEKICK_BACKEND_TAG, "backend ready host=%s port=%u", SIDEKICK_BACKEND_HOST,
                  (unsigned int)SIDEKICK_BACKEND_PORT);
    return rt;
}

void sidekick_backend_request_frame(void)
{
    if (!sidekick_backend_enabled()) {
        return;
    }
    sidekick_backend_schedule(SIDEKICK_BACKEND_REQ_FRAME);
}

void sidekick_backend_end_session(void)
{
    if (!sidekick_backend_enabled()) {
        return;
    }
    sidekick_backend_schedule(SIDEKICK_BACKEND_REQ_SUMMARY);
}
