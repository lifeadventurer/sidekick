/**
 * @file sidekick_config.h
 * @brief SideKick application compile-time configuration.
 *
 * @copyright Copyright (c) 2026 SideKick Contributors. All Rights Reserved.
 *
 */
#ifndef SIDEKICK_CONFIG_H
#define SIDEKICK_CONFIG_H

#include "sidekick_device_config.h"

#define SIDEKICK_APP_NAME "sidekick"

#define SIDEKICK_CAMERA_WIDTH  480
#define SIDEKICK_CAMERA_HEIGHT 480
#define SIDEKICK_CAMERA_FPS    20

#define SIDEKICK_UI_POLL_MS                50
#define SIDEKICK_TUTOR_TICK_MS             1000
#define SIDEKICK_FRAME_UPLOAD_INTERVAL_SEC 10
#define SIDEKICK_TTS_OUTPUT_FORMAT         "pcm_16000"

#define SIDEKICK_MIC_SAMPLE_RATE     16000
#define SIDEKICK_MIC_CHANNELS        1
#define SIDEKICK_MIC_BITS_PER_SAMPLE 16
#define SIDEKICK_MIC_CAPTURE_SECONDS 4
#define SIDEKICK_MIC_UPLOAD_MIN_MS   700

#define SIDEKICK_DEFAULT_TUTOR_MODE    SIDEKICK_TUTOR_MODE_HINT
#define SIDEKICK_ENABLE_CAMERA_PREVIEW 0
#define SIDEKICK_ENABLE_STARTUP_CHIME  1

/* Save each JPEG capture to disk for debugging (best-effort; does not fail capture). */
#define SIDEKICK_ENABLE_CAPTURE_SAVE 1

#endif /* SIDEKICK_CONFIG_H */
