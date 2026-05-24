/**
 * @file sidekick_camera.c
 * @brief SideKick camera preview and JPEG capture.
 *
 * @copyright Copyright (c) 2026 SideKick Contributors. All Rights Reserved.
 *
 */
#include "sidekick_camera.h"

#include "sidekick_config.h"
#include "sidekick_log.h"

static OPERATE_RET sidekick_camera_preview_start_in_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height);

#if defined(ENABLE_CAMERA) && (ENABLE_CAMERA == 1) && defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
#include "tal_api.h"
#include "tdl_camera_manage.h"
#include "tdl_display_manage.h"

#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
#define SIDEKICK_CAMERA_MALLOC tal_psram_malloc
#define SIDEKICK_CAMERA_FREE   tal_psram_free
#else
#define SIDEKICK_CAMERA_MALLOC tal_malloc
#define SIDEKICK_CAMERA_FREE   tal_free
#endif

#define SIDEKICK_DISPLAY_FRAME_BUFF_NUM  2
#define SIDEKICK_GLYPH_WIDTH             5
#define SIDEKICK_GLYPH_SPACING           1
#define SIDEKICK_GLYPH_HEIGHT            7
#define SIDEKICK_END_LETTERS             3
#define SIDEKICK_CAMERA_PREVIEW_ROTATION TUYA_DISPLAY_ROTATION_180

#if SIDEKICK_ENABLE_CAPTURE_SAVE
#include <stdio.h>

#include "tal_time_service.h"
#include "tkl_fs.h"

#define SIDEKICK_CAPTURE_PATH_MAX 192

#if OPERATING_SYSTEM == SYSTEM_LINUX
#define SIDEKICK_CAPTURE_SAVE_DIR "captures"
#else
#define SIDEKICK_CAPTURE_SAVE_DIR "/sdcard/sidekick/captures"
#endif

static bool     s_capture_dir_ready = false;
static uint32_t s_capture_seq       = 0;

static OPERATE_RET sidekick_camera_ensure_capture_dir(void)
{
    BOOL_T exists = FALSE;

    if (s_capture_dir_ready) {
        return OPRT_OK;
    }

    if ((tkl_fs_is_exist(SIDEKICK_CAPTURE_SAVE_DIR, &exists) == 0) && exists) {
        s_capture_dir_ready = true;
        return OPRT_OK;
    }

    if (tkl_fs_mkdir(SIDEKICK_CAPTURE_SAVE_DIR) != 0) {
        return OPRT_COM_ERROR;
    }

    s_capture_dir_ready = true;
    SIDEKICK_LOGI("camera", "capture save dir ready: %s", SIDEKICK_CAPTURE_SAVE_DIR);
    return OPRT_OK;
}

static void sidekick_camera_save_jpeg_capture(const uint8_t *data, uint32_t len)
{
    char       path[SIDEKICK_CAPTURE_PATH_MAX];
    TUYA_FILE  file_hdl  = NULL;
    int        write_len = 0;
    SYS_TICK_T now_ms    = 0;

    if ((data == NULL) || (len == 0)) {
        return;
    }

    if (sidekick_camera_ensure_capture_dir() != OPRT_OK) {
        SIDEKICK_LOGW("camera", "capture save skipped; cannot use dir %s", SIDEKICK_CAPTURE_SAVE_DIR);
        return;
    }

    now_ms = tal_time_get_posix_ms();
    s_capture_seq++;
    snprintf(path, sizeof(path), "%s/frame_%llu_%u.jpg", SIDEKICK_CAPTURE_SAVE_DIR, (unsigned long long)now_ms,
             (unsigned int)s_capture_seq);

    file_hdl = tkl_fopen(path, "wb");
    if (file_hdl == NULL) {
        SIDEKICK_LOGW("camera", "capture save open failed: %s", path);
        return;
    }

    write_len = tkl_fwrite((void *)data, (int)len, file_hdl);
    (void)tkl_fclose(file_hdl);
    if (write_len != (int)len) {
        SIDEKICK_LOGW("camera", "capture save write failed: %s wrote %d of %u", path, write_len, (unsigned int)len);
        return;
    }

    SIDEKICK_LOGI("camera", "capture saved %s (%u bytes)", path, (unsigned int)len);
}
#endif /* SIDEKICK_ENABLE_CAPTURE_SAVE */

