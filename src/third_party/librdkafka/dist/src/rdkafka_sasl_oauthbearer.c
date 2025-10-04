/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2019-2022, Magnus Edenhill
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
 * Builtin SASL OAUTHBEARER support
 */
#include "rdkafka_int.h"
#include "rdkafka_transport_int.h"
#include "rdkafka_sasl_int.h"
#include <openssl/evp.h>
#include "rdunittest.h"

#if WITH_OAUTHBEARER_OIDC
#include "rdkafka_sasl_oauthbearer_oidc.h"
#endif


/**
 * @struct Per-client-instance SASL/OAUTHBEARER handle.
 */
typedef struct rd_kafka_sasl_oauthbearer_handle_s {
        /**< Read-write lock for fields in the handle. */
        rwlock_t lock;

        /**< The b64token value as defined in RFC 6750 Section 2.1
         *   https://tools.ietf.org/html/rfc6750#section-2.1
         */
        char *token_value;

        /**< When the token expires, in terms of the number of
         *   milliseconds since the epoch. Wall clock time.
         */
        rd_ts_t wts_md_lifetime;

        /**< The point after which this token should be replaced with a
         * new one, in terms of the number of milliseconds since the
         * epoch. Wall clock time.
         */
        rd_ts_t wts_refresh_after;

        /**< When the last token refresh was equeued (0 = never)
         *   in terms of the number of milliseconds since the epoch.
         *   Wall clock time.
         */
        rd_ts_t wts_enqueued_refresh;

        /**< The name of the principal to which this token applies. */
        char *md_principal_name;

        /**< The SASL extensions, as per RFC 7628 Section 3.1
         *   https://tools.ietf.org/html/rfc7628#section-3.1
         */
        rd_list_t extensions; /* rd_strtup_t list */

        /**< Error message for validation and/or token retrieval problems. */
        char *errstr;

        /**< Back-pointer to client instance. */
        rd_kafka_t *rk;

        /**< Token refresh timer */
        rd_kafka_timer_t token_refresh_tmr;

        /** Queue to enqueue token_refresh_cb ops on. */
        rd_kafka_q_t *callback_q;

        /** Using internal refresh callback (sasl.oauthbearer.method=oidc) */
        rd_bool_t internal_refresh;

} rd_kafka_sasl_oauthbearer_handle_t;


/**
 * @struct Unsecured JWS info populated when sasl.oauthbearer.config is parsed
 */
struct rd_kafka_sasl_oauthbearer_parsed_ujws {
        char *principal_claim_name;
        char *principal;
        char *scope_claim_name;
        char *scope_csv_text;
        int life_seconds;
        rd_list_t extensions; /* rd_strtup_t list */
};

/**
 * @struct Unsecured JWS token to be set on the client handle
 */
struct rd_kafka_sasl_oauthbearer_token {
        char *token_value;
        int64_t md_lifetime_ms;
        char *md_principal_name;
        char **extensions;
        size_t extension_size;
};

/**
 * @brief Per-connection state
 */
struct rd_kafka_sasl_oauthbearer_state {
        enum { RD_KAFKA_SASL_OAUTHB_STATE_SEND_CLIENT_FIRST_MESSAGE,
               RD_KAFKA_SASL_OAUTHB_STATE_RECV_SERVER_FIRST_MSG,
               RD_KAFKA_SASL_OAUTHB_STATE_RECV_SERVER_MSG_AFTER_FAIL,
        } state;
        char *server_error_msg;

        /*
         * A place to store a consistent view of the token and extensions
         * throughout the authentication process -- even if it is refreshed
         * midway through this particular authentication.
         */
        char *token_value;
        char *md_principal_name;
        rd_list_t extensions; /* rd_strtup_t list */
};



/**
 * @brief free memory inside the given token
 */
static void rd_kafka_sasl_oauthbearer_token_free(
    struct rd_kafka_sasl_oauthbearer_token *token) {
        size_t i;

        RD_IF_FREE(token->token_value, rd_free);
        RD_IF_FREE(token->md_principal_name, rd_free);

        for (i = 0; i < token->extension_size; i++)
                rd_free(token->extensions[i]);

        RD_IF_FREE(token->extensions, rd_free);

        memset(token, 0, sizeof(*token));
}


/**
 * @brief Op callback for RD_KAFKA_OP_OAUTHBEARER_REFRESH
 *
 * @locality Application thread
 */
static rd_kafka_op_res_t rd_kafka_oauthbearer_refresh_op(rd_kafka_t *rk,
                                                         rd_kafka_q_t *rkq,
                                                         rd_kafka_op_t *rko) {
        /* The op callback is invoked when the op is destroyed via
         * rd_kafka_op_destroy() or rd_kafka_event_destroy(), so
         * make sure we don't refresh upon destruction since
         * the op has already been handled by this point.
         */
        if (rko->rko_err != RD_KAFKA_RESP_ERR__DESTROY &&
            rk->rk_conf.sasl.oauthbearer.token_refresh_cb)
                rk->rk_conf.sasl.oauthbearer.token_refresh_cb(
                    rk, rk->rk_conf.sasl.oauthbearer_config,
                    rk->rk_conf.opaque);
        return RD_KAFKA_OP_RES_HANDLED;
}

/**
 * @brief Enqueue a token refresh.
 * @locks rwlock_wrlock(&handle->lock) MUST be held
 */
static void rd_kafka_oauthbearer_enqueue_token_refresh(
    rd_kafka_sasl_oauthbearer_handle_t *handle) {
        rd_kafka_op_t *rko;

        rko = rd_kafka_op_new_cb(handle->rk, RD_KAFKA_OP_OAUTHBEARER_REFRESH,
                                 rd_kafka_oauthbearer_refresh_op);
        rd_kafka_op_set_prio(rko, RD_KAFKA_PRIO_FLASH);

        /* For internal OIDC refresh callback:
         * Force op to be handled by internal callback on the
         * receiving queue, rather than being passed as an event to
         * the application. */
        if (handle->internal_refresh)
                rko->rko_flags |= RD_KAFKA_OP_F_FORCE_CB;

        handle->wts_enqueued_refresh = rd_uclock();
        rd_kafka_q_enq(handle->callback_q, rko);
}

/**
 * @brief Enqueue a token refresh if necessary.
 *
 * The method rd_kafka_oauthbearer_enqueue_token_refresh() is invoked
 * if necessary; the required lock is acquired and released.  This method
 * returns immediately when SASL/OAUTHBEARER is not in use by the client.
 */
static void rd_kafka_oauthbearer_enqueue_token_refresh_if_necessary(
    rd_kafka_sasl_oauthbearer_handle_t *handle) {
        rd_ts_t now_wallclock;

        now_wallclock = rd_uclock();

        rwlock_wrlock(&handle->lock);
        if (handle->wts_refresh_after < now_wallclock &&
            handle->wts_enqueued_refresh <= handle->wts_refresh_after)
                /* Refresh required and not yet scheduled; refresh it */
                rd_kafka_oauthbearer_enqueue_token_refresh(handle);
        rwlock_wrunlock(&handle->lock);
}

/**
 * @returns \c rd_true if SASL/OAUTHBEARER is the configured authentication
 *           mechanism and a token is available, otherwise \c rd_false.
 *
 * @locks none
 * @locality any
 */
static rd_bool_t
rd_kafka_oauthbearer_has_token(rd_kafka_sasl_oauthbearer_handle_t *handle) {
        rd_bool_t retval_has_token;

        rwlock_rdlock(&handle->lock);
        retval_has_token = handle->token_value != NULL;
        rwlock_rdunlock(&handle->lock);

        return retval_has_token;
}

/**
 * @brief Verify that the provided \p key is valid.
 * @returns 0 on success or -1 if \p key is invalid.
 */
static int check_oauthbearer_extension_key(const char *key,
                                           char *errstr,
                                           size_t errstr_size) {
        const char *c;

        if (!strcmp(key, "auth")) {
                rd_snprintf(errstr, errstr_size,
                            "Cannot explicitly set the reserved `auth` "
                            "SASL/OAUTHBEARER extension key");
                return -1;
        }

        /*
         * https://tools.ietf.org/html/rfc7628#section-3.1
         * key            = 1*(ALPHA)
         *
         * https://tools.ietf.org/html/rfc5234#appendix-B.1
         * ALPHA          =  %x41-5A / %x61-7A   ; A-Z / a-z
         */
        if (!*key) {
                rd_snprintf(errstr, errstr_size,
                            "SASL/OAUTHBEARER extension keys "
                            "must not be empty");
                return -1;
        }

        for (c = key; *c; c++) {
                if (!(*c >= 'A' && *c <= 'Z') && !(*c >= 'a' && *c <= 'z')) {
                        rd_snprintf(errstr, errstr_size,
                                    "SASL/OAUTHBEARER extension keys must "
                                    "only consist of A-Z or "
                                    "a-z characters: %s (%c)",
                                    key, *c);
                        return -1;
                }
        }

        return 0;
}

