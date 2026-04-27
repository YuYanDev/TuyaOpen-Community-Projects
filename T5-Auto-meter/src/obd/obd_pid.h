/**
 * @file obd_pid.h
 * @brief OBD-II PID descriptors and ASCII frame parsing.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __OBD_PID_H__
#define __OBD_PID_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"
#include "app_metric.h"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    APP_METRIC_E metric;     /**< target metric channel */
    uint8_t      mode;       /**< OBD mode (0x01 ‒ Show current data) */
    uint8_t      pid;        /**< PID number */
    uint8_t      bytes;      /**< number of data bytes expected (1..4) */
    const char  *cmd;        /**< exact AT-style command sent to ELM327 (no CR) */
    const char  *name;       /**< human-readable name */
} OBD_PID_DESC_T;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Resolve a PID descriptor by metric.
 * @param[in] m metric id
 * @return pointer or NULL if metric is not supported by Mode-01
 */
const OBD_PID_DESC_T *obd_pid_find(APP_METRIC_E m);

/**
 * @brief Build a polling order array based on enabled gauge mask.
 * @param[in] enabled_mask bitmask of APP_METRIC_E that the user has enabled
 * @param[out] out caller buffer, holds at most max items
 * @param[in] max maximum slots in out
 * @return number of descriptors written
 */
int obd_pid_build_poll_list(uint32_t enabled_mask,
                            const OBD_PID_DESC_T **out, int max);

/**
 * @brief Parse a single ELM327 ASCII response line into an integer scaled
 *        according to the metric's documented formula.
 *
 * @param[in] desc PID descriptor that produced the line
 * @param[in] line null-terminated trimmed response, e.g. "41 05 64"
 * @param[out] scaled_value scaled int (depending on metric, see APP_METRIC_BUS_T)
 * @return OPRT_OK on success, OPRT_INVALID_PARM on malformed line
 */
OPERATE_RET obd_pid_parse(const OBD_PID_DESC_T *desc, const char *line,
                          int32_t *scaled_value);

#ifdef __cplusplus
}
#endif

#endif /* __OBD_PID_H__ */
