/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2021-2022, Magnus Edenhill
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
 * @name HTTP client
 *
 */

#include "rdkafka_int.h"
#include "rdunittest.h"

#include <stdarg.h>

#include <curl/curl.h>
#include "rdhttp.h"

/** Maximum response size, increase as necessary. */
#define RD_HTTP_RESPONSE_SIZE_MAX 1024 * 1024 * 500 /* 500kb */


void rd_http_error_destroy(rd_http_error_t *herr) {
        rd_free(herr);
}

static rd_http_error_t *rd_http_error_new(int code, const char *fmt, ...)
    RD_FORMAT(printf, 2, 3);
static rd_http_error_t *rd_http_error_new(int code, const char *fmt, ...) {
        size_t len = 0;
        rd_http_error_t *herr;
        va_list ap;

        va_start(ap, fmt);

        if (fmt && *fmt) {
                va_list ap2;
                va_copy(ap2, ap);
                len = rd_vsnprintf(NULL, 0, fmt, ap2);
                va_end(ap2);
        }

        /* Use single allocation for both herr and the error string */
        herr         = rd_malloc(sizeof(*herr) + len + 1);
        herr->code   = code;
        herr->errstr = herr->data;

        if (len > 0)
                rd_vsnprintf(herr->errstr, len + 1, fmt, ap);
        else
                herr->errstr[0] = '\0';

        va_end(ap);

        return herr;
}

/**
 * @brief Same as rd_http_error_new() but reads the error string from the
 *        provided buffer.
 */
static rd_http_error_t *rd_http_error_new_from_buf(int code,
                                                   const rd_buf_t *rbuf) {
        rd_http_error_t *herr;
        rd_slice_t slice;
        size_t len = rd_buf_len(rbuf);

        if (len == 0)
                return rd_http_error_new(
                    code, "Server did not provide an error string");


        /* Use single allocation for both herr and the error string */
        herr         = rd_malloc(sizeof(*herr) + len + 1);
        herr->code   = code;
        herr->errstr = herr->data;
        rd_slice_init_full(&slice, rbuf);
        rd_slice_read(&slice, herr->errstr, len);
        herr->errstr[len] = '\0';

        return herr;
}

void rd_http_req_destroy(rd_http_req_t *hreq) {
        RD_IF_FREE(hreq->hreq_curl, curl_easy_cleanup);
        RD_IF_FREE(hreq->hreq_buf, rd_buf_destroy_free);
}


/**
 * @brief Curl writefunction. Writes the bytes passed from curl
 *        to the hreq's buffer.
 */
static size_t
rd_http_req_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
        rd_http_req_t *hreq = (rd_http_req_t *)userdata;

        if (unlikely(rd_buf_len(hreq->hreq_buf) + nmemb >
                     RD_HTTP_RESPONSE_SIZE_MAX))
                return 0; /* FIXME: Set some overflow flag or rely on curl? */

        rd_buf_write(hreq->hreq_buf, ptr, nmemb);

        return nmemb;
}

rd_http_error_t *rd_http_req_init(rd_http_req_t *hreq, const char *url) {

        memset(hreq, 0, sizeof(*hreq));

        hreq->hreq_curl = curl_easy_init();
        if (!hreq->hreq_curl)
                return rd_http_error_new(-1, "Failed to create curl handle");

        hreq->hreq_buf = rd_buf_new(1, 1024);

        curl_easy_setopt(hreq->hreq_curl, CURLOPT_URL, url);
        curl_easy_setopt(hreq->hreq_curl, CURLOPT_PROTOCOLS,
                         CURLPROTO_HTTP | CURLPROTO_HTTPS);
        curl_easy_setopt(hreq->hreq_curl, CURLOPT_MAXREDIRS, 16);
        curl_easy_setopt(hreq->hreq_curl, CURLOPT_TIMEOUT, 30);
        curl_easy_setopt(hreq->hreq_curl, CURLOPT_ERRORBUFFER,
                         hreq->hreq_curl_errstr);
        curl_easy_setopt(hreq->hreq_curl, CURLOPT_NOSIGNAL, 1);
        curl_easy_setopt(hreq->hreq_curl, CURLOPT_WRITEFUNCTION,
                         rd_http_req_write_cb);
        curl_easy_setopt(hreq->hreq_curl, CURLOPT_WRITEDATA, (void *)hreq);

        return NULL;
}