/**
 * @brief Verify that the provided \p value is valid.
 * @returns 0 on success or -1 if \p value is invalid.
 */
static int check_oauthbearer_extension_value(const char *value,
                                             char *errstr,
                                             size_t errstr_size) {
        const char *c;

        /*
         * https://tools.ietf.org/html/rfc7628#section-3.1
         * value          = *(VCHAR / SP / HTAB / CR / LF )
         *
         * https://tools.ietf.org/html/rfc5234#appendix-B.1
         * VCHAR          =  %x21-7E  ; visible (printing) characters
         * SP             =  %x20     ; space
         * HTAB           =  %x09     ; horizontal tab
         * CR             =  %x0D     ; carriage return
         * LF             =  %x0A     ; linefeed
         */
        for (c = value; *c; c++) {
                if (!(*c >= '\x21' && *c <= '\x7E') && *c != '\x20' &&
                    *c != '\x09' && *c != '\x0D' && *c != '\x0A') {
                        rd_snprintf(errstr, errstr_size,
                                    "SASL/OAUTHBEARER extension values must "
                                    "only consist of space, horizontal tab, "
                                    "CR, LF, and "
                                    "visible characters (%%x21-7E): %s (%c)",
                                    value, *c);
                        return -1;
                }
        }

        return 0;
}

/**
 * @brief Set SASL/OAUTHBEARER token and metadata
 *
 * @param rk Client instance.
 * @param token_value the mandatory token value to set, often (but not
 *  necessarily) a JWS compact serialization as per
 *  https://tools.ietf.org/html/rfc7515#section-3.1.
 *  Use rd_kafka_sasl_oauthbearer_token_free() to free members if
 *  return value is not -1.
 * @param md_lifetime_ms when the token expires, in terms of the number of
 *  milliseconds since the epoch. See https://currentmillis.com/.
 * @param md_principal_name the mandatory Kafka principal name associated
 *  with the token.
 * @param extensions optional SASL extensions key-value array with
 *  \p extensions_size elements (number of keys * 2), where [i] is the key and
 *  [i+1] is the key's value, to be communicated to the broker
 *  as additional key-value pairs during the initial client response as per
 *  https://tools.ietf.org/html/rfc7628#section-3.1.
 * @param extension_size the number of SASL extension keys plus values,
 *  which should be a non-negative multiple of 2.
 *
 * The SASL/OAUTHBEARER token refresh callback or event handler should cause
 * this method to be invoked upon success, via
 * rd_kafka_oauthbearer_set_token(). The extension keys must not include the
 * reserved key "`auth`", and all extension keys and values must conform to the
 * required format as per https://tools.ietf.org/html/rfc7628#section-3.1:
 *
 * key            = 1*(ALPHA)
 * value          = *(VCHAR / SP / HTAB / CR / LF )
 *
 * @returns \c RD_KAFKA_RESP_ERR_NO_ERROR on success, otherwise errstr set and:
 *          \c RD_KAFKA_RESP_ERR__INVALID_ARG if any of the arguments are
 *              invalid;
 *          \c RD_KAFKA_RESP_ERR__STATE if SASL/OAUTHBEARER is not configured as
 *              the client's authentication mechanism.
 *
 * @sa rd_kafka_oauthbearer_set_token_failure0
 */
