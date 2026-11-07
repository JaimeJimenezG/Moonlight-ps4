#include "certgen.h"
#include "gs_errors.h"
#include "../log.h"

#include <mbedtls/x509_csr.h>
#include <mbedtls/rsa.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>
#if defined(_WIN32)
#  include <direct.h>
#else
#  include <sys/stat.h>
#endif

#define CERT_FILE "cert.pem"
#define KEY_FILE "key.pem"

const char *gs_error = "";

void bytes_to_hex(const unsigned char *in, char *out, size_t len) {
    for (size_t i = 0; i < len; i++)
        sprintf(out + i * 2, "%02x", in[i]);
    out[len * 2] = '\0';
}

void hex_to_bytes(const char *in, unsigned char *out, size_t *outlen) {
    size_t len = strlen(in);
    size_t o = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        unsigned int v;
        if (sscanf(&in[i], "%2x", &v) != 1)
            break;
        out[o++] = (unsigned char)v;
    }
    if (outlen)
        *outlen = o;
}

int rng_fill(client_identity_t *id, unsigned char *buf, size_t len) {
    return mbedtls_ctr_drbg_random(&id->rng, buf, len) == 0 ? GS_OK : GS_FAILED;
}

void uuid_v4(client_identity_t *id, char *out37) {
    unsigned char b[16];
    mbedtls_ctr_drbg_random(&id->rng, b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    snprintf(out37, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static int read_file(const char *path, char *buf, size_t buflen) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, buflen - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n > 0 ? 0 : -1;
}

static int write_file(const char *path, const char *buf) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t len = strlen(buf);
    size_t n = fwrite(buf, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

static int generate_identity(client_identity_t *id) {
    int ret;
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);

    ret = mbedtls_pk_setup(&id->key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret != 0)
        goto fail;

    ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(id->key), mbedtls_ctr_drbg_random,
                              &id->rng, 2048, 65537);
    if (ret != 0)
        goto fail;

    mbedtls_x509write_crt_set_subject_key(&crt, &id->key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &id->key);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    // Same name used by official Moonlight clients.
    ret = mbedtls_x509write_crt_set_subject_name(&crt, "CN=NVIDIA GameStream Client");
    if (ret != 0)
        goto fail;
    ret = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=NVIDIA GameStream Client");
    if (ret != 0)
        goto fail;

    unsigned char serial[1] = { 1 };
    ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
    if (ret != 0)
        goto fail;

    ret = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20440101000000");
    if (ret != 0)
        goto fail;

    ret = mbedtls_x509write_crt_pem(&crt, (unsigned char *)id->cert_pem,
                                    sizeof(id->cert_pem), mbedtls_ctr_drbg_random,
                                    &id->rng);
    if (ret != 0)
        goto fail;

    ret = mbedtls_pk_write_key_pem(&id->key, (unsigned char *)id->key_pem,
                                   sizeof(id->key_pem));
    if (ret != 0)
        goto fail;

    mbedtls_x509write_crt_free(&crt);
    return GS_OK;

fail:
    mbedtls_x509write_crt_free(&crt);
    gs_error = "Could not generate client certificate";
    return GS_FAILED;
}

int identity_init(client_identity_t *id, const char *keydir) {
    memset(id, 0, sizeof(*id));
    mbedtls_pk_init(&id->key);
    mbedtls_x509_crt_init(&id->cert);
    mbedtls_entropy_init(&id->entropy);
    mbedtls_ctr_drbg_init(&id->rng);

    LOGI("identity: seeding RNG...");
    const char *pers = "moonlight-ps4";
    if (mbedtls_ctr_drbg_seed(&id->rng, mbedtls_entropy_func, &id->entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        gs_error = "Could not initialize RNG";
        LOGE("identity: RNG seed FAIL");
        return GS_FAILED;
    }
    LOGI("identity: RNG OK");

#if defined(_WIN32)
    _mkdir(keydir);
#else
    mkdir(keydir, 0755);
#endif

    char cert_path[512], key_path[512];
    snprintf(cert_path, sizeof(cert_path), "%s/%s", keydir, CERT_FILE);
    snprintf(key_path, sizeof(key_path), "%s/%s", keydir, KEY_FILE);
    LOGI("identity: paths cert=%s key=%s", cert_path, key_path);

    int have = read_file(cert_path, id->cert_pem, sizeof(id->cert_pem)) == 0 &&
               read_file(key_path, id->key_pem, sizeof(id->key_pem)) == 0;
    LOGI("identity: certs on disk=%d", have);

    if (have) {
        if (mbedtls_pk_parse_key(&id->key, (unsigned char *)id->key_pem,
                                 strlen(id->key_pem) + 1, NULL, 0,
                                 mbedtls_ctr_drbg_random, &id->rng) != 0) {
            LOGW("identity: parse key FAIL; regenerating");
            have = 0;
        } else {
            LOGI("identity: key loaded OK");
        }
    }

    if (!have) {
        mbedtls_pk_free(&id->key);
        mbedtls_pk_init(&id->key);
        LOGI("identity: generating new certificate...");
        if (generate_identity(id) != GS_OK) {
            LOGE("identity: generate FAIL: %s", gs_error ? gs_error : "?");
            return GS_FAILED;
        }
        if (write_file(cert_path, id->cert_pem) != 0 ||
            write_file(key_path, id->key_pem) != 0) {
            gs_error = "Could not save generated certificate";
            LOGE("identity: write cert/key FAIL");
            return GS_FAILED;
        }
        LOGI("identity: new certificate saved");
    }

    if (mbedtls_x509_crt_parse(&id->cert, (unsigned char *)id->cert_pem,
                               strlen(id->cert_pem) + 1) != 0) {
        gs_error = "Invalid client certificate";
        LOGE("identity: parse cert FAIL");
        return GS_FAILED;
    }

    bytes_to_hex((unsigned char *)id->cert_pem, id->cert_hex, strlen(id->cert_pem));
    LOGI("identity: ready (pem %zu bytes)", strlen(id->cert_pem));
    return GS_OK;
}

void identity_free(client_identity_t *id) {
    mbedtls_pk_free(&id->key);
    mbedtls_x509_crt_free(&id->cert);
    mbedtls_ctr_drbg_free(&id->rng);
    mbedtls_entropy_free(&id->entropy);
}
