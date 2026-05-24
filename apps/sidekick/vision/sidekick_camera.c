#include "sidekick_camera.h"

#include "sidekick_config.h"
#include "sidekick_log.h"

#if defined(ENABLE_CAMERA) && (ENABLE_CAMERA == 1) && defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
#include "tdl_camera_manage.h"
#include "tdl_display_manage.h"

#define SIDEKICK_DISPLAY_FRAME_BUFF_NUM 2

static TDL_DISP_HANDLE_T      s_display_handle = NULL;
static TDL_DISP_DEV_INFO_T    s_display_info;
static TDL_FB_MANAGE_HANDLE_T s_fb_manage      = NULL;
static TDL_CAMERA_HANDLE_T    s_camera_handle  = NULL;
static bool                   s_display_ready  = false;
static bool                   s_camera_open    = false;
static bool                   s_preview_active = false;

static OPERATE_RET sidekick_camera_frame_cb(TDL_CAMERA_HANDLE_T hdl, TDL_CAMERA_FRAME_T *frame)
{
    OPERATE_RET rt = OPRT_OK;

    (void)hdl;

    if (!s_preview_active) {
        return OPRT_OK;
    }

    TDL_DISP_FRAME_BUFF_T *fb = tdl_disp_get_free_fb(s_fb_manage);
    TUYA_CHECK_NULL_RETURN(fb, OPRT_COM_ERROR);

    TUYA_CALL_ERR_LOG(tdl_disp_convert_yuv422_to_fb(frame->data, frame->width, frame->height, fb,
                                                    s_display_info.is_swap, TUYA_DISPLAY_ROTATION_0));

    return tdl_disp_dev_flush(s_display_handle, fb);
}

static OPERATE_RET sidekick_display_open(void)
{
    OPERATE_RET rt = OPRT_OK;

    if (s_display_ready) {
        return OPRT_OK;
    }

    memset(&s_display_info, 0, sizeof(s_display_info));

    s_display_handle = tdl_disp_find_dev(DISPLAY_NAME);
    if (s_display_handle == NULL) {
        SIDEKICK_LOGE("camera", "display device %s not found", DISPLAY_NAME);
        return OPRT_NOT_FOUND;
    }

    TUYA_CALL_ERR_RETURN(tdl_disp_dev_get_info(s_display_handle, &s_display_info));
    TUYA_CALL_ERR_RETURN(tdl_disp_dev_open(s_display_handle));
    TUYA_CALL_ERR_RETURN(tdl_disp_set_brightness(s_display_handle, 100));
    TUYA_CALL_ERR_RETURN(tdl_disp_fb_manage_init(&s_fb_manage));

    for (uint8_t i = 0; i < SIDEKICK_DISPLAY_FRAME_BUFF_NUM; i++) {
        TUYA_CALL_ERR_LOG(
            tdl_disp_fb_manage_add(s_fb_manage, s_display_info.fmt, s_display_info.width, s_display_info.height));
    }

    s_display_ready = true;
    return OPRT_OK;
}
#endif

OPERATE_RET sidekick_camera_preview_start(void)
{
#if defined(ENABLE_CAMERA) && (ENABLE_CAMERA == 1) && defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
    OPERATE_RET rt = sidekick_display_open();
    if (rt != OPRT_OK) {
        return rt;
    }

    if (s_camera_open) {
        s_preview_active = true;
        SIDEKICK_LOGI("camera", "camera preview resumed");
        return OPRT_OK;
    }

    s_camera_handle = tdl_camera_find_dev(CAMERA_NAME);
    if (s_camera_handle == NULL) {
        SIDEKICK_LOGE("camera", "camera device %s not found", CAMERA_NAME);
        return OPRT_NOT_FOUND;
    }

    TDL_CAMERA_CFG_T cfg = {
        .fps          = SIDEKICK_CAMERA_FPS,
        .width        = SIDEKICK_CAMERA_WIDTH,
        .height       = SIDEKICK_CAMERA_HEIGHT,
        .out_fmt      = TDL_CAMERA_FMT_YUV422,
        .get_frame_cb = sidekick_camera_frame_cb,
    };

    TUYA_CALL_ERR_RETURN(tdl_camera_dev_open(s_camera_handle, &cfg));
    s_camera_open    = true;
    s_preview_active = true;
    SIDEKICK_LOGI("camera", "camera preview started");
    return OPRT_OK;
#else
    SIDEKICK_LOGW("camera", "camera/display support not enabled in config");
    return OPRT_OK;
#endif
}

OPERATE_RET sidekick_camera_preview_stop(void)
{
#if defined(ENABLE_CAMERA) && (ENABLE_CAMERA == 1) && defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
    s_preview_active = false;
    SIDEKICK_LOGI("camera", "camera preview paused");
    return OPRT_OK;
#else
    return OPRT_OK;
#endif
}