rd_kafka_resp_err_t
rd_kafka_oauthbearer_set_token0(rd_kafka_t *rk,
                                const char *token_value,
                                int64_t md_lifetime_ms,
                                const char *md_principal_name,
                                const char **extensions,
                                size_t extension_size,
                                char *errstr,
                                size_t errstr_size) {
        rd_kafka_sasl_oauthbearer_handle_t *handle = rk->rk_sasl.handle;
        size_t i;
        rd_ts_t now_wallclock;
        rd_ts_t wts_md_lifetime = md_lifetime_ms * 1000;

        /* Check if SASL/OAUTHBEARER is the configured auth mechanism */
        if (rk->rk_conf.sasl.provider != &rd_kafka_sasl_oauthbearer_provider ||
            !handle) {
                rd_snprintf(errstr, errstr_size,
                            "SASL/OAUTHBEARER is not the "
                            "configured authentication mechanism");
                return RD_KAFKA_RESP_ERR__STATE;
        }

        /* Check if there is an odd number of extension keys + values */
        if (extension_size & 1) {
                rd_snprintf(errstr, errstr_size,
                            "Incorrect extension size "
                            "(must be a non-negative multiple of 2): %" PRIusz,
                            extension_size);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        /* Check args for correct format/value */
        now_wallclock = rd_uclock();
        if (wts_md_lifetime <= now_wallclock) {
                rd_snprintf(errstr, errstr_size,
                            "Must supply an unexpired token: "
                            "now=%" PRId64 "ms, exp=%" PRId64 "ms",
                            now_wallclock / 1000, wts_md_lifetime / 1000);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        if (check_oauthbearer_extension_value(token_value, errstr,
                                              errstr_size) == -1)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        for (i = 0; i + 1 < extension_size; i += 2) {
                if (check_oauthbearer_extension_key(extensions[i], errstr,
                                                    errstr_size) == -1 ||
                    check_oauthbearer_extension_value(extensions[i + 1], errstr,
                                                      errstr_size) == -1)
                        return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        rwlock_wrlock(&handle->lock);

        RD_IF_FREE(handle->md_principal_name, rd_free);
        handle->md_principal_name = rd_strdup(md_principal_name);

        RD_IF_FREE(handle->token_value, rd_free);
        handle->token_value = rd_strdup(token_value);

        handle->wts_md_lifetime = wts_md_lifetime;

        /* Schedule a refresh 80% through its remaining lifetime */
        handle->wts_refresh_after =
            (rd_ts_t)(now_wallclock + 0.8 * (wts_md_lifetime - now_wallclock));

        rd_list_clear(&handle->extensions);
        for (i = 0; i + 1 < extension_size; i += 2)
                rd_list_add(&handle->extensions,
                            rd_strtup_new(extensions[i], extensions[i + 1]));

        RD_IF_FREE(handle->errstr, rd_free);
        handle->errstr = NULL;

        rwlock_wrunlock(&handle->lock);

        rd_kafka_dbg(rk, SECURITY, "BRKMAIN",
                     "Waking up waiting broker threads after "
                     "setting OAUTHBEARER token");
        rd_kafka_all_brokers_wakeup(rk, RD_KAFKA_BROKER_STATE_TRY_CONNECT,
                                    "OAUTHBEARER token update");

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief SASL/OAUTHBEARER token refresh failure indicator.
 *
 * @param rk Client instance.
 * @param errstr mandatory human readable error reason for failing to acquire
 *  a token.
 *
 * The SASL/OAUTHBEARER token refresh callback or event handler should cause
 * this method to be invoked upon failure, via
 * rd_kafka_oauthbearer_set_token_failure().
 *
 * @returns \c RD_KAFKA_RESP_ERR_NO_ERROR on success, otherwise
 *          \c RD_KAFKA_RESP_ERR__STATE if SASL/OAUTHBEARER is enabled but is
 *              not configured to be the client's authentication mechanism,
 *          \c RD_KAFKA_RESP_ERR__INVALID_ARG if no error string is supplied.

 * @sa rd_kafka_oauthbearer_set_token0
 */
rd_kafka_resp_err_t
rd_kafka_oauthbearer_set_token_failure0(rd_kafka_t *rk, const char *errstr) {
        rd_kafka_sasl_oauthbearer_handle_t *handle = rk->rk_sasl.handle;
        rd_bool_t error_changed;

        /* Check if SASL/OAUTHBEARER is the configured auth mechanism */
        if (rk->rk_conf.sasl.provider != &rd_kafka_sasl_oauthbearer_provider ||
            !handle)
                return RD_KAFKA_RESP_ERR__STATE;

        if (!errstr || !*errstr)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        rwlock_wrlock(&handle->lock);
        error_changed = !handle->errstr || strcmp(handle->errstr, errstr);
        RD_IF_FREE(handle->errstr, rd_free);
        handle->errstr = rd_strdup(errstr);
        /* Leave any existing token because it may have some life left,
         * schedule a refresh for 10 seconds later. */
        handle->wts_refresh_after = rd_uclock() + (10 * 1000 * 1000);
        rwlock_wrunlock(&handle->lock);

        /* Trigger an ERR__AUTHENTICATION error if the error changed. */
        if (error_changed)
                rd_kafka_op_err(rk, RD_KAFKA_RESP_ERR__AUTHENTICATION,
                                "Failed to acquire SASL OAUTHBEARER token: %s",
                                errstr);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Parse a config value from the string pointed to by \p loc and starting
 * with the given \p prefix and ending with the given \p value_end_char, storing
 * the newly-allocated memory result in the string pointed to by \p value.
 * @returns -1 if string pointed to by \p value is non-empty (\p errstr set, no
 * memory allocated), else 0 (caller must free allocated memory).
 */
static int parse_ujws_config_value_for_prefix(char **loc,
                                              const char *prefix,
                                              const char value_end_char,
                                              char **value,
                                              char *errstr,
                                              size_t errstr_size) {
        if (*value) {
                rd_snprintf(errstr, errstr_size,
                            "Invalid sasl.oauthbearer.config: "
                            "multiple '%s' entries",
                            prefix);
                return -1;
        }

        *loc += strlen(prefix);
        *value = *loc;
        while (**loc != '\0' && **loc != value_end_char)
                ++*loc;

        if (**loc == value_end_char) {
                /* End the string and skip the character */
                **loc = '\0';
                ++*loc;
        }

        /* return new allocated memory */
        *value = rd_strdup(*value);

        return 0;
}

/*
 * @brief Parse Unsecured JWS config, allocates strings that must be freed
 * @param cfg the config to parse (typically from `sasl.oauthbearer.config`)
 * @param parsed holds the parsed output; it must be all zeros to start.
 * @returns -1 on failure (\p errstr set), else 0.
 */
static int
parse_ujws_config(const char *cfg,
                  struct rd_kafka_sasl_oauthbearer_parsed_ujws *parsed,
                  char *errstr,
                  size_t errstr_size) {
        /*
         * Extensions:
         *
         * https://tools.ietf.org/html/rfc7628#section-3.1
         * key            = 1*(ALPHA)
         * value          = *(VCHAR / SP / HTAB / CR / LF )
         *
         * https://tools.ietf.org/html/rfc5234#appendix-B.1
         * ALPHA          =  %x41-5A / %x61-7A   ; A-Z / a-z
         * VCHAR          =  %x21-7E  ; visible (printing) characters
         * SP             =  %x20     ; space
         * HTAB           =  %x09     ; horizontal tab
         * CR             =  %x0D     ; carriage return
         * LF             =  %x0A     ; linefeed
         */

        static const char *prefix_principal_claim_name = "principalClaimName=";
        static const char *prefix_principal            = "principal=";
        static const char *prefix_scope_claim_name     = "scopeClaimName=";
        static const char *prefix_scope                = "scope=";
        static const char *prefix_life_seconds         = "lifeSeconds=";
        static const char *prefix_extension            = "extension_";

        char *cfg_copy = rd_strdup(cfg);
        char *loc      = cfg_copy;
        int r          = 0;

        while (*loc != '\0' && !r) {
                if (*loc == ' ')
                        ++loc;
                else if (!strncmp(prefix_principal_claim_name, loc,
                                  strlen(prefix_principal_claim_name))) {
                        r = parse_ujws_config_value_for_prefix(
                            &loc, prefix_principal_claim_name, ' ',
                            &parsed->principal_claim_name, errstr, errstr_size);

                        if (!r && !*parsed->principal_claim_name) {
                                rd_snprintf(errstr, errstr_size,
                                            "Invalid sasl.oauthbearer.config: "
                                            "empty '%s'",
                                            prefix_principal_claim_name);
                                r = -1;
                        }

                } else if (!strncmp(prefix_principal, loc,
                                    strlen(prefix_principal))) {
                        r = parse_ujws_config_value_for_prefix(
                            &loc, prefix_principal, ' ', &parsed->principal,
                            errstr, errstr_size);

                        if (!r && !*parsed->principal) {
                                rd_snprintf(errstr, errstr_size,
                                            "Invalid sasl.oauthbearer.config: "
                                            "empty '%s'",
                                            prefix_principal);
                                r = -1;
                        }

                } else if (!strncmp(prefix_scope_claim_name, loc,
                                    strlen(prefix_scope_claim_name))) {
                        r = parse_ujws_config_value_for_prefix(
                            &loc, prefix_scope_claim_name, ' ',
                            &parsed->scope_claim_name, errstr, errstr_size);

                        if (!r && !*parsed->scope_claim_name) {
                                rd_snprintf(errstr, errstr_size,
                                            "Invalid sasl.oauthbearer.config: "
                                            "empty '%s'",
                                            prefix_scope_claim_name);
                                r = -1;
                        }

                } else if (!strncmp(prefix_scope, loc, strlen(prefix_scope))) {
                        r = parse_ujws_config_value_for_prefix(
                            &loc, prefix_scope, ' ', &parsed->scope_csv_text,
                            errstr, errstr_size);

                        if (!r && !*parsed->scope_csv_text) {
                                rd_snprintf(errstr, errstr_size,
                                            "Invalid sasl.oauthbearer.config: "
                                            "empty '%s'",
                                            prefix_scope);
                                r = -1;
                        }

                } else if (!strncmp(prefix_life_seconds, loc,
                                    strlen(prefix_life_seconds))) {
                        char *life_seconds_text = NULL;

                        r = parse_ujws_config_value_for_prefix(
                            &loc, prefix_life_seconds, ' ', &life_seconds_text,
                            errstr, errstr_size);

                        if (!r && !*life_seconds_text) {
                                rd_snprintf(errstr, errstr_size,
                                            "Invalid "
                                            "sasl.oauthbearer.config: "
                                            "empty '%s'",
                                            prefix_life_seconds);
                                r = -1;
                        } else if (!r) {
                                long long life_seconds_long;
                                char *end_ptr;
                                life_seconds_long =
                                    strtoll(life_seconds_text, &end_ptr, 10);
                                if (*end_ptr != '\0') {
                                        rd_snprintf(errstr, errstr_size,
                                                    "Invalid "
                                                    "sasl.oauthbearer.config: "
                                                    "non-integral '%s': %s",
                                                    prefix_life_seconds,
                                                    life_seconds_text);
                                        r = -1;
                                } else if (life_seconds_long <= 0 ||
                                           life_seconds_long > INT_MAX) {
                                        rd_snprintf(errstr, errstr_size,
                                                    "Invalid "
                                                    "sasl.oauthbearer.config: "
                                                    "value out of range of "
                                                    "positive int '%s': %s",
                                                    prefix_life_seconds,
                                                    life_seconds_text);
                                        r = -1;
                                } else {
                                        parsed->life_seconds =
                                            (int)life_seconds_long;
                                }
                        }

                        RD_IF_FREE(life_seconds_text, rd_free);

                } else if (!strncmp(prefix_extension, loc,
                                    strlen(prefix_extension))) {
                        char *extension_key = NULL;

                        r = parse_ujws_config_value_for_prefix(
                            &loc, prefix_extension, '=', &extension_key, errstr,
                            errstr_size);

                        if (!r && !*extension_key) {
                                rd_snprintf(errstr, errstr_size,
                                            "Invalid "
                                            "sasl.oauthbearer.config: "
                                            "empty '%s' key",
                                            prefix_extension);
                                r = -1;
                        } else if (!r) {
                                char *extension_value = NULL;
                                r = parse_ujws_config_value_for_prefix(
                                    &loc, "", ' ', &extension_value, errstr,
                                    errstr_size);
                                if (!r) {
                                        rd_list_add(
                                            &parsed->extensions,
                                            rd_strtup_new(extension_key,
                                                          extension_value));
                                        rd_free(extension_value);
                                }
                        }

                        RD_IF_FREE(extension_key, rd_free);

                } else {
                        rd_snprintf(errstr, errstr_size,
                                    "Unrecognized sasl.oauthbearer.config "
                                    "beginning at: %s",
                                    loc);
                        r = -1;
                }
        }

        rd_free(cfg_copy);

        return r;
}

/**
 * @brief Create unsecured JWS compact serialization
 * from the given information.
 * @returns allocated memory that the caller must free.
 */
static char *create_jws_compact_serialization(
    const struct rd_kafka_sasl_oauthbearer_parsed_ujws *parsed,
    rd_ts_t now_wallclock) {
        static const char *jose_header_encoded =
            "eyJhbGciOiJub25lIn0";  // {"alg":"none"}
        int scope_json_length = 0;
        int max_json_length;
        double now_wallclock_seconds;
        char *scope_json;
        char *scope_curr;
        int i;
        char *claims_json;
        char *jws_claims;
        size_t encode_len;
        char *jws_last_char;
        char *jws_maybe_non_url_char;
        char *retval_jws;
        size_t retval_size;
        rd_list_t scope;

        rd_list_init(&scope, 0, rd_free);
        if (parsed->scope_csv_text) {
                /* Convert from csv to rd_list_t and
                 * calculate json length. */
                char *start = parsed->scope_csv_text;
                char *curr  = start;

                while (*curr != '\0') {
                        /* Ignore empty elements (e.g. ",,") */
                        while (*curr == ',') {
                                ++curr;
                                ++start;
                        }

                        while (*curr != '\0' && *curr != ',')
                                ++curr;

                        if (curr == start)
                                continue;

                        if (*curr == ',') {
                                *curr = '\0';
                                ++curr;
                        }

                        if (!rd_list_find(&scope, start, (void *)strcmp))
                                rd_list_add(&scope, rd_strdup(start));

                        if (scope_json_length == 0) {
                                scope_json_length =
                                    2 +  // ,"
                                    (int)strlen(parsed->scope_claim_name) +
                                    4 +                       // ":["
                                    (int)strlen(start) + 1 +  // "
                                    1;                        // ]
                        } else {
                                scope_json_length += 2;  // ,"
                                scope_json_length += (int)strlen(start);
                                scope_json_length += 1;  // "
                        }

                        start = curr;
                }
        }

        now_wallclock_seconds = now_wallclock / 1000000.0;

        /* Generate json */
        max_json_length = 2 +  // {"
                          (int)strlen(parsed->principal_claim_name) +
                          3 +                                   // ":"
                          (int)strlen(parsed->principal) + 8 +  // ","iat":
                          14 +  // iat NumericDate (e.g. 1549251467.546)
                          7 +   // ,"exp":
                          14 +  // exp NumericDate (e.g. 1549252067.546)
                          scope_json_length + 1;  // }

        /* Generate scope portion of json */
        scope_json  = rd_malloc(scope_json_length + 1);
        *scope_json = '\0';
        scope_curr  = scope_json;

        for (i = 0; i < rd_list_cnt(&scope); i++) {
                if (i == 0)
                        scope_curr += rd_snprintf(
                            scope_curr,
                            (size_t)(scope_json + scope_json_length + 1 -
                                     scope_curr),
                            ",\"%s\":[\"", parsed->scope_claim_name);
                else
                        scope_curr += sprintf(scope_curr, "%s", ",\"");
                scope_curr += sprintf(scope_curr, "%s\"",
                                      (const char *)rd_list_elem(&scope, i));
                if (i == rd_list_cnt(&scope) - 1)
                        scope_curr += sprintf(scope_curr, "%s", "]");
        }

        claims_json = rd_malloc(max_json_length + 1);
        rd_snprintf(claims_json, max_json_length + 1,
                    "{\"%s\":\"%s\",\"iat\":%.3f,\"exp\":%.3f%s}",
                    parsed->principal_claim_name, parsed->principal,
                    now_wallclock_seconds,
                    now_wallclock_seconds + parsed->life_seconds, scope_json);
        rd_free(scope_json);

        /* Convert to base64URL format, first to base64, then to base64URL */
        retval_size = strlen(jose_header_encoded) + 1 +
                      (((max_json_length + 2) / 3) * 4) + 1 + 1;
        retval_jws = rd_malloc(retval_size);
        rd_snprintf(retval_jws, retval_size, "%s.", jose_header_encoded);
        jws_claims = retval_jws + strlen(retval_jws);
        encode_len =
            EVP_EncodeBlock((uint8_t *)jws_claims, (uint8_t *)claims_json,
                            (int)strlen(claims_json));
        rd_free(claims_json);
        jws_last_char = jws_claims + encode_len - 1;

        /* Convert from padded base64 to unpadded base64URL
         * and eliminate any padding. */
        while (jws_last_char >= jws_claims && *jws_last_char == '=')
                --jws_last_char;
        *(++jws_last_char)   = '.';
        *(jws_last_char + 1) = '\0';

        /* Convert the 2 differing encode characters */
        for (jws_maybe_non_url_char = retval_jws; *jws_maybe_non_url_char;
             jws_maybe_non_url_char++)
                if (*jws_maybe_non_url_char == '+')
                        *jws_maybe_non_url_char = '-';
                else if (*jws_maybe_non_url_char == '/')
                        *jws_maybe_non_url_char = '_';

        rd_list_destroy(&scope);

        return retval_jws;
}

/**
 * @brief Same as rd_kafka_oauthbearer_unsecured_token() except it takes
 * additional explicit arguments and return a status code along with
 * the token to set in order to facilitate unit testing.
 * @param token output defining the token to set
 * @param cfg the config to parse (typically from `sasl.oauthbearer.config`)
 * @param now_wallclock_ms the valued to be used for the `iat` claim
 *        (and by implication, the `exp` claim)
 * @returns -1 on failure (\p errstr set), else 0.
 */
static int rd_kafka_oauthbearer_unsecured_token0(
    struct rd_kafka_sasl_oauthbearer_token *token,
    const char *cfg,
    int64_t now_wallclock_ms,
    char *errstr,
    size_t errstr_size) {
        struct rd_kafka_sasl_oauthbearer_parsed_ujws parsed = RD_ZERO_INIT;
        int r;
        int i;

        if (!cfg || !*cfg) {
                rd_snprintf(errstr, errstr_size,
                            "Invalid sasl.oauthbearer.config: "
                            "must not be empty");
                return -1;
        }

        memset(token, 0, sizeof(*token));

        rd_list_init(&parsed.extensions, 0,
                     (void (*)(void *))rd_strtup_destroy);

        if (!(r = parse_ujws_config(cfg, &parsed, errstr, errstr_size))) {
                /* Make sure we have required and valid info */
                if (!parsed.principal_claim_name)
                        parsed.principal_claim_name = rd_strdup("sub");
                if (!parsed.scope_claim_name)
                        parsed.scope_claim_name = rd_strdup("scope");
                if (!parsed.life_seconds)
                        parsed.life_seconds = 3600;
                if (!parsed.principal) {
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid sasl.oauthbearer.config: "
                                    "no principal=<value>");
                        r = -1;
                } else if (strchr(parsed.principal, '"')) {
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid sasl.oauthbearer.config: "
                                    "'\"' cannot appear in principal: %s",
                                    parsed.principal);
                        r = -1;
                } else if (strchr(parsed.principal_claim_name, '"')) {
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid sasl.oauthbearer.config: "
                                    "'\"' cannot appear in "
                                    "principalClaimName: %s",
                                    parsed.principal_claim_name);
                        r = -1;
                } else if (strchr(parsed.scope_claim_name, '"')) {
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid sasl.oauthbearer.config: "
                                    "'\"' cannot appear in scopeClaimName: %s",
                                    parsed.scope_claim_name);
                        r = -1;
                } else if (parsed.scope_csv_text &&
                           strchr(parsed.scope_csv_text, '"')) {
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid sasl.oauthbearer.config: "
                                    "'\"' cannot appear in scope: %s",
                                    parsed.scope_csv_text);
                        r = -1;
                } else {
                        char **extensionv;
                        int extension_pair_count;
                        char *jws = create_jws_compact_serialization(
                            &parsed, now_wallclock_ms * 1000);

                        extension_pair_count = rd_list_cnt(&parsed.extensions);
                        extensionv = rd_malloc(sizeof(*extensionv) * 2 *
                                               extension_pair_count);
                        for (i = 0; i < extension_pair_count; ++i) {
                                rd_strtup_t *strtup =
                                    (rd_strtup_t *)rd_list_elem(
                                        &parsed.extensions, i);
                                extensionv[2 * i] = rd_strdup(strtup->name);
                                extensionv[2 * i + 1] =
                                    rd_strdup(strtup->value);
                        }
                        token->token_value = jws;
                        token->md_lifetime_ms =
                            now_wallclock_ms + parsed.life_seconds * 1000;
                        token->md_principal_name = rd_strdup(parsed.principal);
                        token->extensions        = extensionv;
                        token->extension_size    = 2 * extension_pair_count;
                }
        }
        RD_IF_FREE(parsed.principal_claim_name, rd_free);
        RD_IF_FREE(parsed.principal, rd_free);
        RD_IF_FREE(parsed.scope_claim_name, rd_free);
        RD_IF_FREE(parsed.scope_csv_text, rd_free);
        rd_list_destroy(&parsed.extensions);

        if (r == -1)
                rd_kafka_sasl_oauthbearer_token_free(token);

        return r;
}