/**
 * @brief Synchronously (blockingly) perform the HTTP operation.
 */
rd_http_error_t *rd_http_req_perform_sync(rd_http_req_t *hreq) {
        CURLcode res;
        long code = 0;

        res = curl_easy_perform(hreq->hreq_curl);
        if (unlikely(res != CURLE_OK))
                return rd_http_error_new(-1, "%s", hreq->hreq_curl_errstr);

        curl_easy_getinfo(hreq->hreq_curl, CURLINFO_RESPONSE_CODE, &code);
        hreq->hreq_code = (int)code;
        if (hreq->hreq_code >= 400)
                return rd_http_error_new_from_buf(hreq->hreq_code,
                                                  hreq->hreq_buf);

        return NULL;
}


int rd_http_req_get_code(const rd_http_req_t *hreq) {
        return hreq->hreq_code;
}

const char *rd_http_req_get_content_type(rd_http_req_t *hreq) {
        const char *content_type = NULL;

        if (curl_easy_getinfo(hreq->hreq_curl, CURLINFO_CONTENT_TYPE,
                              &content_type))
                return NULL;

        return content_type;
}


/**
 * @brief Perform a blocking HTTP(S) request to \p url.
 *
 * Returns the response (even if there's a HTTP error code returned)
 * in \p *rbufp.
 *
 * Returns NULL on success (HTTP response code < 400), or an error
 * object on transport or HTTP error - this error object must be destroyed
 * by calling rd_http_error_destroy(). In case of HTTP error the \p *rbufp
 * may be filled with the error response.
 */
rd_http_error_t *rd_http_get(const char *url, rd_buf_t **rbufp) {
        rd_http_req_t hreq;
        rd_http_error_t *herr;

        *rbufp = NULL;

        herr = rd_http_req_init(&hreq, url);
        if (unlikely(herr != NULL))
                return herr;

        herr = rd_http_req_perform_sync(&hreq);
        if (herr) {
                rd_http_req_destroy(&hreq);
                return herr;
        }

        *rbufp        = hreq.hreq_buf;
        hreq.hreq_buf = NULL;

        return NULL;
}


/**
 * @brief Extract the JSON object from \p hreq and return it in \p *jsonp.
 *
 * @returns Returns NULL on success, or an JSON parsing error - this
 *          error object must be destroyed by calling rd_http_error_destroy().
 */
rd_http_error_t *rd_http_parse_json(rd_http_req_t *hreq, cJSON **jsonp) {
        size_t len;
        char *raw_json;
        const char *end = NULL;
        rd_slice_t slice;
        rd_http_error_t *herr = NULL;

        /* cJSON requires the entire input to parse in contiguous memory. */
        rd_slice_init_full(&slice, hreq->hreq_buf);
        len = rd_buf_len(hreq->hreq_buf);

        raw_json = rd_malloc(len + 1);
        rd_slice_read(&slice, raw_json, len);
        raw_json[len] = '\0';

        /* Parse JSON */
        *jsonp = cJSON_ParseWithOpts(raw_json, &end, 0);

        if (!*jsonp)
                herr = rd_http_error_new(hreq->hreq_code,
                                         "Failed to parse JSON response "
                                         "at %" PRIusz "/%" PRIusz,
                                         (size_t)(end - raw_json), len);
        rd_free(raw_json);
        return herr;
}


/**
 * @brief Check if the error returned from HTTP(S) is temporary or not.
 *
 * @returns If the \p error_code is temporary, return rd_true,
 *          otherwise return rd_false.
 *
 * @locality Any thread.
 */
