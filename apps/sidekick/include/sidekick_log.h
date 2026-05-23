#ifndef SIDEKICK_LOG_H
#define SIDEKICK_LOG_H

#include "tal_log.h"

#define SIDEKICK_LOGI(tag, fmt, ...) PR_NOTICE("[%s] " fmt, tag, ##__VA_ARGS__)
#define SIDEKICK_LOGW(tag, fmt, ...) PR_WARN("[%s] " fmt, tag, ##__VA_ARGS__)
#define SIDEKICK_LOGE(tag, fmt, ...) PR_ERR("[%s] " fmt, tag, ##__VA_ARGS__)
#define SIDEKICK_LOGD(tag, fmt, ...) PR_DEBUG("[%s] " fmt, tag, ##__VA_ARGS__)

#endif /* SIDEKICK_LOG_H */