/**
 * @brief Default SASL/OAUTHBEARER token refresh callback that generates an
 * unsecured JWS as per https://tools.ietf.org/html/rfc7515#appendix-A.5.
 *
 * This method interprets `sasl.oauthbearer.config` as space-separated
 * name=value pairs with valid names including principalClaimName,
 * principal, scopeClaimName, scope, and lifeSeconds. The default
 * value for principalClaimName is "sub".  The principal must be specified.
 * The default value for scopeClaimName is "scope", and the default value
 * for lifeSeconds is 3600.  The scope value is CSV format with the
 * default value being no/empty scope. For example:
 * "principalClaimName=azp principal=admin scopeClaimName=roles
 * scope=role1,role2 lifeSeconds=600".
 *
 * SASL extensions can be communicated to the broker via
 * extension_NAME=value. For example:
 * "principal=admin extension_traceId=123".  Extension names and values
 * must conform to the required syntax as per
 * https://tools.ietf.org/html/rfc7628#section-3.1
 *
 * All values -- whether extensions, claim names, or scope elements -- must not
 * include a quote (") character.  The parsing rules also imply that names
 * and values cannot include a space character, and scope elements cannot
 * include a comma (,) character.
 *
 * The existence of any kind of parsing problem -- an unrecognized name,
 * a quote character in a value, an empty value, etc. -- raises the
 * \c RD_KAFKA_RESP_ERR__AUTHENTICATION event.
 *
 * Unsecured tokens are not to be used in production -- they are only good for
 * testing and development purposess -- so while the inflexibility of the
 * parsing rules is acknowledged, it is assumed that this is not problematic.
 */