typedef struct {
    uint8_t     *data;
    uint32_t     len;
    bool         need_capture;
    SEM_HANDLE   sem;
    MUTEX_HANDLE mutex;
} SIDEKICK_JPEG_CAPTURE_T;

static TDL_DISP_HANDLE_T       s_display_handle = NULL;
static TDL_DISP_DEV_INFO_T     s_display_info;
static TDL_FB_MANAGE_HANDLE_T  s_fb_manage      = NULL;
static TDL_CAMERA_HANDLE_T     s_camera_handle  = NULL;
static bool                    s_display_ready  = false;
static bool                    s_camera_open    = false;
static bool                    s_preview_active = false;
static bool                    s_overlay_active = false;
static uint16_t                s_canvas_width   = 0;
static uint16_t                s_canvas_height  = 0;
static bool                    s_rotate_canvas  = false;
static bool                    s_flip_canvas    = false;
static uint16_t                s_header_height  = 0;
static uint16_t                s_end_x          = 0;
static uint16_t                s_end_y          = 0;
static uint16_t                s_end_w          = 0;
static uint16_t                s_end_h          = 0;
static SIDEKICK_JPEG_CAPTURE_T s_jpeg_capture;

static uint32_t sidekick_camera_frame_len(TUYA_DISPLAY_PIXEL_FMT_E fmt, uint16_t width, uint16_t height)
{
    uint8_t  bpp = tdl_disp_get_fmt_bpp(fmt);
    uint32_t bytes_per_pixel;
    uint32_t pixels_per_byte;

    if (bpp == 0) {
        return 0;
    }

    if (bpp < 8) {
        pixels_per_byte = 8 / bpp;
        return ((width + pixels_per_byte - 1) / pixels_per_byte) * height;
    }

    bytes_per_pixel = (bpp + 7) / 8;
    return width * height * bytes_per_pixel;
}

static uint32_t sidekick_camera_color(uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_display_info.fmt == TUYA_PIXEL_FMT_RGB888) {
        return ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
    }

    return (((uint32_t)red & 0xF8) << 8) | (((uint32_t)green & 0xFC) << 3) | ((uint32_t)blue >> 3);
}

static void sidekick_camera_apply_full_frame(TDL_DISP_FRAME_BUFF_T *fb)
{
    fb->x_start = 0;
    fb->y_start = 0;
    fb->width   = s_display_info.width;
    fb->height  = s_display_info.height;
    fb->len     = sidekick_camera_frame_len(fb->fmt, fb->width, fb->height);
}

static OPERATE_RET sidekick_camera_fill_rect_raw(TDL_DISP_FRAME_BUFF_T *fb, uint16_t x, uint16_t y, uint16_t width,
                                                 uint16_t height, uint32_t color)
{
    if ((fb == NULL) || (width == 0) || (height == 0)) {
        return OPRT_OK;
    }

    if ((x >= fb->width) || (y >= fb->height)) {
        return OPRT_OK;
    }

    if ((x + width) > fb->width) {
        width = fb->width - x;
    }

    if ((y + height) > fb->height) {
        height = fb->height - y;
    }

    TDL_DISP_RECT_T rect = {
        .x0 = x,
        .y0 = y,
        .x1 = x + width - 1,
        .y1 = y + height - 1,
    };

    return tdl_disp_draw_fill(fb, &rect, color, s_display_info.is_swap);
}

static OPERATE_RET sidekick_camera_fill_canvas_rect(TDL_DISP_FRAME_BUFF_T *fb, uint16_t x, uint16_t y, uint16_t width,
                                                    uint16_t height, uint32_t color)
{
    if (s_flip_canvas) {
        x = s_canvas_width - x - width;
        y = s_canvas_height - y - height;
    }

    if (!s_rotate_canvas) {
        return sidekick_camera_fill_rect_raw(fb, x, y, width, height, color);
    }

    return sidekick_camera_fill_rect_raw(fb, s_canvas_height - y - height, x, height, width, color);
}

