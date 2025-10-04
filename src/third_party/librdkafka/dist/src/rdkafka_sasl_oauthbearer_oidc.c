/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2021-2022, Magnus Edenhill
 *               2023, Confluent Inc.

 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


/**
 * Builtin SASL OAUTHBEARER OIDC support
 */
#include "rdkafka_int.h"
#include "rdkafka_sasl_int.h"
#include "rdunittest.h"
#include "cJSON.h"
#include <curl/curl.h>
#include "rdhttp.h"
#include "rdkafka_sasl_oauthbearer_oidc.h"
#include "rdbase64.h"


/**
 * @brief Generate Authorization field for HTTP header.
 *        The field contains base64-encoded string which
 *        is generated from \p client_id and \p client_secret.
 *
 * @returns Return the authorization field.
 *
 * @locality Any thread.
 */
static char *rd_kafka_oidc_build_auth_header(const char *client_id,
                                             const char *client_secret) {

        rd_chariov_t client_authorization_in;
        rd_chariov_t client_authorization_out;

        size_t authorization_base64_header_size;
        char *authorization_base64_header;

        client_authorization_in.size =
            strlen(client_id) + strlen(client_secret) + 2;
        client_authorization_in.ptr = rd_malloc(client_authorization_in.size);
        rd_snprintf(client_authorization_in.ptr, client_authorization_in.size,
                    "%s:%s", client_id, client_secret);

        client_authorization_in.size--;
        rd_base64_encode(&client_authorization_in, &client_authorization_out);
        rd_assert(client_authorization_out.ptr);

        authorization_base64_header_size =
            strlen("Authorization: Basic ") + client_authorization_out.size + 1;
        authorization_base64_header =
            rd_malloc(authorization_base64_header_size);
        rd_snprintf(authorization_base64_header,
                    authorization_base64_header_size, "Authorization: Basic %s",
                    client_authorization_out.ptr);

        rd_free(client_authorization_in.ptr);
        rd_free(client_authorization_out.ptr);
        return authorization_base64_header;
}


/**
 * @brief Build headers for HTTP(S) requests based on \p client_id
 *        and \p client_secret. The result will be returned in \p *headersp.
 *
 * @locality Any thread.
 */
static void rd_kafka_oidc_build_headers(const char *client_id,
                                        const char *client_secret,
                                        struct curl_slist **headersp) {
        char *authorization_base64_header;

        authorization_base64_header =
            rd_kafka_oidc_build_auth_header(client_id, client_secret);

        *headersp = curl_slist_append(*headersp, "Accept: application/json");
        *headersp = curl_slist_append(*headersp, authorization_base64_header);

        *headersp = curl_slist_append(
            *headersp, "Content-Type: application/x-www-form-urlencoded");

        rd_free(authorization_base64_header);
}

/**
 * @brief The format of JWT is Header.Payload.Signature.
 *        Extract and decode payloads from JWT \p src.
 *        The decoded payloads will be returned in \p *bufplainp.
 *
 * @returns Return error message while decoding the payload.
 */
static const char *rd_kafka_jwt_b64_decode_payload(const char *src,
                                                   char **bufplainp) {
        char *converted_src;
        char *payload = NULL;

        const char *errstr = NULL;

        int i, padding, len;

        int payload_len;
        int nbytesdecoded;

        int payloads_start = 0;
        int payloads_end   = 0;

        len           = (int)strlen(src);
        converted_src = rd_malloc(len + 4);

        for (i = 0; i < len; i++) {
                switch (src[i]) {
                case '-':
                        converted_src[i] = '+';
                        break;

                case '_':
                        converted_src[i] = '/';
                        break;

                case '.':
                        if (payloads_start == 0)
                                payloads_start = i + 1;
                        else {
                                if (payloads_end > 0) {
                                        errstr =
                                            "The token is invalid with more "
                                            "than 2 delimiters";
                                        goto done;
                                }
                                payloads_end = i;
                        }
                        /* FALLTHRU */

                default:
                        converted_src[i] = src[i];
                }
        }

        if (payloads_start == 0 || payloads_end == 0) {
                errstr = "The token is invalid with less than 2 delimiters";
                goto done;
        }

        payload_len = payloads_end - payloads_start;
        payload     = rd_malloc(payload_len + 4);
        strncpy(payload, (converted_src + payloads_start), payload_len);

        padding = 4 - (payload_len % 4);
        if (padding < 4) {
                while (padding--)
                        payload[payload_len++] = '=';
        }

        nbytesdecoded = ((payload_len + 3) / 4) * 3;
        *bufplainp    = rd_malloc(nbytesdecoded + 1);

        if (EVP_DecodeBlock((uint8_t *)(*bufplainp), (uint8_t *)payload,
                            (int)payload_len) == -1) {
                errstr = "Failed to decode base64 payload";
        }

done:
        RD_IF_FREE(payload, rd_free);
        RD_IF_FREE(converted_src, rd_free);
        return errstr;
}