void rd_kafka_oauthbearer_unsecured_token(rd_kafka_t *rk,
                                          const char *oauthbearer_config,
                                          void *opaque) {
        char errstr[512];
        struct rd_kafka_sasl_oauthbearer_token token = RD_ZERO_INIT;

        rd_kafka_dbg(rk, SECURITY, "OAUTHBEARER", "Creating unsecured token");

        if (rd_kafka_oauthbearer_unsecured_token0(&token, oauthbearer_config,
                                                  rd_uclock() / 1000, errstr,
                                                  sizeof(errstr)) == -1 ||
            rd_kafka_oauthbearer_set_token(
                rk, token.token_value, token.md_lifetime_ms,
                token.md_principal_name, (const char **)token.extensions,
                token.extension_size, errstr, sizeof(errstr)) == -1) {
                rd_kafka_oauthbearer_set_token_failure(rk, errstr);
        }

        rd_kafka_sasl_oauthbearer_token_free(&token);
}

/**
 * @brief Close and free authentication state
 */
static void rd_kafka_sasl_oauthbearer_close(rd_kafka_transport_t *rktrans) {
        struct rd_kafka_sasl_oauthbearer_state *state =
            rktrans->rktrans_sasl.state;

        if (!state)
                return;

        RD_IF_FREE(state->server_error_msg, rd_free);
        rd_free(state->token_value);
        rd_free(state->md_principal_name);
        rd_list_destroy(&state->extensions);
        rd_free(state);
        rktrans->rktrans_sasl.state = NULL;
}



/**
 * @brief Build client-first-message
 */
static void rd_kafka_sasl_oauthbearer_build_client_first_message(
    rd_kafka_transport_t *rktrans,
    rd_chariov_t *out) {
        struct rd_kafka_sasl_oauthbearer_state *state =
            rktrans->rktrans_sasl.state;

        /*
         * https://tools.ietf.org/html/rfc7628#section-3.1
         * kvsep          = %x01
         * key            = 1*(ALPHA)
         * value          = *(VCHAR / SP / HTAB / CR / LF )
         * kvpair         = key "=" value kvsep
         * ;;gs2-header     = See RFC 5801
         * client-resp    = (gs2-header kvsep *kvpair kvsep) / kvsep
         */

        static const char *gs2_header = "n,,";
        static const char *kvsep      = "\x01";
        const int kvsep_size          = (int)strlen(kvsep);
        int extension_size            = 0;
        int i;
        char *buf;
        int size_written;
        unsigned long r;

        for (i = 0; i < rd_list_cnt(&state->extensions); i++) {
                rd_strtup_t *extension = rd_list_elem(&state->extensions, i);
                // kvpair         = key "=" value kvsep
                extension_size += (int)strlen(extension->name) + 1  // "="
                                  + (int)strlen(extension->value) + kvsep_size;
        }

        // client-resp    = (gs2-header kvsep *kvpair kvsep) / kvsep
        out->size = strlen(gs2_header) + kvsep_size + strlen("auth=Bearer ") +
                    strlen(state->token_value) + kvsep_size + extension_size +
                    kvsep_size;
        out->ptr = rd_malloc(out->size + 1);

        buf          = out->ptr;
        size_written = 0;
        r            = rd_snprintf(buf, out->size + 1 - size_written,
                        "%s%sauth=Bearer %s%s", gs2_header, kvsep,
                        state->token_value, kvsep);
        rd_assert(r < out->size + 1 - size_written);
        size_written += r;
        buf = out->ptr + size_written;

        for (i = 0; i < rd_list_cnt(&state->extensions); i++) {
                rd_strtup_t *extension = rd_list_elem(&state->extensions, i);
                r = rd_snprintf(buf, out->size + 1 - size_written, "%s=%s%s",
                                extension->name, extension->value, kvsep);
                rd_assert(r < out->size + 1 - size_written);
                size_written += r;
                buf = out->ptr + size_written;
        }

        r = rd_snprintf(buf, out->size + 1 - size_written, "%s", kvsep);
        rd_assert(r < out->size + 1 - size_written);

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "OAUTHBEARER",
                   "Built client first message");
}



/**
 * @brief SASL OAUTHBEARER client state machine
 * @returns -1 on failure (\p errstr set), else 0.
 */
static int rd_kafka_sasl_oauthbearer_fsm(rd_kafka_transport_t *rktrans,
                                         const rd_chariov_t *in,
                                         char *errstr,
                                         size_t errstr_size) {
        static const char *state_names[] = {
            "client-first-message",
            "server-first-message",
            "server-failure-message",
        };
        struct rd_kafka_sasl_oauthbearer_state *state =
            rktrans->rktrans_sasl.state;
        rd_chariov_t out = RD_ZERO_INIT;
        int r            = -1;

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "OAUTHBEARER",
                   "SASL OAUTHBEARER client in state %s",
                   state_names[state->state]);

        switch (state->state) {
        case RD_KAFKA_SASL_OAUTHB_STATE_SEND_CLIENT_FIRST_MESSAGE:
                rd_dassert(!in); /* Not expecting any server-input */

                rd_kafka_sasl_oauthbearer_build_client_first_message(rktrans,
                                                                     &out);
                state->state = RD_KAFKA_SASL_OAUTHB_STATE_RECV_SERVER_FIRST_MSG;
                break;


        case RD_KAFKA_SASL_OAUTHB_STATE_RECV_SERVER_FIRST_MSG:
                if (!in->size || !*in->ptr) {
                        /* Success */
                        rd_rkb_dbg(rktrans->rktrans_rkb,
                                   SECURITY | RD_KAFKA_DBG_BROKER,
                                   "OAUTHBEARER",
                                   "SASL OAUTHBEARER authentication "
                                   "successful (principal=%s)",
                                   state->md_principal_name);
                        rd_kafka_sasl_auth_done(rktrans);
                        r = 0;
                        break;
                }

                /* Failure; save error message for later */
                state->server_error_msg = rd_strndup(in->ptr, in->size);

                /*
                 * https://tools.ietf.org/html/rfc7628#section-3.1
                 * kvsep          = %x01
                 * client-resp    = (gs2-header kvsep *kvpair kvsep) / kvsep
                 *
                 * Send final kvsep (CTRL-A) character
                 */
                out.size = 1;
                out.ptr  = rd_malloc(out.size + 1);
                rd_snprintf(out.ptr, out.size + 1, "\x01");
                state->state =
                    RD_KAFKA_SASL_OAUTHB_STATE_RECV_SERVER_MSG_AFTER_FAIL;
                r = 0;  // Will fail later in next state after sending response
                break;

        case RD_KAFKA_SASL_OAUTHB_STATE_RECV_SERVER_MSG_AFTER_FAIL:
                /* Failure as previosuly communicated by server first message */
                rd_snprintf(errstr, errstr_size,
                            "SASL OAUTHBEARER authentication failed "
                            "(principal=%s): %s",
                            state->md_principal_name, state->server_error_msg);
                rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY | RD_KAFKA_DBG_BROKER,
                           "OAUTHBEARER", "%s", errstr);
                r = -1;
                break;
        }

        if (out.ptr) {
                r = rd_kafka_sasl_send(rktrans, out.ptr, (int)out.size, errstr,
                                       errstr_size);
                rd_free(out.ptr);
        }

        return r;
}


