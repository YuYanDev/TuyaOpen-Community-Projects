/**
 * @file win95_http10.c
 * @brief Raw-socket HTTP/1.0 client. Sends 'GET /path HTTP/1.0', handles Host,
 *        simple Content-Length or connection-close body delivery, and one-hop
 *        3xx Location redirect. Intentionally minimalist, to match the era.
 * @version 1.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#include "win95_http10.h"
#include "tal_api.h"
#include "tal_network.h"
#include "tal_wifi.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define HTTP_RECV_CHUNK     1024
#define HTTP_HEADER_MAX     4096

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Case-insensitive strncasecmp.
 */
STATIC INT32_T __strncasecmp(CONST CHAR_T *a, CONST CHAR_T *b, UINT32_T n)
{
    for (UINT32_T i = 0; i < n; i++) {
        CHAR_T ca = a[i];
        CHAR_T cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (CHAR_T)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (CHAR_T)(cb + 32);
        if (ca != cb) {
            return (UCHAR_T)ca - (UCHAR_T)cb;
        }
        if (ca == '\0') {
            return 0;
        }
    }
    return 0;
}

OPERATE_RET win95_http10_parse_url(CONST CHAR_T *url,
                                    CHAR_T *host_buf, UINT32_T host_cap,
                                    UINT16_T *port_out,
                                    CHAR_T *path_buf, UINT32_T path_cap)
{
    if (url == NULL || host_buf == NULL || port_out == NULL || path_buf == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (host_cap == 0 || path_cap == 0) {
        return OPRT_INVALID_PARM;
    }

    host_buf[0] = '\0';
    path_buf[0] = '/';
    path_buf[1] = '\0';
    *port_out = 80;

    CONST CHAR_T *p = url;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        /* We cannot do TLS - callers can fall back. */
        p += 8;
        *port_out = 443;
    }

    CONST CHAR_T *slash = strchr(p, '/');
    CONST CHAR_T *colon = strchr(p, ':');
    if (colon && slash && colon > slash) {
        colon = NULL;
    }

    UINT32_T hlen = 0;
    if (colon) {
        hlen = (UINT32_T)(colon - p);
    } else if (slash) {
        hlen = (UINT32_T)(slash - p);
    } else {
        hlen = (UINT32_T)strlen(p);
    }
    if (hlen >= host_cap) {
        hlen = host_cap - 1;
    }
    memcpy(host_buf, p, hlen);
    host_buf[hlen] = '\0';

    if (colon) {
        UINT32_T port_val = 0;
        CONST CHAR_T *pp = colon + 1;
        while (*pp >= '0' && *pp <= '9') {
            port_val = port_val * 10 + (UINT32_T)(*pp - '0');
            pp++;
        }
        if (port_val > 0 && port_val < 65536) {
            *port_out = (UINT16_T)port_val;
        }
    }

    if (slash) {
        UINT32_T plen = (UINT32_T)strlen(slash);
        if (plen >= path_cap) {
            plen = path_cap - 1;
        }
        memcpy(path_buf, slash, plen);
        path_buf[plen] = '\0';
    }

    return OPRT_OK;
}

/**
 * @brief Close socket if > 0.
 */
STATIC VOID_T __sock_close(INT32_T fd)
{
    if (fd > 0) {
        tal_net_close(fd);
    }
}

/**
 * @brief Parse the HTTP status line "HTTP/1.x NNN ...". Writes status code.
 * @return TRUE if parsed.
 */
STATIC BOOL_T __parse_status_line(CONST CHAR_T *line, UINT16_T *code)
{
    if (__strncasecmp(line, "HTTP/1.", 7) != 0) {
        return FALSE;
    }
    CONST CHAR_T *sp = strchr(line, ' ');
    if (sp == NULL) {
        return FALSE;
    }
    UINT32_T c = 0;
    CONST CHAR_T *p = sp + 1;
    while (*p >= '0' && *p <= '9') {
        c = c * 10 + (UINT32_T)(*p - '0');
        p++;
    }
    *code = (UINT16_T)c;
    return TRUE;
}

STATIC VOID_T __copy_trimmed(CHAR_T *dst, UINT32_T cap,
                              CONST CHAR_T *src, UINT32_T len)
{
    UINT32_T start = 0;
    while (start < len && (src[start] == ' ' || src[start] == '\t')) {
        start++;
    }
    while (len > start &&
           (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\r')) {
        len--;
    }
    if (cap == 0) return;
    if (len <= start) {
        dst[0] = '\0';
        return;
    }
    UINT32_T n = len - start;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src + start, n);
    dst[n] = '\0';
}

