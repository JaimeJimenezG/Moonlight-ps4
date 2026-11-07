#include "gs_http.h"
#include "gs_errors.h"
#include "../log.h"

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/error.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define ml_strncasecmp _strnicmp
#else
#  define ml_strncasecmp strncasecmp
#  include <strings.h>
#endif

#define HTTP_TIMEOUT_DEFAULT_MS 7000
#define MAX_RESPONSE (4 * 1024 * 1024)

static client_identity_t *s_id;
static int s_verbose;
static unsigned s_timeout_ms = HTTP_TIMEOUT_DEFAULT_MS;

void http_init(client_identity_t *id, int verbose) {
    s_id = id;
    s_verbose = verbose;
    s_timeout_ms = HTTP_TIMEOUT_DEFAULT_MS;
}

void http_set_timeout_ms(unsigned ms) {
    s_timeout_ms = ms ? ms : HTTP_TIMEOUT_DEFAULT_MS;
}

void http_resp_free(http_resp_t *resp) {
    free(resp->body);
    resp->body = NULL;
    resp->len = 0;
}

typedef struct {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    int use_tls;
} conn_t;

static int conn_read(conn_t *c, unsigned char *buf, size_t len) {
    int ret;
    do {
        if (c->use_tls)
            ret = mbedtls_ssl_read(&c->ssl, buf, len);
        else
            ret = mbedtls_net_recv_timeout(&c->net, buf, len, s_timeout_ms);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        return 0;
    return ret;
}

static int conn_write_all(conn_t *c, const unsigned char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        int ret;
        if (c->use_tls)
            ret = mbedtls_ssl_write(&c->ssl, buf + off, len - off);
        else
            ret = mbedtls_net_send(&c->net, buf + off, len - off);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret <= 0)
            return -1;
        off += (size_t)ret;
    }
    return 0;
}

static void conn_close(conn_t *c) {
    if (c->use_tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
    }
    mbedtls_net_free(&c->net);
}

static int conn_open(conn_t *c, const char *host, unsigned short port, int use_tls) {
    char port_str[8];
    int ret;

    memset(c, 0, sizeof(*c));
    c->use_tls = use_tls;
    mbedtls_net_init(&c->net);

    snprintf(port_str, sizeof(port_str), "%u", port);
    LOGI("http: connect %s:%s tls=%d timeout=%ums", host, port_str, use_tls, s_timeout_ms);
    ret = mbedtls_net_connect(&c->net, host, port_str, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        char err[128];
        mbedtls_strerror(ret, err, sizeof(err));
        LOGE("http: connect FAIL %s:%s -0x%04x %s", host, port_str, (unsigned)-ret, err);
        gs_error = "Could not connect to host";
        return GS_IO_ERROR;
    }
    LOGI("http: TCP connected");

    if (!use_tls)
        return GS_OK;

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);

    ret = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
        goto tls_fail;

    // Server certificate is self-signed; real authenticity comes from
    // pairing (signature verification), not PKI.
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &s_id->rng);
    mbedtls_ssl_conf_read_timeout(&c->conf, s_timeout_ms);

    ret = mbedtls_ssl_conf_own_cert(&c->conf, &s_id->cert, &s_id->key);
    if (ret != 0)
        goto tls_fail;

    ret = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (ret != 0)
        goto tls_fail;

    mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, NULL,
                        mbedtls_net_recv_timeout);

    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            goto tls_fail;
    }
    return GS_OK;

tls_fail:
    if (s_verbose) {
        char err[128];
        mbedtls_strerror(ret, err, sizeof(err));
        LOGE("http: TLS failure: -0x%04x %s", (unsigned)-ret, err);
    }
    conn_close(c);
    gs_error = "TLS handshake failed";
    return GS_IO_ERROR;
}