/**
 * @brief Handle received frame from broker.
 */
static int rd_kafka_sasl_oauthbearer_recv(rd_kafka_transport_t *rktrans,
                                          const void *buf,
                                          size_t size,
                                          char *errstr,
                                          size_t errstr_size) {
        const rd_chariov_t in = {.ptr = (char *)buf, .size = size};
        return rd_kafka_sasl_oauthbearer_fsm(rktrans, &in, errstr, errstr_size);
}


/**
 * @brief Initialize and start SASL OAUTHBEARER (builtin) authentication.
 *
 * Returns 0 on successful init and -1 on error.
 *
 * @locality broker thread
 */
static int rd_kafka_sasl_oauthbearer_client_new(rd_kafka_transport_t *rktrans,
                                                const char *hostname,
                                                char *errstr,
                                                size_t errstr_size) {
        rd_kafka_sasl_oauthbearer_handle_t *handle =
            rktrans->rktrans_rkb->rkb_rk->rk_sasl.handle;
        struct rd_kafka_sasl_oauthbearer_state *state;

        state        = rd_calloc(1, sizeof(*state));
        state->state = RD_KAFKA_SASL_OAUTHB_STATE_SEND_CLIENT_FIRST_MESSAGE;

        /*
         * Save off the state structure now, before any possibility of
         * returning, so that we will always free up the allocated memory in
         * rd_kafka_sasl_oauthbearer_close().
         */
        rktrans->rktrans_sasl.state = state;

        /*
         * Make sure we have a consistent view of the token and extensions
         * throughout the authentication process -- even if it is refreshed
         * midway through this particular authentication.
         */
        rwlock_rdlock(&handle->lock);
        if (!handle->token_value) {
                rd_snprintf(errstr, errstr_size,
                            "OAUTHBEARER cannot log in because there "
                            "is no token available; last error: %s",
                            handle->errstr ? handle->errstr
                                           : "(not available)");
                rwlock_rdunlock(&handle->lock);
                return -1;
        }

        state->token_value       = rd_strdup(handle->token_value);
        state->md_principal_name = rd_strdup(handle->md_principal_name);
        rd_list_copy_to(&state->extensions, &handle->extensions,
                        rd_strtup_list_copy, NULL);

        rwlock_rdunlock(&handle->lock);

        /* Kick off the FSM */
        return rd_kafka_sasl_oauthbearer_fsm(rktrans, NULL, errstr,
                                             errstr_size);
}


/**
 * @brief Token refresh timer callback.
 *
 * @locality rdkafka main thread
 */
static void
rd_kafka_sasl_oauthbearer_token_refresh_tmr_cb(rd_kafka_timers_t *rkts,
                                               void *arg) {
        rd_kafka_t *rk                             = arg;
        rd_kafka_sasl_oauthbearer_handle_t *handle = rk->rk_sasl.handle;

        /* Enqueue a token refresh if necessary */
        rd_kafka_oauthbearer_enqueue_token_refresh_if_necessary(handle);
}


/**
 * @brief Per-client-instance initializer
 */
static int rd_kafka_sasl_oauthbearer_init(rd_kafka_t *rk,
                                          char *errstr,
                                          size_t errstr_size) {
        rd_kafka_sasl_oauthbearer_handle_t *handle;

        handle             = rd_calloc(1, sizeof(*handle));
        rk->rk_sasl.handle = handle;

        rwlock_init(&handle->lock);

        handle->rk = rk;

        rd_list_init(&handle->extensions, 0,
                     (void (*)(void *))rd_strtup_destroy);


        if (rk->rk_conf.sasl.enable_callback_queue) {
                /* SASL specific callback queue enabled */
                rk->rk_sasl.callback_q = rd_kafka_q_new(rk);
                handle->callback_q = rd_kafka_q_keep(rk->rk_sasl.callback_q);
        } else {
                /* Use main queue */
                handle->callback_q = rd_kafka_q_keep(rk->rk_rep);
        }

        rd_kafka_timer_start(
            &rk->rk_timers, &handle->token_refresh_tmr, 1 * 1000 * 1000,
            rd_kafka_sasl_oauthbearer_token_refresh_tmr_cb, rk);

        /* Automatically refresh the token if using the builtin
         * unsecure JWS token refresher, to avoid an initial connection
         * stall as we wait for the application to call poll(). */
        if (rk->rk_conf.sasl.oauthbearer.token_refresh_cb ==
            rd_kafka_oauthbearer_unsecured_token) {
                rk->rk_conf.sasl.oauthbearer.token_refresh_cb(
                    rk, rk->rk_conf.sasl.oauthbearer_config,
                    rk->rk_conf.opaque);

                return 0;
        }


#if WITH_OAUTHBEARER_OIDC
        if (rk->rk_conf.sasl.oauthbearer.method ==
                RD_KAFKA_SASL_OAUTHBEARER_METHOD_OIDC &&
            rk->rk_conf.sasl.oauthbearer.token_refresh_cb ==
                rd_kafka_oidc_token_refresh_cb) {
                handle->internal_refresh = rd_true;
                rd_kafka_sasl_background_callbacks_enable(rk);
        }
#endif

        /* Otherwise enqueue a refresh callback for the application. */
        rd_kafka_oauthbearer_enqueue_token_refresh(handle);

        return 0;
}


/**
 * @brief Per-client-instance destructor
 */
static void rd_kafka_sasl_oauthbearer_term(rd_kafka_t *rk) {
        rd_kafka_sasl_oauthbearer_handle_t *handle = rk->rk_sasl.handle;

        if (!handle)
                return;

        rk->rk_sasl.handle = NULL;

        rd_kafka_timer_stop(&rk->rk_timers, &handle->token_refresh_tmr, 1);

        RD_IF_FREE(handle->md_principal_name, rd_free);
        RD_IF_FREE(handle->token_value, rd_free);
        rd_list_destroy(&handle->extensions);
        RD_IF_FREE(handle->errstr, rd_free);
        RD_IF_FREE(handle->callback_q, rd_kafka_q_destroy);

        rwlock_destroy(&handle->lock);

        rd_free(handle);
}


/**
 * @brief SASL/OAUTHBEARER is unable to connect unless a valid
 *        token is available, and a valid token CANNOT be
 *        available unless/until an initial token retrieval
 *        succeeds, so wait for this precondition if necessary.
 */
static rd_bool_t rd_kafka_sasl_oauthbearer_ready(rd_kafka_t *rk) {
        rd_kafka_sasl_oauthbearer_handle_t *handle = rk->rk_sasl.handle;

        if (!handle)
                return rd_false;

        return rd_kafka_oauthbearer_has_token(handle);
}


/**
 * @brief Validate OAUTHBEARER config, which is a no-op
 * (we rely on initial token retrieval)
 */
static int rd_kafka_sasl_oauthbearer_conf_validate(rd_kafka_t *rk,
                                                   char *errstr,
                                                   size_t errstr_size) {
        /*
         * We must rely on the initial token retrieval as a proxy
         * for configuration validation because the configuration is
         * implementation-dependent, and it is not necessarily the case
         * that the config reflects the default unsecured JWS config
         * that we know how to parse.
         */
        return 0;
}



