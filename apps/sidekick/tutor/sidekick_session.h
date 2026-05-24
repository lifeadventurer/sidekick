#ifndef SIDEKICK_SESSION_H
#define SIDEKICK_SESSION_H

#include "tuya_cloud_types.h"

typedef enum {
    SIDEKICK_SESSION_IDLE = 0,
    SIDEKICK_SESSION_OBSERVING,
    SIDEKICK_SESSION_LISTENING,
    SIDEKICK_SESSION_THINKING,
    SIDEKICK_SESSION_RESPONDING,
} SIDEKICK_SESSION_STATE_E;

typedef enum {
    SIDEKICK_TUTOR_MODE_ACTIVE = 0,
    SIDEKICK_TUTOR_MODE_HINT,
    SIDEKICK_TUTOR_MODE_SUMMARY,
} SIDEKICK_TUTOR_MODE_E;

OPERATE_RET              sidekick_session_init(void);
void                     sidekick_session_tick(void);
SIDEKICK_SESSION_STATE_E sidekick_session_state(void);
bool                     sidekick_session_is_active(void);
SIDEKICK_TUTOR_MODE_E    sidekick_session_mode(void);
const char              *sidekick_session_mode_name(SIDEKICK_TUTOR_MODE_E mode);
void                     sidekick_session_start(void);
void                     sidekick_session_end(void);
void                     sidekick_session_set_mode(SIDEKICK_TUTOR_MODE_E mode);
void                     sidekick_session_next_mode(void);

#endif /* SIDEKICK_SESSION_H */