static rd_bool_t rd_http_is_failure_temporary(int error_code) {
        switch (error_code) {
        case 408: /**< Request timeout */
        case 425: /**< Too early */
        case 500: /**< Internal server error */
        case 502: /**< Bad gateway */
        case 503: /**< Service unavailable */
        case 504: /**< Gateway timeout */
                return rd_true;

        default:
                return rd_false;
        }
}


/**
 * @brief Perform a blocking HTTP(S) request to \p url with
 *        HTTP(S) headers and data with \p timeout_s.
 *        If the HTTP(S) request fails, will retry another \p retries times
 *        with multiplying backoff \p retry_ms.
 *
 * @returns The result will be returned in \p *jsonp.
 *          Returns NULL on success (HTTP response code < 400), or an error
 *          object on transport, HTTP error or a JSON parsing error - this
 *          error object must be destroyed by calling rd_http_error_destroy().
 *
 * @locality Any thread.
 */
rd_http_error_t *rd_http_post_expect_json(rd_kafka_t *rk,
                                          const char *url,
                                          const struct curl_slist *headers,
                                          const char *post_fields,
                                          size_t post_fields_size,
                                          int timeout_s,
                                          int retries,
                                          int retry_ms,
                                          cJSON **jsonp) {
        rd_http_error_t *herr;
        rd_http_req_t hreq;
        int i;
        size_t len;
        const char *content_type;

        herr = rd_http_req_init(&hreq, url);
        if (unlikely(herr != NULL))
                return herr;

        curl_easy_setopt(hreq.hreq_curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(hreq.hreq_curl, CURLOPT_TIMEOUT, timeout_s);

        curl_easy_setopt(hreq.hreq_curl, CURLOPT_POSTFIELDSIZE,
                         post_fields_size);
        curl_easy_setopt(hreq.hreq_curl, CURLOPT_POSTFIELDS, post_fields);

        for (i = 0; i <= retries; i++) {
                if (rd_kafka_terminating(rk)) {
                        rd_http_req_destroy(&hreq);
                        return rd_http_error_new(-1, "Terminating");
                }

                herr = rd_http_req_perform_sync(&hreq);
                len  = rd_buf_len(hreq.hreq_buf);

                if (!herr) {
                        if (len > 0)
                                break; /* Success */
                        /* Empty response */
                        rd_http_req_destroy(&hreq);
                        return NULL;
                }
                /* Retry if HTTP(S) request returns temporary error and there
                 * are remaining retries, else fail. */
                if (i == retries || !rd_http_is_failure_temporary(herr->code)) {
                        rd_http_req_destroy(&hreq);
                        return herr;
                }

                /* Retry */
                rd_http_error_destroy(herr);
                rd_usleep(retry_ms * 1000 * (i + 1), &rk->rk_terminate);
        }

        content_type = rd_http_req_get_content_type(&hreq);

        if (!content_type || rd_strncasecmp(content_type, "application/json",
                                            strlen("application/json"))) {
                if (!herr)
                        herr = rd_http_error_new(
                            hreq.hreq_code, "Response is not JSON encoded: %s",
                            content_type ? content_type : "(n/a)");
                rd_http_req_destroy(&hreq);
                return herr;
        }

        herr = rd_http_parse_json(&hreq, jsonp);

        rd_http_req_destroy(&hreq);

        return herr;
}


/**
 * @brief Same as rd_http_get() but requires a JSON response.
 *        The response is parsed and a JSON object is returned in \p *jsonp.
 *
 * Same error semantics as rd_http_get().
 */
rd_http_error_t *rd_http_get_json(const char *url, cJSON **jsonp) {
        rd_http_req_t hreq;
        rd_http_error_t *herr;
        rd_slice_t slice;
        size_t len;
        const char *content_type;
        char *raw_json;
        const char *end;

        *jsonp = NULL;

        herr = rd_http_req_init(&hreq, url);
        if (unlikely(herr != NULL))
                return herr;

        // FIXME: send Accept: json.. header?

        herr = rd_http_req_perform_sync(&hreq);
        len  = rd_buf_len(hreq.hreq_buf);
        if (herr && len == 0) {
                rd_http_req_destroy(&hreq);
                return herr;
        }

        if (len == 0) {
                /* Empty response: create empty JSON object */
                *jsonp = cJSON_CreateObject();
                rd_http_req_destroy(&hreq);
                return NULL;
        }

        content_type = rd_http_req_get_content_type(&hreq);

        if (!content_type || rd_strncasecmp(content_type, "application/json",
                                            strlen("application/json"))) {
                if (!herr)
                        herr = rd_http_error_new(
                            hreq.hreq_code, "Response is not JSON encoded: %s",
                            content_type ? content_type : "(n/a)");
                rd_http_req_destroy(&hreq);
                return herr;
        }

        /* cJSON requires the entire input to parse in contiguous memory. */
        rd_slice_init_full(&slice, hreq.hreq_buf);
        raw_json = rd_malloc(len + 1);
        rd_slice_read(&slice, raw_json, len);
        raw_json[len] = '\0';

        /* Parse JSON */
        end    = NULL;
        *jsonp = cJSON_ParseWithOpts(raw_json, &end, 0);
        if (!*jsonp && !herr)
                herr = rd_http_error_new(hreq.hreq_code,
                                         "Failed to parse JSON response "
                                         "at %" PRIusz "/%" PRIusz,
                                         (size_t)(end - raw_json), len);

        rd_free(raw_json);
        rd_http_req_destroy(&hreq);

        return herr;
}


void rd_http_global_init(void) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
}


