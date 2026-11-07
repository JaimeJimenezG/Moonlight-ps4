// mbedTLS port of the libgamestream logic (moonlight-embedded, GPLv3).

#define MBEDTLS_ALLOW_PRIVATE_ACCESS // access to cert.sig (certificate signature)

#include "client.h"
#include "gs_http.h"
#include "gs_errors.h"
#include "../log.h"

#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>
#include <mbedtls/pk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  define _WINSOCK_DEPRECATED_NO_WARNINGS
#  include <winsock2.h>
#else
#  include <arpa/inet.h>
#endif

#define SIGNATURE_LEN 256
#define MAX_PLAINCERT 8192

// ---------------------------------------------------------------------------
// Cryptographic utilities
// ---------------------------------------------------------------------------

static void hash_data(int use_sha256, const unsigned char *in, size_t len,
                      unsigned char *out /*32 bytes*/) {
    memset(out, 0, 32);
    if (use_sha256)
        mbedtls_sha256(in, len, out, 0);
    else
        mbedtls_sha1(in, len, out);
}

// AES-128-ECB without padding over multiples of 16 bytes.
static void aes_ecb(int encrypt, const unsigned char *key16,
                    const unsigned char *in, size_t len, unsigned char *out) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (encrypt)
        mbedtls_aes_setkey_enc(&aes, key16, 128);
    else
        mbedtls_aes_setkey_dec(&aes, key16, 128);
    for (size_t off = 0; off + 16 <= len; off += 16)
        mbedtls_aes_crypt_ecb(&aes, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                              in + off, out + off);
    mbedtls_aes_free(&aes);
}

// RSA PKCS#1 v1.5 SHA-256 signature of the message.
static int sign_message(client_identity_t *id, const unsigned char *msg, size_t mlen,
                        unsigned char *sig, size_t sig_size, size_t *slen) {
    unsigned char hash[32];
    mbedtls_sha256(msg, mlen, hash, 0);
    int ret = mbedtls_pk_sign(&id->key, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                              sig, sig_size, slen, mbedtls_ctr_drbg_random, &id->rng);
    return ret == 0 ? GS_OK : GS_FAILED;
}

// Verify a SHA256-RSA signature with the server's PEM certificate.
static bool verify_signature(const unsigned char *data, size_t datalen,
                             const unsigned char *sig, size_t siglen,
                             const char *cert_pem) {
    mbedtls_x509_crt crt;
    unsigned char hash[32];
    bool ok = false;

    mbedtls_x509_crt_init(&crt);
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert_pem,
                               strlen(cert_pem) + 1) != 0)
        goto out;

    mbedtls_sha256(data, datalen, hash, 0);
    ok = mbedtls_pk_verify(&crt.pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                           sig, siglen) == 0;
out:
    mbedtls_x509_crt_free(&crt);
    return ok;
}

// ---------------------------------------------------------------------------
// API requests
// ---------------------------------------------------------------------------