static const uint8_t *sidekick_camera_glyph(char letter)
{
    static const uint8_t glyph_d[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    static const uint8_t glyph_e[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    static const uint8_t glyph_n[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};

    switch (letter) {
    case 'D':
        return glyph_d;
    case 'E':
        return glyph_e;
    case 'N':
        return glyph_n;
    default:
        return NULL;
    }
}

static uint16_t sidekick_camera_text_width(uint16_t letter_count, uint16_t unit)
{
    uint16_t text_units = letter_count * SIDEKICK_GLYPH_WIDTH + (letter_count - 1) * SIDEKICK_GLYPH_SPACING;

    return text_units * unit;
}

static uint16_t sidekick_camera_fit_text_unit(uint16_t letter_count, uint16_t rect_w, uint16_t rect_h,
                                              uint16_t preferred)
{
    uint16_t text_units  = letter_count * SIDEKICK_GLYPH_WIDTH + (letter_count - 1) * SIDEKICK_GLYPH_SPACING;
    uint16_t width_unit  = (rect_w > 4) ? ((rect_w - 4) / text_units) : 1;
    uint16_t height_unit = (rect_h > 4) ? ((rect_h - 4) / SIDEKICK_GLYPH_HEIGHT) : 1;
    uint16_t unit        = preferred;

    if (unit > width_unit) {
        unit = width_unit;
    }

    if (unit > height_unit) {
        unit = height_unit;
    }

    if (unit == 0) {
        unit = 1;
    }

    return unit;
}

static void sidekick_camera_draw_block_letter(TDL_DISP_FRAME_BUFF_T *fb, char letter, uint16_t x, uint16_t y,
                                              uint16_t unit, uint32_t color)
{
    OPERATE_RET    rt    = OPRT_OK;
    const uint8_t *glyph = sidekick_camera_glyph(letter);

    if (glyph == NULL) {
        return;
    }

    for (uint8_t row = 0; row < 7; row++) {
        for (uint8_t col = 0; col < 5; col++) {
            if ((glyph[row] & (0x10 >> col)) == 0) {
                continue;
            }

            TUYA_CALL_ERR_LOG(sidekick_camera_fill_canvas_rect(fb, x + col * unit, y + row * unit, unit, unit, color));
        }
    }
}

static void sidekick_camera_draw_block_text(TDL_DISP_FRAME_BUFF_T *fb, const char *text, uint16_t x, uint16_t y,
                                            uint16_t unit, uint32_t color)
{
    while (*text != '\0') {
        sidekick_camera_draw_block_letter(fb, *text, x, y, unit, color);
        x += unit * 6;
        text++;
    }
}

static void sidekick_camera_draw_session_overlay(TDL_DISP_FRAME_BUFF_T *fb)
{
    OPERATE_RET rt        = OPRT_OK;
    uint32_t    bg        = sidekick_camera_color(0x10, 0x18, 0x28);
    uint32_t    fg        = sidekick_camera_color(0xB7, 0xE4, 0xC7);
    uint16_t    text_unit = sidekick_camera_fit_text_unit(SIDEKICK_END_LETTERS, s_end_w, s_end_h, s_end_h / 8);
    uint16_t    text_w    = sidekick_camera_text_width(SIDEKICK_END_LETTERS, text_unit);
    uint16_t    text_h    = SIDEKICK_GLYPH_HEIGHT * text_unit;
    uint16_t    text_x    = s_end_x + ((s_end_w > text_w) ? ((s_end_w - text_w) / 2) : 0);
    uint16_t    text_y    = s_end_y + ((s_end_h > text_h) ? ((s_end_h - text_h) / 2) : 0);

    TUYA_CALL_ERR_LOG(sidekick_camera_fill_canvas_rect(fb, 0, 0, s_canvas_width, s_header_height, bg));
    sidekick_camera_draw_block_text(fb, "END", text_x, text_y, text_unit, fg);
}

static OPERATE_RET sidekick_camera_frame_cb(TDL_CAMERA_HANDLE_T hdl, TDL_CAMERA_FRAME_T *frame)
{
    OPERATE_RET rt = OPRT_OK;

    (void)hdl;

    if (!s_preview_active) {
        return OPRT_OK;
    }

    TDL_DISP_FRAME_BUFF_T *fb = tdl_disp_get_free_fb(s_fb_manage);
    TUYA_CHECK_NULL_RETURN(fb, OPRT_COM_ERROR);
    sidekick_camera_apply_full_frame(fb);

    TUYA_CALL_ERR_LOG(tdl_disp_convert_yuv422_to_fb(frame->data, frame->width, frame->height, fb,
                                                    s_display_info.is_swap, SIDEKICK_CAMERA_PREVIEW_ROTATION));
    if (s_overlay_active) {
        sidekick_camera_draw_session_overlay(fb);
    }

    return tdl_disp_dev_flush(s_display_handle, fb);
}

static OPERATE_RET sidekick_camera_encoded_frame_cb(TDL_CAMERA_HANDLE_T hdl, TDL_CAMERA_FRAME_T *frame)
{
    (void)hdl;

    if ((frame == NULL) || (s_jpeg_capture.mutex == NULL) || !s_jpeg_capture.need_capture) {
        return OPRT_OK;
    }

    tal_mutex_lock(s_jpeg_capture.mutex);
    if (s_jpeg_capture.need_capture) {
        if (s_jpeg_capture.data != NULL) {
            SIDEKICK_CAMERA_FREE(s_jpeg_capture.data);
            s_jpeg_capture.data = NULL;
        }

        s_jpeg_capture.data = (uint8_t *)SIDEKICK_CAMERA_MALLOC(frame->data_len);
        if (s_jpeg_capture.data != NULL) {
            memcpy(s_jpeg_capture.data, frame->data, frame->data_len);
            s_jpeg_capture.len          = frame->data_len;
            s_jpeg_capture.need_capture = false;
        } else {
            s_jpeg_capture.len          = 0;
            s_jpeg_capture.need_capture = false;
            SIDEKICK_LOGE("camera", "failed to allocate jpeg capture len=%u", (unsigned int)frame->data_len);
        }

        if (s_jpeg_capture.sem != NULL) {
            tal_semaphore_post(s_jpeg_capture.sem);
        }
    }
    tal_mutex_unlock(s_jpeg_capture.mutex);

    return OPRT_OK;
}

static OPERATE_RET sidekick_camera_capture_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    if (s_jpeg_capture.mutex == NULL) {
        TUYA_CALL_ERR_RETURN(tal_mutex_create_init(&s_jpeg_capture.mutex));
    }

    if (s_jpeg_capture.sem == NULL) {
        TUYA_CALL_ERR_RETURN(tal_semaphore_create_init(&s_jpeg_capture.sem, 0, 1));
    }

    return rt;
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
    return sidekick_camera_preview_start_in_rect(0, 0, 0, 0);
}

static OPERATE_RET sidekick_camera_preview_start_in_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
#if defined(ENABLE_CAMERA) && (ENABLE_CAMERA == 1) && defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
    OPERATE_RET rt = sidekick_display_open();
    if (rt != OPRT_OK) {
        return rt;
    }

    (void)x;
    (void)y;
    (void)width;
    (void)height;
    s_overlay_active = false;

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
        .fps                  = SIDEKICK_CAMERA_FPS,
        .width                = SIDEKICK_CAMERA_WIDTH,
        .height               = SIDEKICK_CAMERA_HEIGHT,
        .out_fmt              = TDL_CAMERA_FMT_JPEG_YUV422_BOTH,
        .get_frame_cb         = sidekick_camera_frame_cb,
        .get_encoded_frame_cb = sidekick_camera_encoded_frame_cb,
    };

    TUYA_CALL_ERR_RETURN(sidekick_camera_capture_init());
    TUYA_CALL_ERR_RETURN(tdl_camera_dev_open(s_camera_handle, &cfg));
    s_camera_open    = true;
    s_preview_active = true;
    SIDEKICK_LOGI("camera", "camera preview started");
    return OPRT_OK;