/**
 * @brief Unittest. Requires a (local) webserver to be set with env var
 *        RD_UT_HTTP_URL=http://localhost:1234/some-path
 *
 * This server must return a JSON object or array containing at least one
 * object on the main URL with a 2xx response code,
 * and 4xx response on $RD_UT_HTTP_URL/error (with whatever type of body).
 */

int unittest_http(void) {
        const char *base_url = rd_getenv("RD_UT_HTTP_URL", NULL);
        char *error_url;
        size_t error_url_size;
        cJSON *json, *jval;
        rd_http_error_t *herr;
        rd_bool_t empty;

        if (!base_url || !*base_url)
                RD_UT_SKIP("RD_UT_HTTP_URL environment variable not set");

        RD_UT_BEGIN();

        error_url_size = strlen(base_url) + strlen("/error") + 1;
        error_url      = rd_alloca(error_url_size);
        rd_snprintf(error_url, error_url_size, "%s/error", base_url);

        /* Try the base url first, parse its JSON and extract a key-value. */
        json = NULL;
        herr = rd_http_get_json(base_url, &json);
        RD_UT_ASSERT(!herr, "Expected get_json(%s) to succeed, got: %s",
                     base_url, herr->errstr);

        empty = rd_true;
        cJSON_ArrayForEach(jval, json) {
                empty = rd_false;
                break;
        }
        RD_UT_ASSERT(!empty, "Expected non-empty JSON response from %s",
                     base_url);
        RD_UT_SAY(
            "URL %s returned no error and a non-empty "
            "JSON object/array as expected",
            base_url);
        cJSON_Delete(json);


        /* Try the error URL, verify error code. */
        json = NULL;
        herr = rd_http_get_json(error_url, &json);
        RD_UT_ASSERT(herr != NULL, "Expected get_json(%s) to fail", error_url);
        RD_UT_ASSERT(herr->code >= 400,
                     "Expected get_json(%s) error code >= "
                     "400, got %d",
                     error_url, herr->code);
        RD_UT_SAY(
            "Error URL %s returned code %d, errstr \"%s\" "
            "and %s JSON object as expected",
            error_url, herr->code, herr->errstr, json ? "a" : "no");
        /* Check if there's a JSON document returned */
        if (json)
                cJSON_Delete(json);
        rd_http_error_destroy(herr);

        RD_UT_PASS();
}
