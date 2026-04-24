/**
 * @file win95_ntp.c
 * @brief Minimal UDP NTP client for Win95 desktop clock sync.
 *        Sends a 48-byte NTP client packet to pool.ntp.org:123,
 *        extracts the transmit timestamp, and calls tal_time_set_posix().
 * @version 1.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#include "win95_ntp.h"

#include "tal_api.h"
#include "tal_network.h"
#include "tal_time_service.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define NTP_SERVER      "pool.ntp.org"
#define NTP_PORT        123
#define NTP_TIMEOUT_MS  5000
/* Seconds between NTP epoch (Jan 1 1900) and POSIX epoch (Jan 1 1970) */
#define NTP_DELTA       2208988800UL

/* Fallback: time.cloudflare.com 162.159.200.1 */
#define NTP_FALLBACK_IP ((162UL << 24) | (159UL << 16) | (200UL << 8) | 1UL)

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC volatile BOOL_T  s_synced     = FALSE;
STATIC THREAD_HANDLE    s_ntp_thread = NULL;

/* ---------------------------------------------------------------------------
 * NTP thread
 * --------------------------------------------------------------------------- */
STATIC VOID_T __ntp_thread(VOID_T *arg)
{
    (VOID_T)arg;
    uint8_t pkt[48];
    uint8_t resp[48];

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B; /* LI=0, VN=3, Mode=3 (client) */

    TUYA_IP_ADDR_T addr = 0;
    if (tal_net_gethostbyname(NTP_SERVER, &addr) != OPRT_OK || addr == 0) {
        PR_WARN("NTP: DNS failed for %s, using fallback IP", NTP_SERVER);
        addr = (TUYA_IP_ADDR_T)NTP_FALLBACK_IP;
    }

    INT32_T fd = tal_net_socket_create(PROTOCOL_UDP);
    if (fd < 0) {
        PR_ERR("NTP: socket create failed");
        goto done;
    }

    tal_net_set_timeout(fd, NTP_TIMEOUT_MS, TRANS_RECV);

    INT32_T sent = tal_net_send_to(fd, pkt, 48, addr, NTP_PORT);
    if (sent != 48) {
        PR_ERR("NTP: send failed (%d)", sent);
        goto close;
    }

    TUYA_IP_ADDR_T from_addr = 0;
    uint16_t from_port = 0;
    INT32_T got = tal_net_recvfrom(fd, resp, 48, &from_addr, &from_port);
    if (got < 44) {
        PR_ERR("NTP: short response (%d)", got);
        goto close;
    }

    /* Transmit timestamp: big-endian uint32 at byte 40 (seconds since 1900) */
    uint32_t ntp_secs = ((uint32_t)resp[40] << 24)
                      | ((uint32_t)resp[41] << 16)
                      | ((uint32_t)resp[42] <<  8)
                      |  (uint32_t)resp[43];

    if (ntp_secs > NTP_DELTA) {
        TIME_T posix = (TIME_T)(ntp_secs - NTP_DELTA);
        if (tal_time_set_posix(posix, 0) == OPRT_OK) {
            s_synced = TRUE;
            PR_NOTICE("NTP sync OK: posix=%lu", (unsigned long)posix);
        }
    } else {
        PR_ERR("NTP: implausible timestamp %lu", (unsigned long)ntp_secs);
    }

close:
    tal_net_close(fd);
done:
    s_ntp_thread = NULL;
    tal_thread_delete(NULL);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
void win95_ntp_trigger(void)
{
    if (s_synced || s_ntp_thread != NULL) {
        return;
    }
    THREAD_CFG_T cfg = {2048, 3, "w95_ntp"};
    OPERATE_RET rt = tal_thread_create_and_start(&s_ntp_thread, NULL, NULL,
                                                  __ntp_thread, NULL, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("NTP: thread create failed: %d", rt);
        s_ntp_thread = NULL;
    }
}

bool win95_ntp_synced(void)
{
    return (bool)s_synced;
}