/**
 * @brief Build post_fields with \p scope.
 *        The format of the post_fields is
 *        `grant_type=client_credentials&scope=scope`
 *        The post_fields will be returned in \p *post_fields.
 *        The post_fields_size will be returned in \p post_fields_size.
 *
 */
static void rd_kafka_oidc_build_post_fields(const char *scope,
                                            char **post_fields,
                                            size_t *post_fields_size) {
        size_t scope_size = 0;

        if (scope)
                scope_size = strlen(scope);
        if (scope_size == 0) {
                *post_fields      = rd_strdup("grant_type=client_credentials");
                *post_fields_size = strlen("grant_type=client_credentials");
        } else {
                *post_fields_size =
                    strlen("grant_type=client_credentials&scope=") + scope_size;
                *post_fields = rd_malloc(*post_fields_size + 1);
                rd_snprintf(*post_fields, *post_fields_size + 1,
                            "grant_type=client_credentials&scope=%s", scope);
        }
}


/**
 * @brief Implementation of Oauth/OIDC token refresh callback function,
 *        will receive the JSON response after HTTP call to token provider,
 *        then extract the jwt from the JSON response, and forward it to
 *        the broker.
 */
void rd_kafka_oidc_token_refresh_cb(rd_kafka_t *rk,
                                    const char *oauthbearer_config,
                                    void *opaque) {
        const int timeout_s = 20;
        const int retry     = 4;
        const int retry_ms  = 5 * 1000;

        double exp;

        cJSON *json     = NULL;
        cJSON *payloads = NULL;
        cJSON *parsed_token, *jwt_exp, *jwt_sub;

        rd_http_error_t *herr;

        char *jwt_token;
        char *post_fields;
        char *decoded_payloads = NULL;

        struct curl_slist *headers = NULL;

        const char *token_url;
        const char *sub;
        const char *errstr;

        size_t post_fields_size;
        size_t extension_cnt;
        size_t extension_key_value_cnt = 0;

        char set_token_errstr[512];
        char decode_payload_errstr[512];

        char **extensions          = NULL;
        char **extension_key_value = NULL;

        if (rd_kafka_terminating(rk))
                return;

        rd_kafka_oidc_build_headers(rk->rk_conf.sasl.oauthbearer.client_id,
                                    rk->rk_conf.sasl.oauthbearer.client_secret,
                                    &headers);

        /* Build post fields */
        rd_kafka_oidc_build_post_fields(rk->rk_conf.sasl.oauthbearer.scope,
                                        &post_fields, &post_fields_size);

        token_url = rk->rk_conf.sasl.oauthbearer.token_endpoint_url;

        herr = rd_http_post_expect_json(rk, token_url, headers, post_fields,
                                        post_fields_size, timeout_s, retry,
                                        retry_ms, &json);

        if (unlikely(herr != NULL)) {
                rd_kafka_log(rk, LOG_ERR, "OIDC",
                             "Failed to retrieve OIDC "
                             "token from \"%s\": %s (%d)",
                             token_url, herr->errstr, herr->code);
                rd_kafka_oauthbearer_set_token_failure(rk, herr->errstr);
                rd_http_error_destroy(herr);
                goto done;
        }

        parsed_token = cJSON_GetObjectItem(json, "access_token");

        if (parsed_token == NULL) {
                rd_kafka_oauthbearer_set_token_failure(
                    rk,
                    "Expected JSON JWT response with "
                    "\"access_token\" field");
                goto done;
        }

        jwt_token = cJSON_GetStringValue(parsed_token);
        if (jwt_token == NULL) {
                rd_kafka_oauthbearer_set_token_failure(
                    rk,
                    "Expected JSON "
                    "response as a value string");
                goto done;
        }

        errstr = rd_kafka_jwt_b64_decode_payload(jwt_token, &decoded_payloads);
        if (errstr != NULL) {
                rd_snprintf(decode_payload_errstr,
                            sizeof(decode_payload_errstr),
                            "Failed to decode JWT payload: %s", errstr);
                rd_kafka_oauthbearer_set_token_failure(rk,
                                                       decode_payload_errstr);
                goto done;
        }

        payloads = cJSON_Parse(decoded_payloads);
        if (payloads == NULL) {
                rd_kafka_oauthbearer_set_token_failure(
                    rk, "Failed to parse JSON JWT payload");
                goto done;
        }

        jwt_exp = cJSON_GetObjectItem(payloads, "exp");
        if (jwt_exp == NULL) {
                rd_kafka_oauthbearer_set_token_failure(
                    rk,
                    "Expected JSON JWT response with "
                    "\"exp\" field");
                goto done;
        }

        exp = cJSON_GetNumberValue(jwt_exp);
        if (exp <= 0) {
                rd_kafka_oauthbearer_set_token_failure(
                    rk,
                    "Expected JSON JWT response with "
                    "valid \"exp\" field");
                goto done;
        }

        jwt_sub = cJSON_GetObjectItem(payloads, "sub");
        if (jwt_sub == NULL) {
                rd_kafka_oauthbearer_set_token_failure(
                    rk,
                    "Expected JSON JWT response with "
                    "\"sub\" field");
                goto done;
        }

        sub = cJSON_GetStringValue(jwt_sub);
        if (sub == NULL) {
                rd_kafka_oauthbearer_set_token_failure(
                    rk,
                    "Expected JSON JWT response with "
                    "valid \"sub\" field");
                goto done;
        }

        if (rk->rk_conf.sasl.oauthbearer.extensions_str) {
                extensions =
                    rd_string_split(rk->rk_conf.sasl.oauthbearer.extensions_str,
                                    ',', rd_true, &extension_cnt);

                extension_key_value = rd_kafka_conf_kv_split(
                    (const char **)extensions, extension_cnt,
                    &extension_key_value_cnt);
        }

        if (rd_kafka_oauthbearer_set_token(
                rk, jwt_token, (int64_t)exp * 1000, sub,
                (const char **)extension_key_value, extension_key_value_cnt,
                set_token_errstr,
                sizeof(set_token_errstr)) != RD_KAFKA_RESP_ERR_NO_ERROR)
                rd_kafka_oauthbearer_set_token_failure(rk, set_token_errstr);

done:
        RD_IF_FREE(decoded_payloads, rd_free);
        RD_IF_FREE(post_fields, rd_free);
        RD_IF_FREE(json, cJSON_Delete);
        RD_IF_FREE(headers, curl_slist_free_all);
        RD_IF_FREE(extensions, rd_free);
        RD_IF_FREE(extension_key_value, rd_free);
        RD_IF_FREE(payloads, cJSON_Delete);
}