// GET to the API; path is built with printf format.
#if defined(_MSC_VER)
#  define ML_API_GET_ATTR
#elif defined(__clang__) || defined(__GNUC__)
#  define ML_API_GET_ATTR __attribute__((format(printf, 4, 5)))
#else
#  define ML_API_GET_ATTR
#endif
ML_API_GET_ATTR
static int api_get(gs_server_t *server, int https, http_resp_t *resp,
                   const char *fmt, ...) {
    char path[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(path, sizeof(path), fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof(path)) {
        gs_error = "Request too long";
        return GS_INVALID;
    }
    return http_get(server->address, https ? server->httpsPort : server->httpPort,
                    https, path, resp);
}

// Check status_code and extract a field from the XML.
static int xml_get_field(http_resp_t *resp, const char *field, char **out) {
    *out = NULL;
    if (xml_status(resp->body, resp->len) != 200) {
        gs_error = "Server returned an error status";
        return GS_ERROR;
    }
    return xml_search(resp->body, resp->len, field, out);
}

static int load_serverinfo(gs_server_t *server, bool https) {
    char uuid[37];
    http_resp_t resp;
    int ret = GS_INVALID;
    char *paired = NULL, *currentGame = NULL, *state = NULL, *httpsPort = NULL;
    char *appversion = NULL, *gfeversion = NULL, *codecMode = NULL;

    uuid_v4(server->id, uuid);
    ret = api_get(server, https, &resp,
                  "/serverinfo?uniqueid=%s&uuid=%s", GS_UNIQUEID, uuid);
    if (ret != GS_OK)
        return ret;

    ret = GS_INVALID;
    if (xml_status(resp.body, resp.len) != 200) {
        gs_error = "serverinfo returned an error status";
        goto out;
    }

    if (xml_search(resp.body, resp.len, "currentgame", &currentGame) != GS_OK ||
        xml_search(resp.body, resp.len, "PairStatus", &paired) != GS_OK ||
        xml_search(resp.body, resp.len, "appversion", &appversion) != GS_OK ||
        xml_search(resp.body, resp.len, "state", &state) != GS_OK)
        goto out;

    if (!strlen(currentGame) || !strlen(paired) || !strlen(appversion) || !strlen(state))
        goto out;

    xml_search(resp.body, resp.len, "HttpsPort", &httpsPort);
    xml_search(resp.body, resp.len, "GfeVersion", &gfeversion);
    xml_search(resp.body, resp.len, "ServerCodecModeSupport", &codecMode);

    server->paired = !strcmp(paired, "1");
    server->currentGame = atoi(currentGame);
    server->serverMajorVersion = atoi(appversion);
    server->serverCodecModeSupport = (codecMode && codecMode[0]) ? atoi(codecMode) : SCM_H264;
    server->isNvidiaSoftware = strstr(state, "MJOLNIR") != NULL;
    snprintf(server->appVersion, sizeof(server->appVersion), "%s", appversion);
    if (gfeversion)
        snprintf(server->gfeVersion, sizeof(server->gfeVersion), "%s", gfeversion);

    server->httpsPort = httpsPort ? (unsigned short)atoi(httpsPort) : 0;
    if (!server->httpsPort)
        server->httpsPort = 47984;

    if (strstr(state, "_SERVER_BUSY") == NULL) {
        // After GFE 2.8 currentgame stays set even with no active session.
        server->currentGame = 0;
    }
    ret = GS_OK;

out:
    free(paired);
    free(currentGame);
    free(state);
    free(httpsPort);
    free(appversion);
    free(gfeversion);
    free(codecMode);
    http_resp_free(&resp);
    return ret;
}

int gs_init(gs_server_t *server, client_identity_t *id, const char *address,
            unsigned short httpPort, const char *keydir) {
    (void)keydir;
    memset(server, 0, sizeof(*server));
    server->id = id;
    snprintf(server->address, sizeof(server->address), "%s", address);
    server->httpPort = httpPort ? httpPort : 47989;
    server->httpsPort = 0;

    LOGI("gs_init: HTTP serverinfo %s:%u ...", server->address, server->httpPort);
    int ret = load_serverinfo(server, false);
    if (ret != GS_OK) {
        LOGE("gs_init: HTTP serverinfo FAIL ret=%d err=%s", ret,
             gs_error ? gs_error : "?");
        return ret;
    }
    LOGI("gs_init: HTTP OK paired=%d httpsPort=%u ver=%s",
         server->paired, server->httpsPort, server->appVersion);

    LOGI("gs_init: HTTPS serverinfo port %u ...", server->httpsPort);
    if (load_serverinfo(server, true) != GS_OK) {
        LOGW("gs_init: HTTPS serverinfo failed (normal if not paired); paired=0");
        server->paired = false;
    } else {
        LOGI("gs_init: HTTPS OK paired=%d", server->paired);
    }

    LiInitializeServerInformation(&server->serverInfo);
    server->serverInfo.address = server->address;
    server->serverInfo.serverInfoAppVersion = server->appVersion;
    server->serverInfo.serverInfoGfeVersion = server->gfeVersion;
    server->serverInfo.serverCodecModeSupport = server->serverCodecModeSupport;
    return GS_OK;
}

// ---------------------------------------------------------------------------
// Four-phase pairing
// ---------------------------------------------------------------------------

// Check paired=1 in the response; frees resp.
static int check_paired(http_resp_t *resp) {
    char *result = NULL;
    int ret = xml_get_field(resp, "paired", &result);
    if (ret == GS_OK && (!result || strcmp(result, "1") != 0)) {
        gs_error = "Server rejected pairing";
        ret = GS_FAILED;
    }
    free(result);
    http_resp_free(resp);
    return ret;
}

int gs_pair(gs_server_t *server, const char *pin) {
    int ret;
    char uuid[37];
    http_resp_t resp = {0};
    char *result = NULL;
    client_identity_t *id = server->id;

    if (server->paired) {
        gs_error = "Already paired";
        return GS_WRONG_STATE;
    }
    if (server->currentGame != 0) {
        gs_error = "A game is running on the server; close it before pairing";
        return GS_WRONG_STATE;
    }

    LOGI("pairing: phase 1, getservercert (PIN=%s)", pin);
    LOGI("pair f1...");

    // --- Phase 1: salt + client certificate -> server certificate
    unsigned char salt[16];
    char salt_hex[33];
    rng_fill(id, salt, sizeof(salt));
    bytes_to_hex(salt, salt_hex, sizeof(salt));

    uuid_v4(id, uuid);
    ret = api_get(server, 0, &resp,
                  "/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1"
                  "&phrase=getservercert&salt=%s&clientcert=%s",
                  GS_UNIQUEID, uuid, salt_hex, id->cert_hex);
    if (ret != GS_OK) {
        LOGE("pairing: phase 1 http FAIL ret=%d %s", ret, gs_error ? gs_error : "");
        return ret;
    }

    if ((ret = xml_get_field(&resp, "paired", &result)) != GS_OK)
        goto fail_resp;
    if (strcmp(result, "1") != 0) {
        gs_error = "Pairing rejected (phase 1)";
        ret = GS_FAILED;
        goto fail_resp;
    }
    free(result);
    result = NULL;

    if ((ret = xml_search(resp.body, resp.len, "plaincert", &result)) != GS_OK)
        goto fail_resp;
    if (!result[0]) {
        // Sunshine returns empty plaincert if another pairing is in progress.
        gs_error = "Server did not return its certificate (another pairing in progress?)";
        ret = GS_FAILED;
        goto fail_resp;
    }

    size_t plaincert_len = 0;
    if (strlen(result) / 2 >= sizeof(server->serverCertPem)) {
        gs_error = "Server certificate too large";
        ret = GS_FAILED;
        goto fail_resp;
    }
    hex_to_bytes(result, (unsigned char *)server->serverCertPem, &plaincert_len);
    server->serverCertPem[plaincert_len] = '\0';
    free(result);
    result = NULL;
    http_resp_free(&resp);

    // AES key derived from salt+PIN.
    int use_sha256 = server->serverMajorVersion >= 7;
    int hash_len = use_sha256 ? 32 : 20;
    unsigned char salt_pin[16 + 4];
    unsigned char aes_key[32];
    memcpy(salt_pin, salt, 16);
    memcpy(salt_pin + 16, pin, 4);
    hash_data(use_sha256, salt_pin, sizeof(salt_pin), aes_key);

    LOGI("pairing: phase 2, clientchallenge");
    LOGI("pair f2...");

    // --- Phase 2: encrypted client challenge
    unsigned char challenge[16];
    unsigned char challenge_enc[16];
    char challenge_hex[33];
    rng_fill(id, challenge, sizeof(challenge));
    aes_ecb(1, aes_key, challenge, sizeof(challenge), challenge_enc);
    bytes_to_hex(challenge_enc, challenge_hex, sizeof(challenge_enc));

    uuid_v4(id, uuid);
    ret = api_get(server, 0, &resp,
                  "/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1"
                  "&clientchallenge=%s",
                  GS_UNIQUEID, uuid, challenge_hex);
    if (ret != GS_OK)
        goto fail;

    if ((ret = xml_get_field(&resp, "paired", &result)) != GS_OK)
        goto fail_resp;
    if (strcmp(result, "1") != 0) {
        gs_error = "Pairing rejected (phase 2)";
        ret = GS_FAILED;
        goto fail_resp;
    }
    free(result);
    result = NULL;

    if ((ret = xml_search(resp.body, resp.len, "challengeresponse", &result)) != GS_OK)
        goto fail_resp;

    unsigned char challresp_enc[64];
    unsigned char challresp[64];
    size_t challresp_len = 0;
    if (strlen(result) / 2 > sizeof(challresp_enc)) {
        gs_error = "challengeresponse too large";
        ret = GS_FAILED;
        goto fail_resp;
    }
    hex_to_bytes(result, challresp_enc, &challresp_len);
    aes_ecb(0, aes_key, challresp_enc, challresp_len, challresp);
    free(result);
    result = NULL;
    http_resp_free(&resp);

    LOGI("pairing: phase 3, serverchallengeresp");
    LOGI("pair f3...");

    // --- Phase 3: response to server challenge
    // challresp = hash(servchal..) + serverChallenge(16), starting at hash_len.
    unsigned char client_secret[16];
    rng_fill(id, client_secret, sizeof(client_secret));

    const unsigned char *cert_sig = id->cert.sig.p;
    size_t cert_sig_len = id->cert.sig.len;
    if (cert_sig_len != SIGNATURE_LEN)
        LOGW("pairing: certificate signature is %zu bytes (expected %d)",
             cert_sig_len, SIGNATURE_LEN);

    unsigned char chall_response[16 + SIGNATURE_LEN + 16];
    size_t cr_len = 0;
    memcpy(chall_response, challresp + hash_len, 16);
    cr_len += 16;
    memcpy(chall_response + cr_len, cert_sig, cert_sig_len);
    cr_len += cert_sig_len;
    memcpy(chall_response + cr_len, client_secret, sizeof(client_secret));
    cr_len += sizeof(client_secret);

    unsigned char cr_hash[32];
    unsigned char cr_hash_enc[32];
    char cr_hex[65];
    hash_data(use_sha256, chall_response, cr_len, cr_hash);
    aes_ecb(1, aes_key, cr_hash, 32, cr_hash_enc);
    bytes_to_hex(cr_hash_enc, cr_hex, 32);

    uuid_v4(id, uuid);
    ret = api_get(server, 0, &resp,
                  "/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1"
                  "&serverchallengeresp=%s",
                  GS_UNIQUEID, uuid, cr_hex);
    if (ret != GS_OK)
        goto fail;

    if ((ret = xml_get_field(&resp, "paired", &result)) != GS_OK)
        goto fail_resp;
    if (strcmp(result, "1") != 0) {
        gs_error = "Pairing rejected (phase 3): wrong PIN?";
        ret = GS_FAILED;
        goto fail_resp;
    }
    free(result);
    result = NULL;

    if ((ret = xml_search(resp.body, resp.len, "pairingsecret", &result)) != GS_OK)
        goto fail_resp;

    unsigned char pairing_secret[16 + SIGNATURE_LEN];
    size_t ps_len = 0;
    if (strlen(result) / 2 > sizeof(pairing_secret)) {
        gs_error = "pairingsecret too large";
        ret = GS_FAILED;
        goto fail_resp;
    }
    hex_to_bytes(result, pairing_secret, &ps_len);
    free(result);
    result = NULL;
    http_resp_free(&resp);

    // Anti-MITM check: secret signature must be from the certificate
    // the server presented in phase 1.
    if (!verify_signature(pairing_secret, 16, pairing_secret + 16,
                          ps_len - 16, server->serverCertPem)) {
        gs_error = "Invalid server signature (possible MITM)";
        ret = GS_FAILED;
        goto fail;
    }

    LOGI("pairing: phase 4, clientpairingsecret");
    LOGI("pair f4...");

    // --- Phase 4: signed client secret
    unsigned char signature[SIGNATURE_LEN];
    size_t sig_len = 0;
    if (sign_message(id, client_secret, sizeof(client_secret),
                     signature, sizeof(signature), &sig_len) != GS_OK) {
        gs_error = "Could not sign client secret";
        ret = GS_FAILED;
        goto fail;
    }

    unsigned char client_pairing_secret[16 + SIGNATURE_LEN];
    char cps_hex[(16 + SIGNATURE_LEN) * 2 + 1];
    memcpy(client_pairing_secret, client_secret, 16);
    memcpy(client_pairing_secret + 16, signature, sig_len);
    bytes_to_hex(client_pairing_secret, cps_hex, 16 + sig_len);

    uuid_v4(id, uuid);
    ret = api_get(server, 0, &resp,
                  "/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1"
                  "&clientpairingsecret=%s",
                  GS_UNIQUEID, uuid, cps_hex);
    if (ret != GS_OK)
        goto fail;
    if ((ret = check_paired(&resp)) != GS_OK)
        goto fail;

    LOGI("pairing: HTTPS confirmation, pairchallenge");
    LOGI("pair f5 https...");

    // --- HTTPS confirmation with client certificate
    uuid_v4(id, uuid);
    ret = api_get(server, 1, &resp,
                  "/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1"
                  "&phrase=pairchallenge",
                  GS_UNIQUEID, uuid);
    if (ret != GS_OK)
        goto fail;
    if ((ret = check_paired(&resp)) != GS_OK)
        goto fail;

    server->paired = true;
    LOGI("pairing: completed");
    LOGI("pair DONE");
    return GS_OK;

fail_resp:
    free(result);
    http_resp_free(&resp);
fail:
    gs_unpair(server);
    return ret;
}

int gs_unpair(gs_server_t *server) {
    char uuid[37];
    http_resp_t resp;

    uuid_v4(server->id, uuid);
    int ret = api_get(server, 0, &resp, "/unpair?uniqueid=%s&uuid=%s",
                      GS_UNIQUEID, uuid);
    if (ret == GS_OK) {
        http_resp_free(&resp);
        server->paired = false;
    }
    return ret;
}

// ---------------------------------------------------------------------------
// Applist and launch
// ---------------------------------------------------------------------------

int gs_applist(gs_server_t *server, app_entry_t **list) {
    char uuid[37];
    http_resp_t resp;

    uuid_v4(server->id, uuid);
    int ret = api_get(server, 1, &resp, "/applist?uniqueid=%s&uuid=%s",
                      GS_UNIQUEID, uuid);
    if (ret != GS_OK)
        return ret;

    if (xml_status(resp.body, resp.len) != 200) {
        gs_error = "applist returned an error status";
        ret = GS_ERROR;
    } else {
        ret = xml_applist(resp.body, resp.len, list);
    }
    http_resp_free(&resp);
    return ret;
}

int gs_start_app(gs_server_t *server, STREAM_CONFIGURATION *config, int appId,
                 bool sops, bool localaudio, int gamepad_mask) {
    char uuid[37];
    http_resp_t resp;
    char *result = NULL;
    int ret;

    rng_fill(server->id, (unsigned char *)config->remoteInputAesKey,
             sizeof(config->remoteInputAesKey));
    memset(config->remoteInputAesIv, 0, sizeof(config->remoteInputAesIv));

    uint32_t rikeyid = 0;
    rng_fill(server->id, (unsigned char *)config->remoteInputAesIv, 4);
    memcpy(&rikeyid, config->remoteInputAesIv, 4);
    rikeyid = htonl(rikeyid);

    char rikey_hex[sizeof(config->remoteInputAesKey) * 2 + 1];
    bytes_to_hex((unsigned char *)config->remoteInputAesKey, rikey_hex,
                 sizeof(config->remoteInputAesKey));

    // FPS > 60 makes GFE SOPS force 720p60; Sunshine ignores it.
    int fps = (server->isNvidiaSoftware && config->fps > 60) ? 0 : config->fps;
    int surround_info = SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(config->audioConfiguration);

    uuid_v4(server->id, uuid);
    ret = api_get(server, 1, &resp,
                  "/%s?uniqueid=%s&uuid=%s&appid=%d&mode=%dx%dx%d"
                  "&additionalStates=1&sops=%d&rikey=%s&rikeyid=%u"
                  "&localAudioPlayMode=%d&surroundAudioInfo=%d"
                  "&remoteControllersBitmap=%d&gcmap=%d%s",
                  server->currentGame ? "resume" : "launch",
                  GS_UNIQUEID, uuid, appId,
                  config->width, config->height, fps,
                  sops ? 1 : 0, rikey_hex, (unsigned)rikeyid,
                  localaudio ? 1 : 0, surround_info,
                  gamepad_mask, gamepad_mask,
                  LiGetLaunchUrlQueryParameters());
    if (ret != GS_OK)
        return ret;

    if (xml_status(resp.body, resp.len) != 200) {
        char *msg = NULL;
        xml_search(resp.body, resp.len, "root", &msg);
        gs_error = "launch returned an error status";
        free(msg);
        ret = GS_ERROR;
        goto out;
    }

    if (xml_search(resp.body, resp.len, "gamesession", &result) != GS_OK &&
        xml_search(resp.body, resp.len, "resume", &result) != GS_OK) {
        ret = GS_INVALID;
        goto out;
    }
    if (!strcmp(result, "0")) {
        gs_error = "Server could not start the session";
        ret = GS_FAILED;
        goto out;
    }
    free(result);
    result = NULL;

    if (xml_search(resp.body, resp.len, "sessionUrl0", &result) == GS_OK) {
        snprintf(server->rtspSessionUrl, sizeof(server->rtspSessionUrl), "%s", result);
        server->serverInfo.rtspSessionUrl = server->rtspSessionUrl;
    }

    server->currentGame = appId;
    ret = GS_OK;

out:
    free(result);
    http_resp_free(&resp);
    return ret;
}

int gs_quit_app(gs_server_t *server) {
    char uuid[37];
    http_resp_t resp;
    char *result = NULL;

    uuid_v4(server->id, uuid);
    int ret = api_get(server, 1, &resp, "/cancel?uniqueid=%s&uuid=%s",
                      GS_UNIQUEID, uuid);
    if (ret != GS_OK)
        return ret;

    if ((ret = xml_get_field(&resp, "cancel", &result)) != GS_OK)
        goto out;
    if (!strcmp(result, "0")) {
        gs_error = "Server could not cancel the session";
        ret = GS_FAILED;
        goto out;
    }
    server->currentGame = 0;
    ret = GS_OK;

out:
    free(result);
    http_resp_free(&resp);
    return ret;
}

int gs_refresh_status(gs_server_t *server) {
    if (!server)
        return GS_INVALID;
    bool https = server->paired && server->httpsPort != 0;
    int ret = load_serverinfo(server, https);
    if (ret != GS_OK && https)
        ret = load_serverinfo(server, false);
    return ret;
}