// Read until exactly `len` bytes are complete.
static int read_exact(conn_t *c, unsigned char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = conn_read(c, buf + off, len - off);
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

// Read a line ending in \n (included) into buf.
static int read_line(conn_t *c, char *buf, size_t buflen) {
    size_t o = 0;
    while (o + 1 < buflen) {
        unsigned char ch;
        int n = conn_read(c, &ch, 1);
        if (n <= 0)
            return -1;
        buf[o++] = (char)ch;
        if (ch == '\n')
            break;
    }
    buf[o] = '\0';
    return (int)o;
}

int http_get(const char *host, unsigned short port, int use_tls,
             const char *path, http_resp_t *resp) {
    conn_t c;
    char line[1024];
    int ret = GS_FAILED;
    long content_length = -1;
    int chunked = 0;

    resp->body = NULL;
    resp->len = 0;

    if (s_verbose)
        LOGI("http: %s://%s:%u%.120s", use_tls ? "https" : "http", host, port, path);
    else
        LOGI("http: %s://%s:%u (path len %zu)", use_tls ? "https" : "http", host, port, strlen(path));

    ret = conn_open(&c, host, port, use_tls);
    if (ret != GS_OK) {
        LOGE("http: conn_open FAIL ret=%d err=%s", ret, gs_error ? gs_error : "");
        return ret;
    }

    char req[6144];
    int reqlen = snprintf(req, sizeof(req),
                          "GET %s HTTP/1.1\r\n"
                          "Host: %s:%u\r\n"
                          "User-Agent: moonlight-ps4/0.1\r\n"
                          "Accept: */*\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          path, host, port);
    if (reqlen <= 0 || reqlen >= (int)sizeof(req)) {
        gs_error = "URL too long";
        ret = GS_INVALID;
        goto out;
    }

    if (conn_write_all(&c, (unsigned char *)req, (size_t)reqlen) != 0) {
        gs_error = "Failed to send request";
        ret = GS_IO_ERROR;
        goto out;
    }

    // Status line
    if (read_line(&c, line, sizeof(line)) <= 0) {
        gs_error = "No response from server";
        ret = GS_IO_ERROR;
        goto out;
    }
    int http_status = 0;
    if (sscanf(line, "HTTP/%*d.%*d %d", &http_status) != 1) {
        gs_error = "Invalid HTTP response";
        ret = GS_IO_ERROR;
        goto out;
    }

    // Headers
    for (;;) {
        if (read_line(&c, line, sizeof(line)) <= 0) {
            gs_error = "Truncated HTTP headers";
            ret = GS_IO_ERROR;
            goto out;
        }
        if (!strcmp(line, "\r\n") || !strcmp(line, "\n"))
            break;
        if (!ml_strncasecmp(line, "Content-Length:", 15))
            content_length = strtol(line + 15, NULL, 10);
        else if (!ml_strncasecmp(line, "Transfer-Encoding:", 18) && strstr(line, "chunked"))
            chunked = 1;
    }

    // Body
    if (chunked) {
        size_t cap = 8192;
        resp->body = malloc(cap);
        if (!resp->body) {
            ret = GS_OUT_OF_MEMORY;
            goto out;
        }
        for (;;) {
            if (read_line(&c, line, sizeof(line)) <= 0) {
                ret = GS_IO_ERROR;
                goto out;
            }
            long chunk = strtol(line, NULL, 16);
            if (chunk < 0 || (size_t)chunk > MAX_RESPONSE) {
                ret = GS_IO_ERROR;
                goto out;
            }
            if (chunk == 0) {
                read_line(&c, line, sizeof(line)); // final CRLF
                break;
            }
            if (resp->len + (size_t)chunk + 1 > cap) {
                while (cap < resp->len + (size_t)chunk + 1)
                    cap *= 2;
                char *nb = realloc(resp->body, cap);
                if (!nb) {
                    ret = GS_OUT_OF_MEMORY;
                    goto out;
                }
                resp->body = nb;
            }
            if (read_exact(&c, (unsigned char *)resp->body + resp->len, (size_t)chunk) != 0) {
                ret = GS_IO_ERROR;
                goto out;
            }
            resp->len += (size_t)chunk;
            read_line(&c, line, sizeof(line)); // CRLF after chunk
        }
    } else if (content_length >= 0) {
        if (content_length > MAX_RESPONSE) {
            gs_error = "Response too large";
            ret = GS_IO_ERROR;
            goto out;
        }
        resp->body = malloc((size_t)content_length + 1);
        if (!resp->body) {
            ret = GS_OUT_OF_MEMORY;
            goto out;
        }
        if (read_exact(&c, (unsigned char *)resp->body, (size_t)content_length) != 0) {
            gs_error = "Truncated HTTP body";
            ret = GS_IO_ERROR;
            goto out;
        }
        resp->len = (size_t)content_length;
    } else {
        // No length: read until close.
        size_t cap = 8192;
        resp->body = malloc(cap);
        if (!resp->body) {
            ret = GS_OUT_OF_MEMORY;
            goto out;
        }
        for (;;) {
            if (resp->len + 2048 + 1 > cap) {
                cap *= 2;
                if (cap > MAX_RESPONSE) {
                    ret = GS_IO_ERROR;
                    goto out;
                }
                char *nb = realloc(resp->body, cap);
                if (!nb) {
                    ret = GS_OUT_OF_MEMORY;
                    goto out;
                }
                resp->body = nb;
            }
            int n = conn_read(&c, (unsigned char *)resp->body + resp->len, 2048);
            if (n < 0 && resp->len == 0) {
                ret = GS_IO_ERROR;
                goto out;
            }
            if (n <= 0)
                break;
            resp->len += (size_t)n;
        }
    }

    if (resp->body)
        resp->body[resp->len] = '\0';

    if (http_status != 200) {
        gs_error = "Server returned an HTTP error";
        ret = GS_FAILED;
        goto out;
    }

    if (s_verbose && resp->body)
        LOGI("http: response %d, %zu bytes", http_status, resp->len);

    ret = GS_OK;

out:
    if (ret != GS_OK)
        http_resp_free(resp);
    conn_close(&c);
    return ret;
}
