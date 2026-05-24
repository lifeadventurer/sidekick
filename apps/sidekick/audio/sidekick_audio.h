/**
 * @file sidekick_audio.h
 * @brief SideKick audio input and speaker playback helpers.
 *
 * @copyright Copyright (c) 2026 SideKick Contributors. All Rights Reserved.
 *
 */
#ifndef SIDEKICK_AUDIO_H
#define SIDEKICK_AUDIO_H

#include "tuya_cloud_types.h"

OPERATE_RET sidekick_audio_input_start(void);
OPERATE_RET sidekick_audio_play_startup_chime(void);
OPERATE_RET sidekick_audio_play_pcm(const uint8_t *pcm, uint32_t len);
uint32_t    sidekick_audio_frame_count(void);

#endif /* SIDEKICK_AUDIO_H */