const struct rd_kafka_sasl_provider rd_kafka_sasl_oauthbearer_provider = {
    .name          = "OAUTHBEARER (builtin)",
    .init          = rd_kafka_sasl_oauthbearer_init,
    .term          = rd_kafka_sasl_oauthbearer_term,
    .ready         = rd_kafka_sasl_oauthbearer_ready,
    .client_new    = rd_kafka_sasl_oauthbearer_client_new,
    .recv          = rd_kafka_sasl_oauthbearer_recv,
    .close         = rd_kafka_sasl_oauthbearer_close,
    .conf_validate = rd_kafka_sasl_oauthbearer_conf_validate,
};



/**
 * @name Unit tests
 *
 *
 */

/**
 * @brief `sasl.oauthbearer.config` test:
 * should generate correct default values.
 */
static int do_unittest_config_defaults(void) {
        static const char *sasl_oauthbearer_config =
            "principal=fubar "
            "scopeClaimName=whatever";
        // default scope is empty, default lifetime is 3600 seconds
        // {"alg":"none"}
        // .
        // {"sub":"fubar","iat":1.000,"exp":3601.000}
        //
        static const char *expected_token_value =
            "eyJhbGciOiJub25lIn0"
            "."
            "eyJzdWIiOiJmdWJhciIsImlhdCI6MS4wMDAsImV4cCI6MzYwMS4wMDB9"
            ".";
        rd_ts_t now_wallclock_ms = 1000;
        char errstr[512];
        struct rd_kafka_sasl_oauthbearer_token token;
        int r;

        r = rd_kafka_oauthbearer_unsecured_token0(
            &token, sasl_oauthbearer_config, now_wallclock_ms, errstr,
            sizeof(errstr));
        if (r == -1)
                RD_UT_FAIL("Failed to create a token: %s: %s",
                           sasl_oauthbearer_config, errstr);

        RD_UT_ASSERT(token.md_lifetime_ms == now_wallclock_ms + 3600 * 1000,
                     "Invalid md_lifetime_ms %" PRId64, token.md_lifetime_ms);
        RD_UT_ASSERT(!strcmp(token.md_principal_name, "fubar"),
                     "Invalid md_principal_name %s", token.md_principal_name);
        RD_UT_ASSERT(!strcmp(token.token_value, expected_token_value),
                     "Invalid token_value %s, expected %s", token.token_value,
                     expected_token_value);

        rd_kafka_sasl_oauthbearer_token_free(&token);

        RD_UT_PASS();
}

/**
 * @brief `sasl.oauthbearer.config` test:
 * should generate correct token for explicit scope and lifeSeconds values.
 */
static int do_unittest_config_explicit_scope_and_life(void) {
        static const char *sasl_oauthbearer_config =
            "principal=fubar "
            "scope=role1,role2 lifeSeconds=60";
        // {"alg":"none"}
        // .
        // {"sub":"fubar","iat":1.000,"exp":61.000,"scope":["role1","role2"]}
        //
        static const char *expected_token_value =
            "eyJhbGciOiJub25lIn0"
            "."
            "eyJzdWIiOiJmdWJhciIsImlhdCI6MS4wMDAsImV4cCI6NjEuMDAwLCJzY29wZ"
            "SI6WyJyb2xlMSIsInJvbGUyIl19"
            ".";
        rd_ts_t now_wallclock_ms = 1000;
        char errstr[512];
        struct rd_kafka_sasl_oauthbearer_token token;
        int r;

        r = rd_kafka_oauthbearer_unsecured_token0(
            &token, sasl_oauthbearer_config, now_wallclock_ms, errstr,
            sizeof(errstr));
        if (r == -1)
                RD_UT_FAIL("Failed to create a token: %s: %s",
                           sasl_oauthbearer_config, errstr);

        RD_UT_ASSERT(token.md_lifetime_ms == now_wallclock_ms + 60 * 1000,
                     "Invalid md_lifetime_ms %" PRId64, token.md_lifetime_ms);
        RD_UT_ASSERT(!strcmp(token.md_principal_name, "fubar"),
                     "Invalid md_principal_name %s", token.md_principal_name);
        RD_UT_ASSERT(!strcmp(token.token_value, expected_token_value),
                     "Invalid token_value %s, expected %s", token.token_value,
                     expected_token_value);

        rd_kafka_sasl_oauthbearer_token_free(&token);

        RD_UT_PASS();
}

/**
 * @brief `sasl.oauthbearer.config` test:
 * should generate correct token when all values are provided explicitly.
 */
static int do_unittest_config_all_explicit_values(void) {
        static const char *sasl_oauthbearer_config =
            "principal=fubar "
            "principalClaimName=azp scope=role1,role2 "
            "scopeClaimName=roles lifeSeconds=60";
        // {"alg":"none"}
        // .
        // {"azp":"fubar","iat":1.000,"exp":61.000,"roles":["role1","role2"]}
        //
        static const char *expected_token_value =
            "eyJhbGciOiJub25lIn0"
            "."
            "eyJhenAiOiJmdWJhciIsImlhdCI6MS4wMDAsImV4cCI6NjEuMDAwLCJyb2xlc"
            "yI6WyJyb2xlMSIsInJvbGUyIl19"
            ".";
        rd_ts_t now_wallclock_ms = 1000;
        char errstr[512];
        struct rd_kafka_sasl_oauthbearer_token token;
        int r;

        r = rd_kafka_oauthbearer_unsecured_token0(
            &token, sasl_oauthbearer_config, now_wallclock_ms, errstr,
            sizeof(errstr));
        if (r == -1)
                RD_UT_FAIL("Failed to create a token: %s: %s",
                           sasl_oauthbearer_config, errstr);

        RD_UT_ASSERT(token.md_lifetime_ms == now_wallclock_ms + 60 * 1000,
                     "Invalid md_lifetime_ms %" PRId64, token.md_lifetime_ms);
        RD_UT_ASSERT(!strcmp(token.md_principal_name, "fubar"),
                     "Invalid md_principal_name %s", token.md_principal_name);
        RD_UT_ASSERT(!strcmp(token.token_value, expected_token_value),
                     "Invalid token_value %s, expected %s", token.token_value,
                     expected_token_value);

        rd_kafka_sasl_oauthbearer_token_free(&token);

        RD_UT_PASS();
}

/**
 * @brief `sasl.oauthbearer.config` test:
 * should fail when no principal specified.
 */
static int do_unittest_config_no_principal_should_fail(void) {
        static const char *expected_msg =
            "Invalid sasl.oauthbearer.config: "
            "no principal=<value>";
        static const char *sasl_oauthbearer_config =
            "extension_notaprincipal=hi";
        rd_ts_t now_wallclock_ms = 1000;
        char errstr[512];
        struct rd_kafka_sasl_oauthbearer_token token = RD_ZERO_INIT;
        int r;

        r = rd_kafka_oauthbearer_unsecured_token0(
            &token, sasl_oauthbearer_config, now_wallclock_ms, errstr,
            sizeof(errstr));
        if (r != -1)
                rd_kafka_sasl_oauthbearer_token_free(&token);

        RD_UT_ASSERT(r == -1, "Did not fail despite missing principal");

        RD_UT_ASSERT(!strcmp(errstr, expected_msg),
                     "Incorrect error message when no principal: "
                     "expected=%s received=%s",
                     expected_msg, errstr);
        RD_UT_PASS();
}

/**
 * @brief `sasl.oauthbearer.config` test:
 * should fail when no sasl.oauthbearer.config is specified.
 */
static int do_unittest_config_empty_should_fail(void) {
        static const char *expected_msg =
            "Invalid sasl.oauthbearer.config: "
            "must not be empty";
        static const char *sasl_oauthbearer_config = "";
        rd_ts_t now_wallclock_ms                   = 1000;
        char errstr[512];
        struct rd_kafka_sasl_oauthbearer_token token = RD_ZERO_INIT;
        int r;

        r = rd_kafka_oauthbearer_unsecured_token0(
            &token, sasl_oauthbearer_config, now_wallclock_ms, errstr,
            sizeof(errstr));
        if (r != -1)
                rd_kafka_sasl_oauthbearer_token_free(&token);

        RD_UT_ASSERT(r == -1, "Did not fail despite empty config");

        RD_UT_ASSERT(!strcmp(errstr, expected_msg),
                     "Incorrect error message with empty config: "
                     "expected=%s received=%s",
                     expected_msg, errstr);
        RD_UT_PASS();
}

