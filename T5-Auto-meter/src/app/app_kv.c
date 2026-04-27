/**
 * @file app_kv.c
 * @brief Persisted user preferences using TuyaOS tal_kv.
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "app_kv.h"
#include "app_config.h"
#include "tal_api.h"
#include "tal_kv.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define KV_KEY_PREFS    "auto_meter.prefs"

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC APP_PREFS_T s_prefs;
STATIC BOOL_T      s_inited = FALSE;
STATIC BOOL_T      s_kv_ready = FALSE;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Reset preferences to factory defaults.
 * @return none
 */
STATIC VOID_T __prefs_default(APP_PREFS_T *p)
{
    memset(p, 0, sizeof(*p));
    /* Enable everything except oil pressure (OEM specific) by default */
    p->gauge_enabled_mask = (1u << APP_METRIC_WATER_TEMP) | (1u << APP_METRIC_OIL_TEMP) |
                            (1u << APP_METRIC_INTAKE_TEMP) | (1u << APP_METRIC_FUEL_LEVEL) |
                            (1u << APP_METRIC_VOLTAGE) | (1u << APP_METRIC_BOOST) |
                            (1u << APP_METRIC_G_FORCE);
    p->current_gauge = APP_METRIC_WATER_TEMP;
    p->brightness_pct = APP_DEFAULT_BRIGHTNESS_PCT;
    p->unit_temp = APP_UNIT_TEMP_C;
    p->unit_press = APP_UNIT_PRESS_KPA;
    p->mock_enabled = 0;
    p->bound_addr_valid = 0;
}

/**
 * @brief Initialize KV system & load preferences.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET app_kv_init(VOID_T)
{
    if (s_inited) {
        return OPRT_OK;
    }

    __prefs_default(&s_prefs);

    if (!s_kv_ready) {
        tal_kv_cfg_t kv_cfg = {
            .seed = "t5automkvseed-01",
            .key  = "t5automkvkey-001",
        };
        int rc = tal_kv_init(&kv_cfg);
        if (rc == OPRT_OK) {
            s_kv_ready = TRUE;
        } else {
            PR_WARN("tal_kv_init failed: %d, prefs runtime-only", rc);
        }
    }

    if (s_kv_ready) {
        uint8_t *buf = NULL;
        size_t   len = 0;
        if (tal_kv_get(KV_KEY_PREFS, &buf, &len) == OPRT_OK && buf != NULL) {
            if (len == sizeof(APP_PREFS_T)) {
                memcpy(&s_prefs, buf, sizeof(APP_PREFS_T));
                /* Sanitize fields that may be out of range after firmware update */
                if (s_prefs.brightness_pct > 100) {
                    s_prefs.brightness_pct = APP_DEFAULT_BRIGHTNESS_PCT;
                }
                if (s_prefs.current_gauge >= APP_METRIC_COUNT) {
                    s_prefs.current_gauge = APP_METRIC_WATER_TEMP;
                }
            } else {
                PR_WARN("prefs size mismatch (got %u, want %u), reset to defaults",
                        (unsigned)len, (unsigned)sizeof(APP_PREFS_T));
            }
            tal_kv_free(buf);
        }
    }

    s_inited = TRUE;
    return OPRT_OK;
}

/**
 * @brief Get a const pointer to the in-memory preferences.
 * @return pointer (never NULL after init)
 */
const APP_PREFS_T *app_kv_prefs(VOID_T)
{
    if (!s_inited) {
        app_kv_init();
    }
    return &s_prefs;
}

/**
 * @brief Persist current preferences to KV storage.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET app_kv_save(VOID_T)
{
    if (!s_inited) {
        return OPRT_NOT_FOUND;
    }
    if (!s_kv_ready) {
        return OPRT_OK; /* runtime-only mode: silently no-op */
    }
    int rc = tal_kv_set(KV_KEY_PREFS, (const uint8_t *)&s_prefs, sizeof(s_prefs));
    if (rc != OPRT_OK) {
        PR_ERR("tal_kv_set prefs failed: %d", rc);
        return rc;
    }
    return OPRT_OK;
}

/**
 * @brief Toggle whether a given gauge is enabled.
 */
OPERATE_RET app_kv_set_gauge_enabled(APP_METRIC_E m, BOOL_T enabled)
{
    if (!s_inited) {
        app_kv_init();
    }
    if (m >= APP_METRIC_COUNT) {
        return OPRT_INVALID_PARM;
    }
    if (enabled) {
        s_prefs.gauge_enabled_mask |= (1u << m);
    } else {
        s_prefs.gauge_enabled_mask &= ~(1u << m);
        /* Make sure at least one gauge stays enabled */
        if (s_prefs.gauge_enabled_mask == 0) {
            s_prefs.gauge_enabled_mask = (1u << APP_METRIC_WATER_TEMP);
        }
    }
    return app_kv_save();
}

/**
 * @brief Set currently displayed gauge.
 */
OPERATE_RET app_kv_set_current_gauge(APP_METRIC_E m)
{
    if (!s_inited) {
        app_kv_init();
    }
    if (m >= APP_METRIC_COUNT) {
        return OPRT_INVALID_PARM;
    }
    s_prefs.current_gauge = (uint8_t)m;
    return app_kv_save();
}

/**
 * @brief Toggle mock mode.
 */
OPERATE_RET app_kv_set_mock_enabled(BOOL_T enabled)
{
    if (!s_inited) {
        app_kv_init();
    }
    s_prefs.mock_enabled = enabled ? 1 : 0;
    return app_kv_save();
}

/**
 * @brief Set brightness percentage (clamped to 0..100).
 */
OPERATE_RET app_kv_set_brightness(uint8_t pct)
{
    if (!s_inited) {
        app_kv_init();
    }
    if (pct > 100) {
        pct = 100;
    }
    s_prefs.brightness_pct = pct;
    return app_kv_save();
}

/**
 * @brief Save bound BLE peer address.
 */
OPERATE_RET app_kv_set_bound_addr(const uint8_t *addr)
{
    if (addr == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (!s_inited) {
        app_kv_init();
    }
    memcpy(s_prefs.bound_addr, addr, 6);
    s_prefs.bound_addr_valid = 1;
    return app_kv_save();
}

/**
 * @brief Forget bound device.
 */
OPERATE_RET app_kv_clear_bound_addr(VOID_T)
{
    if (!s_inited) {
        app_kv_init();
    }
    memset(s_prefs.bound_addr, 0, sizeof(s_prefs.bound_addr));
    s_prefs.bound_addr_valid = 0;
    return app_kv_save();
}