STATIC VOID_T __parse_headers_block(CONST CHAR_T *hdr, UINT32_T hdr_len,
                                     WIN95_HTTP_RESP_T *out)
{
    UINT32_T i = 0;
    while (i < hdr_len) {
        UINT32_T line_start = i;
        while (i < hdr_len && hdr[i] != '\n') {
            i++;
        }
        UINT32_T line_end = i;
        if (i < hdr_len && hdr[i] == '\n') i++;
        if (line_end > line_start && hdr[line_end - 1] == '\r') {
            line_end--;
        }
        if (line_end <= line_start) {
            continue;
        }

        CONST CHAR_T *line = &hdr[line_start];
        UINT32_T line_len = line_end - line_start;
        CONST CHAR_T *colon = memchr(line, ':', line_len);
        if (!colon) {
            continue;
        }
        UINT32_T key_len = (UINT32_T)(colon - line);
        CONST CHAR_T *val = colon + 1;
        UINT32_T val_len = line_len - key_len - 1;

        if (key_len == 8 && __strncasecmp(line, "Location", 8) == 0) {
            __copy_trimmed(out->location, sizeof(out->location), val, val_len);
            out->has_location = (out->location[0] != '\0');
        } else if (key_len == 12 && __strncasecmp(line, "Content-Type", 12) == 0) {
            __copy_trimmed(out->content_type, sizeof(out->content_type), val, val_len);
            CHAR_T *semi = strchr(out->content_type, ';');
            if (semi) {
                *semi = '\0';
            }
        } else if (key_len == 10 && __strncasecmp(line, "Set-Cookie", 10) == 0) {
            if (out->set_cookie_count < WIN95_HTTP_COOKIE_MAX) {
                __copy_trimmed(out->set_cookie[out->set_cookie_count],
                               WIN95_HTTP_COOKIE_LEN, val, val_len);
                if (out->set_cookie[out->set_cookie_count][0] != '\0') {
                    out->set_cookie_count++;
                }
            }
        }
    }
}

/**
 * @brief Find first occurrence of needle in hay (bounded, not null-terminated).
 */
STATIC INT32_T __memfind(CONST CHAR_T *hay, UINT32_T hay_len,
                          CONST CHAR_T *needle, UINT32_T needle_len)
{
    if (needle_len == 0 || hay_len < needle_len) {
        return -1;
    }
    for (UINT32_T i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(&hay[i], needle, needle_len) == 0) {
            return (INT32_T)i;
        }
    }
    return -1;
}

STATIC VOID_T __best_effort_bind_station_ip(INT32_T fd)
{
    NW_IP_S ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    if (tal_wifi_get_ip(WF_STATION, &ip_info) == OPRT_OK && ip_info.ip[0] != '\0') {
        TUYA_IP_ADDR_T bind_addr = tal_net_str2addr(ip_info.ip);
        if (bind_addr != 0) {
            tal_net_bind(fd, bind_addr, 0);
        }
    }
}

/**
 * @brief Do one GET, no redirect handling. Used internally.
 */
