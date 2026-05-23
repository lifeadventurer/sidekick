#include "sidekick_hardware.h"

#include "board_com_api.h"
#include "sidekick_log.h"

static const char *TAG = "hardware";

OPERATE_RET sidekick_hardware_init(void)
{
    OPERATE_RET rt = board_register_hardware();
    if (rt != OPRT_OK) {
        SIDEKICK_LOGE(TAG, "board hardware registration failed: %d", rt);
        return rt;
    }

    SIDEKICK_LOGI(TAG, "board hardware registered");
    return OPRT_OK;
}