/**
 * @brief Make sure the jwt is able to be extracted from HTTP(S) response.
 *        The JSON response after HTTP(S) call to token provider will be in
 *        rd_http_req_t.hreq_buf and jwt is the value of field "access_token",
 *        the format is {"access_token":"*******"}.
 *        This function mocks up the rd_http_req_t.hreq_buf using an dummy
 *        jwt. The rd_http_parse_json will extract the jwt from rd_http_req_t
 *        and make sure the extracted jwt is same with the dummy one.
 */
static int ut_sasl_oauthbearer_oidc_should_succeed(void) {
        /* Generate a token in the https://jwt.io/ website by using the
         * following steps:
         * 1. Select the algorithm RS256 from the Algorithm drop-down menu.
         * 2. Enter the header and the payload.
         *    payload should contains "exp", "iat", "sub", for example:
         *    payloads = {"exp": 1636532769,
                          "iat": 1516239022,
                          "sub": "sub"}
              header should contains "kid", for example:
              headers={"kid": "abcedfg"} */
        static const char *expected_jwt_token =
            "eyJhbGciOiJIUzI1NiIsInR5"
            "cCI6IkpXVCIsImtpZCI6ImFiY2VkZmcifQ"
            "."
            "eyJpYXQiOjE2MzIzNzUzMjAsInN1YiI6InN"
            "1YiIsImV4cCI6MTYzMjM3NTYyMH0"
            "."
            "bT5oY8K-rS2gQ7Awc40844bK3zhzBhZb7sputErqQHY";
        char *expected_token_value;
        size_t token_len;
        rd_http_req_t hreq;
        rd_http_error_t *herr;
        cJSON *json = NULL;
        char *token;
        cJSON *parsed_token;

        RD_UT_BEGIN();

        herr = rd_http_req_init(&hreq, "");

        RD_UT_ASSERT(!herr,
                     "Expected initialize to succeed, "
                     "but failed with error code: %d, error string: %s",
                     herr->code, herr->errstr);

        token_len = strlen("access_token") + strlen(expected_jwt_token) + 8;

        expected_token_value = rd_malloc(token_len);
        rd_snprintf(expected_token_value, token_len, "{\"%s\":\"%s\"}",
                    "access_token", expected_jwt_token);
        rd_buf_write(hreq.hreq_buf, expected_token_value, token_len);

        herr = rd_http_parse_json(&hreq, &json);
        RD_UT_ASSERT(!herr,
                     "Failed to parse JSON token: error code: %d, "
                     "error string: %s",
                     herr->code, herr->errstr);

        RD_UT_ASSERT(json, "Expected non-empty json.");

        parsed_token = cJSON_GetObjectItem(json, "access_token");

        RD_UT_ASSERT(parsed_token, "Expected access_token in JSON response.");
        token = parsed_token->valuestring;

        RD_UT_ASSERT(!strcmp(expected_jwt_token, token),
                     "Incorrect token received: "
                     "expected=%s; received=%s",
                     expected_jwt_token, token);

        rd_free(expected_token_value);
        rd_http_error_destroy(herr);
        rd_http_req_destroy(&hreq);
        cJSON_Delete(json);

        RD_UT_PASS();
}