#else
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    SIDEKICK_LOGW("camera", "camera/display support not enabled in config");
    return OPRT_OK;
#endif
}

OPERATE_RET sidekick_camera_preview_start_with_header(uint16_t canvas_width, uint16_t canvas_height, bool rotate_canvas,
                                                      bool flip_canvas, uint16_t header_height, uint16_t end_x,
                                                      uint16_t end_y, uint16_t end_w, uint16_t end_h)
{
#if defined(ENABLE_CAMERA) && (ENABLE_CAMERA == 1) && defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
    OPERATE_RET rt = sidekick_camera_preview_start_in_rect(0, 0, 0, 0);
    if (rt != OPRT_OK) {
        return rt;
    }

    s_canvas_width   = canvas_width;
    s_canvas_height  = canvas_height;
    s_rotate_canvas  = rotate_canvas;
    s_flip_canvas    = flip_canvas;
    s_header_height  = header_height;
    s_end_x          = end_x;
    s_end_y          = end_y;
    s_end_w          = end_w;
    s_end_h          = end_h;
    s_overlay_active = true;

    return OPRT_OK;
#else
    (void)canvas_width;
    (void)canvas_height;
    (void)rotate_canvas;
    (void)flip_canvas;
    (void)header_height;
    (void)end_x;
    (void)end_y;
    (void)end_w;
    (void)end_h;
    SIDEKICK_LOGW("camera", "camera/display support not enabled in config");
    return OPRT_OK;
#endif
}

