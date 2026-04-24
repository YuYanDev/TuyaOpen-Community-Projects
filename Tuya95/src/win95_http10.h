/**
 * @file win95_http10.h
 * @brief Retro HTTP/1.0 client over raw sockets (for IE 1.0/2.0 simulation)
 * @version 1.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __WIN95_HTTP10_H__
#define __WIN95_HTTP10_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

#define WIN95_HTTP_BODY_MAX     (16 * 1024)
/* PAGE_BUF_MAX: pre-alloc size for HTTP/1.0 recv (body + headers headroom). */
#define WIN95_HTTP_PAGE_BUF_MAX (WIN95_HTTP_BODY_MAX + 4096)
/* IMG_BODY_MAX: pre-alloc size per image slot (retro images are small). */
#define WIN95_HTTP_IMG_BODY_MAX (8 * 1024)

#define WIN95_HTTP_HOST_MAX     128
#define WIN95_HTTP_PATH_MAX     256
#define WIN95_HTTP_CTYPE_MAX    96
#define WIN95_HTTP_COOKIE_MAX   8
#define WIN95_HTTP_COOKIE_LEN   160

typedef struct {
    UINT16_T status_code;
    CHAR_T  *body;          /* body data; points into body_buf if pre-allocated */
    UINT32_T body_len;      /* actual data length */
    CHAR_T  *body_buf;      /* pre-allocated buffer (NULL = dynamic tal_malloc) */
    UINT32_T body_buf_cap;  /* capacity of body_buf; 0 if dynamic */
    CHAR_T   location[256]; /* set on 30x redirects */
    BOOL_T   has_location;
    CHAR_T   content_type[WIN95_HTTP_CTYPE_MAX];
    CHAR_T   set_cookie[WIN95_HTTP_COOKIE_MAX][WIN95_HTTP_COOKIE_LEN];
    UINT8_T  set_cookie_count;
} WIN95_HTTP_RESP_T;

/**
 * @brief Synchronous HTTP/1.0 GET. Closes the connection after response.
 *        If out->body_buf is non-NULL and out->body_buf_cap > 0, that buffer
 *        is used for recv and body will NOT be freed by win95_http10_free().
 *        Otherwise allocates with tal_malloc; caller must call win95_http10_free().
 */
OPERATE_RET win95_http10_get(CONST CHAR_T *host, UINT16_T port,
                              CONST CHAR_T *path, UINT32_T timeout_ms,
                              WIN95_HTTP_RESP_T *out);

/**
 * @brief Free the dynamic body buffer inside a response.
 *        Does NOT free body if body_buf is set (externally managed).
 *        Always safe to call multiple times.
 */
VOID_T win95_http10_free(WIN95_HTTP_RESP_T *resp);

/**
 * @brief Parse an http URL into host, port, path.
 */
OPERATE_RET win95_http10_parse_url(CONST CHAR_T *url,
                                    CHAR_T *host_buf, UINT32_T host_cap,
                                    UINT16_T *port_out,
                                    CHAR_T *path_buf, UINT32_T path_cap);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_HTTP10_H__ */
