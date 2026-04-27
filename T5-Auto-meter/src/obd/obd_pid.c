/**
 * @file obd_pid.c
 * @brief OBD-II PID table and ASCII parser.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 *
 * @note PID formulas reference SAE J1979 (CAN). All scaled outputs follow the
 *       semantic documented in APP_METRIC_BUS_T.
 */
#include "obd_pid.h"
#include "tal_api.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC const OBD_PID_DESC_T s_pid_table[] = {
    { APP_METRIC_WATER_TEMP,  0x01, 0x05, 1, "0105", "Coolant Temp"   },
    { APP_METRIC_INTAKE_TEMP, 0x01, 0x0F, 1, "010F", "Intake Air Temp"},
    { APP_METRIC_FUEL_LEVEL,  0x01, 0x2F, 1, "012F", "Fuel Level"     },
    { APP_METRIC_VOLTAGE,     0x01, 0x42, 2, "0142", "Module Voltage" },
    { APP_METRIC_OIL_TEMP,    0x01, 0x5C, 1, "015C", "Engine Oil Temp"},
    { APP_METRIC_BOOST,       0x01, 0x70, 4, "0170", "Boost Pressure" },
};

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Convert a single hex char to its numeric value.
 * @return -1 on invalid char
 */
STATIC int __hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

/**
 * @brief Parse hex-pairs from a string into a byte array, skipping whitespace.
 * @return number of bytes parsed
 */
STATIC int __parse_hex_bytes(const char *line, uint8_t *out, int max_out)
{
    int n = 0;
    while (*line && n < max_out) {
        while (*line && isspace((unsigned char)*line)) {
            line++;
        }
        if (!*line) {
            break;
        }
        int hi = __hex_nibble(*line++);
        if (hi < 0 || !*line) {
            break;
        }
        int lo = __hex_nibble(*line++);
        if (lo < 0) {
            break;
        }
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

/**
 * @brief Lookup PID by metric.
 */
const OBD_PID_DESC_T *obd_pid_find(APP_METRIC_E m)
{
    for (size_t i = 0; i < sizeof(s_pid_table) / sizeof(s_pid_table[0]); i++) {
        if (s_pid_table[i].metric == m) {
            return &s_pid_table[i];
        }
    }
    return NULL;
}

/**
 * @brief Build polling list from enabled mask.
 */
int obd_pid_build_poll_list(uint32_t enabled_mask,
                            const OBD_PID_DESC_T **out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    int n = 0;
    for (size_t i = 0; i < sizeof(s_pid_table) / sizeof(s_pid_table[0]) && n < max; i++) {
        if (enabled_mask & (1u << s_pid_table[i].metric)) {
            out[n++] = &s_pid_table[i];
        }
    }
    return n;
}

/**
 * @brief Parse ELM327 line into scaled engineering integer.
 *
 * Expected line format: "41 PP A B C D" where PP is the requested PID and
 * A..D are the data bytes. Whitespace and the prompt '>' are stripped before
 * calling. Lines like "NO DATA" / "?" / "STOPPED" are rejected.
 */
OPERATE_RET obd_pid_parse(const OBD_PID_DESC_T *desc, const char *line,
                          int32_t *scaled_value)
{
    if (desc == NULL || line == NULL || scaled_value == NULL) {
        return OPRT_INVALID_PARM;
    }

    /* Reject non-data lines */
    if (strstr(line, "NO DATA") || strstr(line, "no data") ||
        strstr(line, "STOPPED") || strstr(line, "ERROR") ||
        strstr(line, "?") == line) {
        return OPRT_NOT_FOUND;
    }

    uint8_t bytes[16] = {0};
    int n = __parse_hex_bytes(line, bytes, sizeof(bytes));
    if (n < 2 + desc->bytes) {
        return OPRT_INVALID_PARM;
    }

    /* Validate echo of mode+0x40 and PID */
    int idx = 0;
    if (bytes[0] == 0x40 + desc->mode && bytes[1] == desc->pid) {
        idx = 2;
    } else {
        /* Some adapters return the first frame number prefix (0x10) for
         * multi-frame; in that case shift past until we find 0x4X PP */
        for (int i = 0; i < n - 1; i++) {
            if (bytes[i] == 0x40 + desc->mode && bytes[i + 1] == desc->pid) {
                idx = i + 2;
                break;
            }
        }
        if (idx == 0) {
            return OPRT_INVALID_PARM;
        }
    }

    if (n < idx + desc->bytes) {
        return OPRT_INVALID_PARM;
    }

    uint8_t a = bytes[idx];
    uint8_t b = (desc->bytes >= 2) ? bytes[idx + 1] : 0;
    uint8_t c = (desc->bytes >= 3) ? bytes[idx + 2] : 0;
    uint8_t d = (desc->bytes >= 4) ? bytes[idx + 3] : 0;
    (void)c;
    (void)d;

    int32_t v = 0;
    switch (desc->metric) {
    case APP_METRIC_WATER_TEMP:
    case APP_METRIC_INTAKE_TEMP:
    case APP_METRIC_OIL_TEMP:
        /* °C = A - 40, output 0.1 °C */
        v = ((int32_t)a - 40) * 10;
        break;

    case APP_METRIC_FUEL_LEVEL:
        /* % = 100/255 * A, output 0.1 % */
        v = (int32_t)a * 1000 / 255;
        break;

    case APP_METRIC_VOLTAGE:
        /* V = (256*A + B) / 1000 → output mV directly */
        v = ((int32_t)a << 8) | b;
        break;

    case APP_METRIC_BOOST:
        /* PID 0x70 returns boost pressure control parameters; semantics
         * vary by manufacturer. Best-effort: treat byte D as commanded
         * boost in kPa, signed offset by 128. Output 0.1 kPa.
         * For more reliable boost on petrol engines, use MAP - BARO. */
        v = ((int32_t)d - 128) * 10;
        break;

    default:
        return OPRT_INVALID_PARM;
    }

    *scaled_value = v;
    return OPRT_OK;
}