/**
 * @brief Make sure JSON doesn't include the "access_token" key,
 *        it will fail and return an empty token.
 */
static int ut_sasl_oauthbearer_oidc_with_empty_key(void) {
        static const char *empty_token_format = "{}";
        size_t token_len;
        rd_http_req_t hreq;
        rd_http_error_t *herr;
        cJSON *json = NULL;
        cJSON *parsed_token;

        RD_UT_BEGIN();

        herr = rd_http_req_init(&hreq, "");
        RD_UT_ASSERT(!herr,
                     "Expected initialization to succeed, "
                     "but it failed with error code: %d, error string: %s",
                     herr->code, herr->errstr);

        token_len = strlen(empty_token_format);

        rd_buf_write(hreq.hreq_buf, empty_token_format, token_len);

        herr = rd_http_parse_json(&hreq, &json);

        RD_UT_ASSERT(!herr,
                     "Expected JSON token parsing to succeed, "
                     "but it failed with error code: %d, error string: %s",
                     herr->code, herr->errstr);

        RD_UT_ASSERT(json, "Expected non-empty json.");

        parsed_token = cJSON_GetObjectItem(json, "access_token");

        RD_UT_ASSERT(!parsed_token,
                     "Did not expecte access_token in JSON response");

        rd_http_req_destroy(&hreq);
        rd_http_error_destroy(herr);
        cJSON_Delete(json);
        cJSON_Delete(parsed_token);
        RD_UT_PASS();
}

/**
 * @brief Make sure the post_fields return correct with the scope.
 */
static int ut_sasl_oauthbearer_oidc_post_fields(void) {
        static const char *scope = "test-scope";
        static const char *expected_post_fields =
            "grant_type=client_credentials&scope=test-scope";

        size_t expected_post_fields_size = strlen(expected_post_fields);

        size_t post_fields_size;

        char *post_fields;

        RD_UT_BEGIN();

        rd_kafka_oidc_build_post_fields(scope, &post_fields, &post_fields_size);

        RD_UT_ASSERT(expected_post_fields_size == post_fields_size,
                     "Expected expected_post_fields_size is %" PRIusz
                     " received post_fields_size is %" PRIusz,
                     expected_post_fields_size, post_fields_size);
        RD_UT_ASSERT(!strcmp(expected_post_fields, post_fields),
                     "Expected expected_post_fields is %s"
                     " received post_fields is %s",
                     expected_post_fields, post_fields);

        rd_free(post_fields);

        RD_UT_PASS();
}

/**
 * @brief Make sure the post_fields return correct with the empty scope.
 */
static int ut_sasl_oauthbearer_oidc_post_fields_with_empty_scope(void) {
        static const char *scope = NULL;
        static const char *expected_post_fields =
            "grant_type=client_credentials";

        size_t expected_post_fields_size = strlen(expected_post_fields);

        size_t post_fields_size;

        char *post_fields;

        RD_UT_BEGIN();

        rd_kafka_oidc_build_post_fields(scope, &post_fields, &post_fields_size);

        RD_UT_ASSERT(expected_post_fields_size == post_fields_size,
                     "Expected expected_post_fields_size is %" PRIusz
                     " received post_fields_size is %" PRIusz,
                     expected_post_fields_size, post_fields_size);
        RD_UT_ASSERT(!strcmp(expected_post_fields, post_fields),
                     "Expected expected_post_fields is %s"
                     " received post_fields is %s",
                     expected_post_fields, post_fields);

        rd_free(post_fields);

        RD_UT_PASS();
}


/**
 * @brief make sure the jwt is able to be extracted from HTTP(S) requests
 *        or fail as expected.
 */
int unittest_sasl_oauthbearer_oidc(void) {
        int fails = 0;
        fails += ut_sasl_oauthbearer_oidc_should_succeed();
        fails += ut_sasl_oauthbearer_oidc_with_empty_key();
        fails += ut_sasl_oauthbearer_oidc_post_fields();
        fails += ut_sasl_oauthbearer_oidc_post_fields_with_empty_scope();
        return fails;
}
