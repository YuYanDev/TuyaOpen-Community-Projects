/**
 * @file app_mock.h
 * @brief Mock data generator that emulates plausible OBD telemetry for offline UI testing.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __APP_MOCK_H__
#define __APP_MOCK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Start the mock generator task. Idempotent.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET app_mock_start(VOID_T);

/**
 * @brief Stop the mock generator task. Idempotent.
 * @return OPRT_OK on success
 */
OPERATE_RET app_mock_stop(VOID_T);

/**
 * @brief Whether mock task is currently running.
 * @return TRUE if running
 */
BOOL_T app_mock_is_running(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MOCK_H__ */
