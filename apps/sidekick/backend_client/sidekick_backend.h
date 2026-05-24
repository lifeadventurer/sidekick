/**
 * @file sidekick_backend.h
 * @brief SideKick firmware backend upload client.
 *
 * @copyright Copyright (c) 2026 SideKick Contributors. All Rights Reserved.
 *
 */
#ifndef SIDEKICK_BACKEND_H
#define SIDEKICK_BACKEND_H

#include "tuya_cloud_types.h"

OPERATE_RET sidekick_backend_init(void);
void        sidekick_backend_request_frame(void);
void        sidekick_backend_request_microphone(void);
void        sidekick_backend_end_session(void);

#endif /* SIDEKICK_BACKEND_H */