OPERATE_RET sidekick_camera_preview_stop(void)
{
#if defined(ENABLE_CAMERA) && (ENABLE_CAMERA == 1) && defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
    s_preview_active = false;
    s_overlay_active = false;
    SIDEKICK_LOGI("camera", "camera preview paused");
    return OPRT_OK;
#else
    return OPRT_OK;
#endif
}

OPERATE_RET sidekick_camera_capture_jpeg(uint8_t **image_data, uint32_t *image_data_len)
{
#if defined(ENABLE_CAMERA) && (ENABLE_CAMERA == 1) && defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
    OPERATE_RET rt = OPRT_OK;

    if ((image_data == NULL) || (image_data_len == NULL)) {
        return OPRT_INVALID_PARM;
    }

    *image_data     = NULL;
    *image_data_len = 0;

    if (!s_camera_open) {
        TUYA_CALL_ERR_RETURN(sidekick_camera_preview_start());
    }

    TUYA_CALL_ERR_RETURN(sidekick_camera_capture_init());

    tal_mutex_lock(s_jpeg_capture.mutex);
    s_jpeg_capture.need_capture = true;
    tal_mutex_unlock(s_jpeg_capture.mutex);

    rt = tal_semaphore_wait(s_jpeg_capture.sem, 3000);
    if (rt != OPRT_OK) {
        tal_mutex_lock(s_jpeg_capture.mutex);
        s_jpeg_capture.need_capture = false;
        tal_mutex_unlock(s_jpeg_capture.mutex);
        SIDEKICK_LOGW("camera", "jpeg capture timeout");
        return OPRT_COM_ERROR;
    }

    tal_mutex_lock(s_jpeg_capture.mutex);
    if ((s_jpeg_capture.data == NULL) || (s_jpeg_capture.len == 0)) {
        tal_mutex_unlock(s_jpeg_capture.mutex);
        return OPRT_COM_ERROR;
    }

    *image_data = (uint8_t *)SIDEKICK_CAMERA_MALLOC(s_jpeg_capture.len);
    if (*image_data == NULL) {
        tal_mutex_unlock(s_jpeg_capture.mutex);
        return OPRT_MALLOC_FAILED;
    }

    memcpy(*image_data, s_jpeg_capture.data, s_jpeg_capture.len);
    *image_data_len = s_jpeg_capture.len;
    tal_mutex_unlock(s_jpeg_capture.mutex);

#if SIDEKICK_ENABLE_CAPTURE_SAVE
    sidekick_camera_save_jpeg_capture(*image_data, *image_data_len);
#endif

    return OPRT_OK;
#else
    (void)image_data;
    (void)image_data_len;
    return OPRT_NOT_SUPPORTED;
#endif
}

OPERATE_RET sidekick_camera_free_jpeg(uint8_t **image_data)
{
#if defined(ENABLE_CAMERA) && (ENABLE_CAMERA == 1) && defined(ENABLE_DISPLAY) && (ENABLE_DISPLAY == 1)
    if ((image_data == NULL) || (*image_data == NULL)) {
        return OPRT_INVALID_PARM;
    }

    SIDEKICK_CAMERA_FREE(*image_data);
    *image_data = NULL;
    return OPRT_OK;
#else
    (void)image_data;
    return OPRT_OK;
#endif
}