STATIC OPERATE_RET __http10_get_once(CONST CHAR_T *host, UINT16_T port,
                                       CONST CHAR_T *path, UINT32_T timeout_ms,
                                       WIN95_HTTP_RESP_T *out)
{
    INT32_T fd = -1;
    CHAR_T *buf = NULL;
    UINT32_T buf_cap = HTTP_RECV_CHUNK * 4;
    UINT32_T buf_len = 0;
    OPERATE_RET rt = OPRT_OK;

    /* Preserve caller-provided pre-allocated buffer across the struct reset. */
    CHAR_T *const ext_buf = out->body_buf;
    UINT32_T const ext_cap = out->body_buf_cap;
    memset(out, 0, sizeof(*out));
    out->body_buf     = ext_buf;
    out->body_buf_cap = ext_cap;

    TUYA_IP_ADDR_T addr = 0;
    if (tal_net_gethostbyname(host, &addr) != OPRT_OK || addr == 0) {
        PR_ERR("DNS failed for %s", host);
        return OPRT_COM_ERROR;
    }

    fd = tal_net_socket_create(PROTOCOL_TCP);
    if (fd < 0) {
        return OPRT_SOCK_ERR;
    }

    __best_effort_bind_station_ip(fd);
    tal_net_set_timeout(fd, (INT32_T)timeout_ms, TRANS_SEND);
    tal_net_set_timeout(fd, (INT32_T)timeout_ms, TRANS_RECV);

    if (tal_net_connect(fd, addr, port) != 0) {
        PR_ERR("connect failed: %s:%u", host, port);
        __sock_close(fd);
        return OPRT_SOCK_CONN_ERR;
    }

    /* Build + send the request. */
    CHAR_T req[512];
    INT32_T n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: Mozilla/2.0 (compatible; MSIE 2.0; TuyaOS95; 16bit)\r\n"
        "Accept: text/html, text/plain, */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);
    if (n <= 0 || n >= (INT32_T)sizeof(req)) {
        __sock_close(fd);
        return OPRT_COM_ERROR;
    }
    if (tal_net_send(fd, req, (UINT32_T)n) != n) {
        __sock_close(fd);
        return OPRT_SOCK_ERR;
    }

    /* Use caller's pre-allocated buffer if provided; otherwise malloc. */
    if (ext_buf && ext_cap > 0) {
        buf     = ext_buf;
        buf_cap = ext_cap;
    } else {
        buf = (CHAR_T *)tal_malloc(buf_cap);
        if (buf == NULL) {
            __sock_close(fd);
            return OPRT_MALLOC_FAILED;
        }
    }

    /* Recv loop. HTTP/1.0 with Connection: close means read until EOF. */
    for (;;) {
        if (buf_len + HTTP_RECV_CHUNK + 1 > buf_cap) {
            if (ext_buf) {
                break; /* fixed-size external buffer full */
            }
            if (buf_cap >= WIN95_HTTP_BODY_MAX + HTTP_HEADER_MAX) {
                /* Hit the hard cap - stop reading to protect PSRAM. */
                break;
            }
            UINT32_T new_cap = buf_cap * 2;
            if (new_cap > WIN95_HTTP_BODY_MAX + HTTP_HEADER_MAX) {
                new_cap = WIN95_HTTP_BODY_MAX + HTTP_HEADER_MAX;
            }
            CHAR_T *nb = (CHAR_T *)tal_realloc(buf, new_cap);
            if (nb == NULL) {
                break;
            }
            buf = nb;
            buf_cap = new_cap;
        }

        INT32_T got = tal_net_recv(fd, &buf[buf_len],
                                    buf_cap - buf_len - 1);
        if (got <= 0) {
            break;
        }
        buf_len += (UINT32_T)got;
        if (buf_len >= buf_cap - 1) {
            break;
        }
    }
    buf[buf_len] = '\0';
    __sock_close(fd);

    if (buf_len == 0) {
        if (!ext_buf) tal_free(buf);
        return OPRT_COM_ERROR;
    }

    /* Split header / body at first \r\n\r\n (fallback \n\n). */
    INT32_T sep = __memfind(buf, buf_len, "\r\n\r\n", 4);
    UINT32_T body_ofs = 0;
    if (sep >= 0) {
        body_ofs = (UINT32_T)sep + 4;
    } else {
        sep = __memfind(buf, buf_len, "\n\n", 2);
        if (sep >= 0) {
            body_ofs = (UINT32_T)sep + 2;
        } else {
            body_ofs = buf_len;
        }
    }

    /* Parse status line (first line of header). */
    UINT16_T status_code = 0;
    __parse_status_line(buf, &status_code);
    out->status_code = status_code;

    if (sep >= 0) {
        __parse_headers_block(buf, (UINT32_T)sep, out);
    }

    UINT32_T body_len = buf_len - body_ofs;
    if (body_len > WIN95_HTTP_BODY_MAX) {
        body_len = WIN95_HTTP_BODY_MAX;
    }

    if (body_ofs > 0 && body_len > 0) {
        memmove(buf, &buf[body_ofs], body_len);
    }
    buf[body_len] = '\0';

    out->body = buf;
    out->body_len = body_len;
    return rt;
}

OPERATE_RET win95_http10_get(CONST CHAR_T *host, UINT16_T port,
                              CONST CHAR_T *path, UINT32_T timeout_ms,
                              WIN95_HTTP_RESP_T *out)
{
    if (host == NULL || path == NULL || out == NULL) {
        return OPRT_INVALID_PARM;
    }

    OPERATE_RET rt = __http10_get_once(host, port, path, timeout_ms, out);
    if (rt != OPRT_OK) {
        return rt;
    }

    /* One-hop redirect follow. */
    if (out->status_code >= 300 && out->status_code < 400 && out->has_location) {
        CHAR_T next_host[WIN95_HTTP_HOST_MAX];
        CHAR_T next_path[WIN95_HTTP_PATH_MAX];
        UINT16_T next_port = 80;

        if (out->location[0] == '/') {
            /* same host */
            strncpy(next_host, host, sizeof(next_host) - 1);
            next_host[sizeof(next_host) - 1] = '\0';
            strncpy(next_path, out->location, sizeof(next_path) - 1);
            next_path[sizeof(next_path) - 1] = '\0';
            next_port = port;
        } else {
            if (win95_http10_parse_url(out->location,
                    next_host, sizeof(next_host),
                    &next_port,
                    next_path, sizeof(next_path)) != OPRT_OK) {
                return rt; /* give caller the 30x as-is */
            }
            /* If redirect is HTTPS, we can't follow. */
            if (next_port == 443) {
                PR_WARN("Redirect to HTTPS not supported: %s", out->location);
                return rt;
            }
        }

        /* Free the redirect body, refetch. */
        win95_http10_free(out);
        return __http10_get_once(next_host, next_port, next_path, timeout_ms, out);
    }

    return rt;
}

VOID_T win95_http10_free(WIN95_HTTP_RESP_T *resp)
{
    if (resp == NULL) {
        return;
    }
    /* Only free body if it was dynamically allocated (body_buf == NULL). */
    if (resp->body && resp->body_buf == NULL) {
        tal_free(resp->body);
    }
    resp->body = NULL;
    resp->body_len = 0;
    resp->has_location = FALSE;
    resp->location[0] = '\0';
    resp->content_type[0] = '\0';
    resp->set_cookie_count = 0;
    /* body_buf and body_buf_cap are NOT cleared — owned by the allocating caller. */
}