/**
 * @brief `sasl.oauthbearer.config` test:
 * should fail when something unrecognized is specified.
 */
static int do_unittest_config_unrecognized_should_fail(void) {
        static const char *expected_msg =
            "Unrecognized "
            "sasl.oauthbearer.config beginning at: unrecognized";
        static const char *sasl_oauthbearer_config =
            "principal=fubar unrecognized";
        rd_ts_t now_wallclock_ms = 1000;
        char errstr[512];
        struct rd_kafka_sasl_oauthbearer_token token;
        int r;

        r = rd_kafka_oauthbearer_unsecured_token0(
            &token, sasl_oauthbearer_config, now_wallclock_ms, errstr,
            sizeof(errstr));
        if (r != -1)
                rd_kafka_sasl_oauthbearer_token_free(&token);

        RD_UT_ASSERT(r == -1, "Did not fail with something unrecognized");

        RD_UT_ASSERT(!strcmp(errstr, expected_msg),
                     "Incorrect error message with something unrecognized: "
                     "expected=%s received=%s",
                     expected_msg, errstr);
        RD_UT_PASS();
}

/**
 * @brief `sasl.oauthbearer.config` test:
 * should fail when empty values are specified.
 */
static int do_unittest_config_empty_value_should_fail(void) {
        static const char *sasl_oauthbearer_configs[] = {
            "principal=", "principal=fubar principalClaimName=",
            "principal=fubar scope=", "principal=fubar scopeClaimName=",
            "principal=fubar lifeSeconds="};
        static const char *expected_prefix =
            "Invalid sasl.oauthbearer.config: empty";
        size_t i;
        rd_ts_t now_wallclock_ms = 1000;
        char errstr[512];
        int r;

        for (i = 0; i < sizeof(sasl_oauthbearer_configs) / sizeof(const char *);
             i++) {
                struct rd_kafka_sasl_oauthbearer_token token;
                r = rd_kafka_oauthbearer_unsecured_token0(
                    &token, sasl_oauthbearer_configs[i], now_wallclock_ms,
                    errstr, sizeof(errstr));
                if (r != -1)
                        rd_kafka_sasl_oauthbearer_token_free(&token);

                RD_UT_ASSERT(r == -1, "Did not fail with an empty value: %s",
                             sasl_oauthbearer_configs[i]);

                RD_UT_ASSERT(
                    !strncmp(expected_prefix, errstr, strlen(expected_prefix)),
                    "Incorrect error message prefix when empty "
                    "(%s): expected=%s received=%s",
                    sasl_oauthbearer_configs[i], expected_prefix, errstr);
        }
        RD_UT_PASS();
}

/**
 * @brief `sasl.oauthbearer.config` test:
 * should fail when value with embedded quote is specified.
 */
static int do_unittest_config_value_with_quote_should_fail(void) {
        static const char *sasl_oauthbearer_configs[] = {
            "principal=\"fu", "principal=fubar principalClaimName=\"bar",
            "principal=fubar scope=\"a,b,c",
            "principal=fubar scopeClaimName=\"baz"};
        static const char *expected_prefix =
            "Invalid "
            "sasl.oauthbearer.config: '\"' cannot appear in ";
        size_t i;
        rd_ts_t now_wallclock_ms = 1000;
        char errstr[512];
        int r;

        for (i = 0; i < sizeof(sasl_oauthbearer_configs) / sizeof(const char *);
             i++) {
                struct rd_kafka_sasl_oauthbearer_token token;
                r = rd_kafka_oauthbearer_unsecured_token0(
                    &token, sasl_oauthbearer_configs[i], now_wallclock_ms,
                    errstr, sizeof(errstr));
                if (r != -1)
                        rd_kafka_sasl_oauthbearer_token_free(&token);

                RD_UT_ASSERT(r == -1, "Did not fail with embedded quote: %s",
                             sasl_oauthbearer_configs[i]);

                RD_UT_ASSERT(
                    !strncmp(expected_prefix, errstr, strlen(expected_prefix)),
                    "Incorrect error message prefix with "
                    "embedded quote (%s): expected=%s received=%s",
                    sasl_oauthbearer_configs[i], expected_prefix, errstr);
        }
        RD_UT_PASS();
}

/**
 * @brief `sasl.oauthbearer.config` test:
 * should generate correct extensions.
 */
static int do_unittest_config_extensions(void) {
        static const char *sasl_oauthbearer_config =
            "principal=fubar "
            "extension_a=b extension_yz=yzval";
        rd_ts_t now_wallclock_ms = 1000;
        char errstr[512];
        struct rd_kafka_sasl_oauthbearer_token token;
        int r;

        r = rd_kafka_oauthbearer_unsecured_token0(
            &token, sasl_oauthbearer_config, now_wallclock_ms, errstr,
            sizeof(errstr));

        if (r == -1)
                RD_UT_FAIL("Failed to create a token: %s: %s",
                           sasl_oauthbearer_config, errstr);

        RD_UT_ASSERT(token.extension_size == 4,
                     "Incorrect extensions: expected 4, received %" PRIusz,
                     token.extension_size);

        RD_UT_ASSERT(!strcmp(token.extensions[0], "a") &&
                         !strcmp(token.extensions[1], "b") &&
                         !strcmp(token.extensions[2], "yz") &&
                         !strcmp(token.extensions[3], "yzval"),
                     "Incorrect extensions: expected a=b and "
                     "yz=yzval but received %s=%s and %s=%s",
                     token.extensions[0], token.extensions[1],
                     token.extensions[2], token.extensions[3]);

        rd_kafka_sasl_oauthbearer_token_free(&token);

        RD_UT_PASS();
}

/**
 * @brief make sure illegal extensions keys are rejected
 */
static int do_unittest_illegal_extension_keys_should_fail(void) {
        static const char *illegal_keys[] = {"", "auth", "a1", " a"};
        size_t i;
        char errstr[512];
        int r;

        for (i = 0; i < sizeof(illegal_keys) / sizeof(const char *); i++) {
                r = check_oauthbearer_extension_key(illegal_keys[i], errstr,
                                                    sizeof(errstr));
                RD_UT_ASSERT(r == -1,
                             "Did not recognize illegal extension key: %s",
                             illegal_keys[i]);
        }
        RD_UT_PASS();
}

/**
 * @brief make sure illegal extensions keys are rejected
 */
static int do_unittest_odd_extension_size_should_fail(void) {
        static const char *expected_errstr =
            "Incorrect extension size "
            "(must be a non-negative multiple of 2): 1";
        char errstr[512];
        rd_kafka_resp_err_t err;
        rd_kafka_t rk                             = RD_ZERO_INIT;
        rd_kafka_sasl_oauthbearer_handle_t handle = RD_ZERO_INIT;

        rk.rk_conf.sasl.provider = &rd_kafka_sasl_oauthbearer_provider;
        rk.rk_sasl.handle        = &handle;

        rwlock_init(&handle.lock);

        err = rd_kafka_oauthbearer_set_token0(&rk, "abcd", 1000, "fubar", NULL,
                                              1, errstr, sizeof(errstr));

        rwlock_destroy(&handle.lock);

        RD_UT_ASSERT(err, "Did not recognize illegal extension size");
        RD_UT_ASSERT(!strcmp(errstr, expected_errstr),
                     "Incorrect error message for illegal "
                     "extension size: expected=%s; received=%s",
                     expected_errstr, errstr);
        RD_UT_ASSERT(err == RD_KAFKA_RESP_ERR__INVALID_ARG,
                     "Expected ErrInvalidArg, not %s", rd_kafka_err2name(err));

        RD_UT_PASS();
}

int unittest_sasl_oauthbearer(void) {
        int fails = 0;

        fails += do_unittest_config_no_principal_should_fail();
        fails += do_unittest_config_empty_should_fail();
        fails += do_unittest_config_empty_value_should_fail();
        fails += do_unittest_config_value_with_quote_should_fail();
        fails += do_unittest_config_unrecognized_should_fail();
        fails += do_unittest_config_defaults();
        fails += do_unittest_config_explicit_scope_and_life();
        fails += do_unittest_config_all_explicit_values();
        fails += do_unittest_config_extensions();
        fails += do_unittest_illegal_extension_keys_should_fail();
        fails += do_unittest_odd_extension_size_should_fail();

        return fails;
}
