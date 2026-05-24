#ifndef SIDEKICK_CAMERA_H
#define SIDEKICK_CAMERA_H

#include "tuya_cloud_types.h"

OPERATE_RET sidekick_camera_preview_start(void);
OPERATE_RET sidekick_camera_preview_start_with_header(uint16_t canvas_width, uint16_t canvas_height, bool rotate_canvas,
                                                      bool flip_canvas, uint16_t header_height, uint16_t end_x,
                                                      uint16_t end_y, uint16_t end_w, uint16_t end_h);
OPERATE_RET sidekick_camera_preview_stop(void);

#endif /* SIDEKICK_CAMERA_H */
