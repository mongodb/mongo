/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "tls/s2n_client_hello.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/param.h>
#include <time.h>

#include "api/unstable/fingerprint.h"
#include "crypto/s2n_fips.h"
#include "crypto/s2n_hash.h"
#include "crypto/s2n_rsa_signing.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/extensions/s2n_client_server_name.h"
#include "tls/extensions/s2n_client_supported_groups.h"
#include "tls/extensions/s2n_extension_list.h"
#include "tls/extensions/s2n_server_key_share.h"
#include "tls/s2n_alerts.h"
#include "tls/s2n_auth_selection.h"
#include "tls/s2n_cipher_preferences.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_handshake_type.h"
#include "tls/s2n_security_policies.h"
#include "tls/s2n_signature_algorithms.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_bitmap.h"
#include "utils/s2n_random.h"
#include "utils/s2n_safety.h"

struct s2n_client_hello *s2n_connection_get_client_hello(struct s2n_connection *conn)
{
    if (conn->client_hello.parsed != 1) {
        return NULL;
    }

    return &conn->client_hello;
}

static uint32_t min_size(struct s2n_blob *blob, uint32_t max_length)
{
    return blob->size < max_length ? blob->size : max_length;
}

static S2N_RESULT s2n_generate_client_session_id(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->config);

    /* Session id already generated - no-op */
    if (conn->session_id_len) {
        return S2N_RESULT_OK;
    }

    /* Only generate the session id if using tickets */
    bool generate = conn->config->use_tickets;

    /* TLS1.3 doesn't require session ids. The field is actually renamed to legacy_session_id.
     * However, we still set a session id if dealing with troublesome middleboxes
     * (middlebox compatibility mode) or if trying to use a TLS1.2 ticket.
     */
    if (conn->client_protocol_version >= S2N_TLS13) {
        generate = s2n_is_middlebox_compat_enabled(conn) || conn->resume_protocol_version;
    }

    /* Session id not needed - no-op */
    if (!generate) {
        return S2N_RESULT_OK;
    }

    /* QUIC should not allow session ids for any reason.
     *
     *= https://www.rfc-editor.org/rfc/rfc9001#section-8.4
     *# A server SHOULD treat the receipt of a TLS ClientHello with a non-empty
     *# legacy_session_id field as a connection error of type PROTOCOL_VIOLATION.
     */
    RESULT_ENSURE(!conn->quic_enabled, S2N_ERR_UNSUPPORTED_WITH_QUIC);

    struct s2n_blob session_id = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&session_id, conn->session_id, S2N_TLS_SESSION_ID_MAX_LEN));
    RESULT_GUARD(s2n_get_public_random_data(&session_id));
    conn->session_id_len = S2N_TLS_SESSION_ID_MAX_LEN;
    return S2N_RESULT_OK;
}

ssize_t s2n_client_hello_get_raw_message_length(struct s2n_client_hello *ch)
{
    POSIX_ENSURE_REF(ch);

    return ch->raw_message.size;
}

ssize_t s2n_client_hello_get_raw_message(struct s2n_client_hello *ch, uint8_t *out, uint32_t max_length)
{
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(out);

    uint32_t len = min_size(&ch->raw_message, max_length);
    POSIX_CHECKED_MEMCPY(out, ch->raw_message.data, len);
    return len;
}

ssize_t s2n_client_hello_get_cipher_suites_length(struct s2n_client_hello *ch)
{
    POSIX_ENSURE_REF(ch);

    return ch->cipher_suites.size;
}

int s2n_client_hello_cb_done(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->config);
    POSIX_ENSURE(conn->config->client_hello_cb_mode == S2N_CLIENT_HELLO_CB_NONBLOCKING, S2N_ERR_INVALID_STATE);
    POSIX_ENSURE(conn->client_hello.callback_invoked == 1, S2N_ERR_ASYNC_NOT_PERFORMED);
    POSIX_ENSURE(conn->client_hello.parsed == 1, S2N_ERR_INVALID_STATE);

    conn->client_hello.callback_async_blocked = 0;
    conn->client_hello.callback_async_done = 1;

    return S2N_SUCCESS;
}

ssize_t s2n_client_hello_get_cipher_suites(struct s2n_client_hello *ch, uint8_t *out, uint32_t max_length)
{
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(out);
    POSIX_ENSURE_REF(ch->cipher_suites.data);

    uint32_t len = min_size(&ch->cipher_suites, max_length);

    POSIX_CHECKED_MEMCPY(out, ch->cipher_suites.data, len);

    return len;
}

ssize_t s2n_client_hello_get_extensions_length(struct s2n_client_hello *ch)
{
    POSIX_ENSURE_REF(ch);

    return ch->extensions.raw.size;
}

ssize_t s2n_client_hello_get_extensions(struct s2n_client_hello *ch, uint8_t *out, uint32_t max_length)
{
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(out);
    POSIX_ENSURE_REF(ch->extensions.raw.data);

    uint32_t len = min_size(&ch->extensions.raw, max_length);

    POSIX_CHECKED_MEMCPY(out, ch->extensions.raw.data, len);

    return len;
}

int s2n_client_hello_free_raw_message(struct s2n_client_hello *client_hello)
{
    POSIX_ENSURE_REF(client_hello);

    POSIX_GUARD(s2n_free(&client_hello->raw_message));

    /* These point to data in the raw_message stuffer,
       so we don't need to free them */
    client_hello->cipher_suites.data = NULL;
    client_hello->extensions.raw.data = NULL;

    return 0;
}

int s2n_client_hello_free(struct s2n_client_hello **ch)
{
    POSIX_ENSURE_REF(ch);
    if (*ch == NULL) {
        return S2N_SUCCESS;
    }

    POSIX_ENSURE((*ch)->alloced, S2N_ERR_INVALID_ARGUMENT);
    POSIX_GUARD(s2n_client_hello_free_raw_message(*ch));
    POSIX_GUARD(s2n_free_object((uint8_t **) ch, sizeof(struct s2n_client_hello)));
    *ch = NULL;
    return S2N_SUCCESS;
}

int s2n_collect_client_hello(struct s2n_client_hello *ch, struct s2n_stuffer *source)
{
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(source);

    uint32_t size = s2n_stuffer_data_available(source);
    S2N_ERROR_IF(size == 0, S2N_ERR_BAD_MESSAGE);

    POSIX_GUARD(s2n_realloc(&ch->raw_message, size));
    POSIX_GUARD(s2n_stuffer_read(source, &ch->raw_message));

    return 0;
}

static S2N_RESULT s2n_client_hello_verify_for_retry(struct s2n_connection *conn,
        struct s2n_client_hello *old_ch, struct s2n_client_hello *new_ch,
        uint8_t *previous_client_random)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(old_ch);
    RESULT_ENSURE_REF(new_ch);
    RESULT_ENSURE_REF(previous_client_random);

    if (!s2n_is_hello_retry_handshake(conn)) {
        return S2N_RESULT_OK;
    }

    /*
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.1.2
     *# The client will also send a
     *# ClientHello when the server has responded to its ClientHello with a
     *# HelloRetryRequest.  In that case, the client MUST send the same
     *# ClientHello without modification, except as follows:
     *
     * All of the exceptions that follow are extensions.
     */
    RESULT_ENSURE(old_ch->legacy_version == new_ch->legacy_version, S2N_ERR_BAD_MESSAGE);
    RESULT_ENSURE(old_ch->compression_methods.size == new_ch->compression_methods.size, S2N_ERR_BAD_MESSAGE);
    RESULT_ENSURE(s2n_constant_time_equals(old_ch->compression_methods.data, new_ch->compression_methods.data,
                          new_ch->compression_methods.size),
            S2N_ERR_BAD_MESSAGE);

    /* Some clients are not compliant with TLS 1.3 RFC, and send mismatching values in their second
     * ClientHello. For increased compatibility, these checks are skipped outside of tests. The
     * checks are still included in tests to ensure the s2n-tls client remains compliant.
     */
    if (s2n_in_test()) {
        /* In the past, the s2n-tls client updated the client random in the second ClientHello
         * which is not allowed by RFC8446: https://github.com/aws/s2n-tls/pull/3311. Although the
         * issue was addressed, its existence means that old versions of the s2n-tls client will
         * fail this validation.
         */
        RESULT_ENSURE(s2n_constant_time_equals(
                              previous_client_random,
                              conn->handshake_params.client_random,
                              S2N_TLS_RANDOM_DATA_LEN),
                S2N_ERR_BAD_MESSAGE);

        /* Some clients have been found to send a mismatching legacy session ID. */
        RESULT_ENSURE(old_ch->session_id.size == new_ch->session_id.size, S2N_ERR_BAD_MESSAGE);
        RESULT_ENSURE(s2n_constant_time_equals(old_ch->session_id.data, new_ch->session_id.data,
                              new_ch->session_id.size),
                S2N_ERR_BAD_MESSAGE);

        /* Some clients have been found to send a mismatching cipher suite list. */
        RESULT_ENSURE(old_ch->cipher_suites.size == new_ch->cipher_suites.size, S2N_ERR_BAD_MESSAGE);
        RESULT_ENSURE(s2n_constant_time_equals(old_ch->cipher_suites.data, new_ch->cipher_suites.data,
                              new_ch->cipher_suites.size),
                S2N_ERR_BAD_MESSAGE);
    }

    /*
     * Now enforce that the extensions also exactly match,
     * except for the exceptions described in the RFC.
     */
    for (size_t i = 0; i < s2n_array_len(s2n_supported_extensions); i++) {
        s2n_parsed_extension *old_extension = &old_ch->extensions.parsed_extensions[i];
        uint32_t old_size = old_extension->extension.size;
        s2n_parsed_extension *new_extension = &new_ch->extensions.parsed_extensions[i];
        uint32_t new_size = new_extension->extension.size;

        /* The extension type is only set if the extension is present.
         * Look for a non-zero-length extension.
         */
        uint16_t extension_type = 0;
        if (old_size != 0) {
            extension_type = old_extension->extension_type;
        } else if (new_size != 0) {
            extension_type = new_extension->extension_type;
        } else {
            continue;
        }

        switch (extension_type) {
            /*
             *= https://www.rfc-editor.org/rfc/rfc8446#section-4.1.2
             *#    -  If a "key_share" extension was supplied in the HelloRetryRequest,
             *#       replacing the list of shares with a list containing a single
             *#       KeyShareEntry from the indicated group.
             */
            case TLS_EXTENSION_KEY_SHARE:
                /* Handled when parsing the key share extension */
                break;
            /*
             *= https://www.rfc-editor.org/rfc/rfc8446#section-4.1.2
             *#    -  Removing the "early_data" extension (Section 4.2.10) if one was
             *#       present.  Early data is not permitted after a HelloRetryRequest.
             */
            case TLS_EXTENSION_EARLY_DATA:
                RESULT_ENSURE(new_size == 0, S2N_ERR_BAD_MESSAGE);
                break;
            /*
             *= https://www.rfc-editor.org/rfc/rfc8446#section-4.1.2
             *#    -  Including a "cookie" extension if one was provided in the
             *#       HelloRetryRequest.
             */
            case TLS_EXTENSION_COOKIE:
                /* Handled when parsing the cookie extension */
                break;
            /*
             *= https://www.rfc-editor.org/rfc/rfc8446#section-4.1.2
             *#    -  Updating the "pre_shared_key" extension if present by recomputing
             *#       the "obfuscated_ticket_age" and binder values and (optionally)
             *#       removing any PSKs which are incompatible with the server's
             *#       indicated cipher suite.
             */
            case TLS_EXTENSION_PRE_SHARED_KEY:
                /* Handled when parsing the psk extension */
                break;

            /* Some clients have been found to send mismatching supported versions in their second
             * ClientHello. The extension isn't compared byte-for-byte for increased compatibility
             * with these clients.
             */
            case TLS_EXTENSION_SUPPORTED_VERSIONS:
                /* Additional HRR validation for the supported versions extension is performed when
                 * parsing the extension.
                 */
                break;

            /*
             * No more exceptions.
             * All other extensions must match.
             */
            default:
                RESULT_ENSURE(old_size == new_size, S2N_ERR_BAD_MESSAGE);
                RESULT_ENSURE(s2n_constant_time_equals(
                                      new_extension->extension.data,
                                      old_extension->extension.data,
                                      old_size),
                        S2N_ERR_BAD_MESSAGE);
        }
    }

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_client_hello_parse_raw(struct s2n_client_hello *client_hello,
        uint8_t client_protocol_version[S2N_TLS_PROTOCOL_VERSION_LEN],
        uint8_t client_random[S2N_TLS_RANDOM_DATA_LEN])
{
    RESULT_ENSURE_REF(client_hello);

    struct s2n_stuffer in_stuffer = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init_written(&in_stuffer, &client_hello->raw_message));
    struct s2n_stuffer *in = &in_stuffer;

    /**
     * https://tools.ietf.org/rfc/rfc8446#4.1.2
     * Structure of this message:
     *
     *    uint16 ProtocolVersion;
     *    opaque Random[32];
     *
     *    uint8 CipherSuite[2];
     *
     *    struct {
     *        ProtocolVersion legacy_version = 0x0303;
     *        Random random;
     *        opaque legacy_session_id<0..32>;
     *        CipherSuite cipher_suites<2..2^16-2>;
     *        opaque legacy_compression_methods<1..2^8-1>;
     *        Extension extensions<8..2^16-1>;
     *    } ClientHello;
     **/

    /* legacy_version */
    RESULT_GUARD_POSIX(s2n_stuffer_read_bytes(in, client_protocol_version, S2N_TLS_PROTOCOL_VERSION_LEN));

    /* Encode the version as a 1 byte representation of the two protocol version bytes, with the
     * major version in the tens place and the minor version in the ones place. For example, the
     * TLS 1.2 protocol version is 0x0303, which is encoded as S2N_TLS12 (33).
     */
    client_hello->legacy_version = (client_protocol_version[0] * 10) + client_protocol_version[1];

    /* random */
    RESULT_GUARD_POSIX(s2n_stuffer_erase_and_read_bytes(in, client_random, S2N_TLS_RANDOM_DATA_LEN));

    /* legacy_session_id */
    uint8_t session_id_len = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(in, &session_id_len));
    RESULT_ENSURE(session_id_len <= S2N_TLS_SESSION_ID_MAX_LEN, S2N_ERR_BAD_MESSAGE);
    uint8_t *session_id = s2n_stuffer_raw_read(in, session_id_len);
    RESULT_ENSURE(session_id != NULL, S2N_ERR_BAD_MESSAGE);
    RESULT_GUARD_POSIX(s2n_blob_init(&client_hello->session_id, session_id, session_id_len));

    /* cipher suites */
    uint16_t cipher_suites_length = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(in, &cipher_suites_length));
    RESULT_ENSURE(cipher_suites_length > 0, S2N_ERR_BAD_MESSAGE);
    RESULT_ENSURE(cipher_suites_length % S2N_TLS_CIPHER_SUITE_LEN == 0, S2N_ERR_BAD_MESSAGE);
    uint8_t *cipher_suites = s2n_stuffer_raw_read(in, cipher_suites_length);
    RESULT_ENSURE(cipher_suites != NULL, S2N_ERR_BAD_MESSAGE);
    RESULT_GUARD_POSIX(s2n_blob_init(&client_hello->cipher_suites, cipher_suites, cipher_suites_length));

    /* legacy_compression_methods */
    uint8_t compression_methods_len = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(in, &compression_methods_len));
    uint8_t *compression_methods = s2n_stuffer_raw_read(in, compression_methods_len);
    RESULT_ENSURE(compression_methods != NULL, S2N_ERR_BAD_MESSAGE);
    RESULT_GUARD_POSIX(s2n_blob_init(&client_hello->compression_methods, compression_methods, compression_methods_len));

    /* extensions */
    RESULT_GUARD_POSIX(s2n_extension_list_parse(in, &client_hello->extensions));

    return S2N_RESULT_OK;
}

int s2n_parse_client_hello(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    /* If a retry, move the old version of the client hello
     * somewhere safe so we can compare it to the new client hello later.
     */
    DEFER_CLEANUP(struct s2n_client_hello previous_hello_retry = conn->client_hello,
            s2n_client_hello_free_raw_message);
    if (s2n_is_hello_retry_handshake(conn)) {
        POSIX_CHECKED_MEMSET(&conn->client_hello, 0, sizeof(struct s2n_client_hello));
    }

    POSIX_GUARD(s2n_collect_client_hello(&conn->client_hello, &conn->handshake.io));

    /* The ClientHello version must be TLS12 after a HelloRetryRequest */
    if (s2n_is_hello_retry_handshake(conn)) {
        POSIX_ENSURE_EQ(conn->client_hello_version, S2N_TLS12);
    }

    if (conn->client_hello_version == S2N_SSLv2) {
        POSIX_GUARD(s2n_sslv2_client_hello_recv(conn));
        return S2N_SUCCESS;
    }

    /* Save the current client_random for comparison in the case of a retry */
    uint8_t previous_client_random[S2N_TLS_RANDOM_DATA_LEN] = { 0 };
    POSIX_CHECKED_MEMCPY(previous_client_random, conn->handshake_params.client_random,
            S2N_TLS_RANDOM_DATA_LEN);

    /* Parse raw, collected client hello */
    uint8_t client_protocol_version[S2N_TLS_PROTOCOL_VERSION_LEN] = { 0 };
    POSIX_GUARD_RESULT(s2n_client_hello_parse_raw(&conn->client_hello,
            client_protocol_version, conn->handshake_params.client_random));

    /* Protocol version in the ClientHello is fixed at 0x0303(TLS 1.2) for
     * future versions of TLS. Therefore, we will negotiate down if a client sends
     * an unexpected value above 0x0303.
     */
    conn->client_protocol_version = MIN((client_protocol_version[0] * 10) + client_protocol_version[1], S2N_TLS12);
    conn->client_hello_version = conn->client_protocol_version;

    /* Copy the session id to the connection. */
    conn->session_id_len = conn->client_hello.session_id.size;
    POSIX_CHECKED_MEMCPY(conn->session_id, conn->client_hello.session_id.data, conn->session_id_len);

    POSIX_GUARD_RESULT(s2n_client_hello_verify_for_retry(conn,
            &previous_hello_retry, &conn->client_hello, previous_client_random));
    return S2N_SUCCESS;
}

static S2N_RESULT s2n_client_hello_parse_message_impl(struct s2n_client_hello **result,
        const uint8_t *raw_message, uint32_t raw_message_size)
{
    RESULT_ENSURE_REF(result);

    DEFER_CLEANUP(struct s2n_blob mem = { 0 }, s2n_free);
    RESULT_GUARD_POSIX(s2n_alloc(&mem, sizeof(struct s2n_client_hello)));
    RESULT_GUARD_POSIX(s2n_blob_zero(&mem));

    DEFER_CLEANUP(struct s2n_client_hello *client_hello = NULL, s2n_client_hello_free);
    client_hello = (struct s2n_client_hello *) (void *) mem.data;
    client_hello->alloced = true;
    ZERO_TO_DISABLE_DEFER_CLEANUP(mem);

    DEFER_CLEANUP(struct s2n_stuffer in = { 0 }, s2n_stuffer_free);
    RESULT_GUARD_POSIX(s2n_stuffer_alloc(&in, raw_message_size));
    RESULT_GUARD_POSIX(s2n_stuffer_write_bytes(&in, raw_message, raw_message_size));

    uint8_t message_type = 0;
    uint32_t message_len = 0;
    RESULT_GUARD(s2n_handshake_parse_header(&in, &message_type, &message_len));
    RESULT_ENSURE(message_type == TLS_CLIENT_HELLO, S2N_ERR_BAD_MESSAGE);
    RESULT_ENSURE(message_len == s2n_stuffer_data_available(&in), S2N_ERR_BAD_MESSAGE);

    RESULT_GUARD_POSIX(s2n_collect_client_hello(client_hello, &in));
    RESULT_ENSURE(s2n_stuffer_data_available(&in) == 0, S2N_ERR_BAD_MESSAGE);

    uint8_t protocol_version[S2N_TLS_PROTOCOL_VERSION_LEN] = { 0 };
    uint8_t random[S2N_TLS_RANDOM_DATA_LEN] = { 0 };
    RESULT_GUARD(s2n_client_hello_parse_raw(client_hello, protocol_version, random));

    *result = client_hello;
    ZERO_TO_DISABLE_DEFER_CLEANUP(client_hello);
    return S2N_RESULT_OK;
}

struct s2n_client_hello *s2n_client_hello_parse_message(const uint8_t *raw_message, uint32_t raw_message_size)
{
    struct s2n_client_hello *result = NULL;
    PTR_GUARD_RESULT(s2n_client_hello_parse_message_impl(&result, raw_message, raw_message_size));
    return result;
}

int s2n_process_client_hello(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);
    POSIX_ENSURE_REF(conn->secure->cipher_suite);

    /* Client hello is parsed and config is finalized.
     * Negotiate protocol version, cipher suite, ALPN, select a cert, etc. */
    struct s2n_client_hello *client_hello = &conn->client_hello;

    const struct s2n_security_policy *security_policy = NULL;
    POSIX_GUARD(s2n_connection_get_security_policy(conn, &security_policy));

    if (!s2n_connection_supports_tls13(conn) || !s2n_security_policy_supports_tls13(security_policy)) {
        conn->server_protocol_version = MIN(conn->server_protocol_version, S2N_TLS12);
        conn->actual_protocol_version = MIN(conn->server_protocol_version, S2N_TLS12);
    }

    /* Set default key exchange curve.
     * This is going to be our fallback if the client has no preference.
     *
     * P-256 is our preferred fallback option because the TLS1.3 RFC requires
     * all implementations to support it:
     *
     *     https://tools.ietf.org/rfc/rfc8446#section-9.1
     *     A TLS-compliant application MUST support key exchange with secp256r1 (NIST P-256)
     *     and SHOULD support key exchange with X25519 [RFC7748]
     *
     *= https://www.rfc-editor.org/rfc/rfc4492#section-4
     *# A client that proposes ECC cipher suites may choose not to include these extensions.
     *# In this case, the server is free to choose any one of the elliptic curves or point formats listed in Section 5.
     *
     */
    const struct s2n_ecc_preferences *ecc_pref = NULL;
    POSIX_GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_pref));
    POSIX_ENSURE_REF(ecc_pref);
    POSIX_ENSURE_GT(ecc_pref->count, 0);
    if (s2n_ecc_preferences_includes_curve(ecc_pref, TLS_EC_CURVE_SECP_256_R1)) {
        conn->kex_params.server_ecc_evp_params.negotiated_curve = &s2n_ecc_curve_secp256r1;
    } else {
        /* If P-256 isn't allowed by the current security policy, instead choose
         * the first / most preferred curve.
         */
        conn->kex_params.server_ecc_evp_params.negotiated_curve = ecc_pref->ecc_curves[0];
    }

    POSIX_GUARD(s2n_extension_list_process(S2N_EXTENSION_LIST_CLIENT_HELLO, conn, &conn->client_hello.extensions));

    /* After parsing extensions, select a curve and corresponding keyshare to use */
    if (conn->actual_protocol_version >= S2N_TLS13) {
        POSIX_GUARD(s2n_extensions_server_key_share_select(conn));
    }

    /* for pre TLS 1.3 connections, protocol selection is not done in supported_versions extensions, so do it here */
    if (conn->actual_protocol_version < S2N_TLS13) {
        conn->actual_protocol_version = MIN(conn->server_protocol_version, conn->client_protocol_version);
    }

    if (conn->client_protocol_version < security_policy->minimum_protocol_version) {
        POSIX_GUARD(s2n_queue_reader_unsupported_protocol_version_alert(conn));
        POSIX_BAIL(S2N_ERR_PROTOCOL_VERSION_UNSUPPORTED);
    }

    if (s2n_connection_is_quic_enabled(conn)) {
        POSIX_ENSURE(conn->actual_protocol_version >= S2N_TLS13, S2N_ERR_PROTOCOL_VERSION_UNSUPPORTED);
    }

    /* Find potential certificate matches before we choose the cipher. */
    POSIX_GUARD(s2n_conn_find_name_matching_certs(conn));

    /* Save the previous cipher suite */
    uint8_t previous_cipher_suite_iana[S2N_TLS_CIPHER_SUITE_LEN] = { 0 };
    POSIX_CHECKED_MEMCPY(previous_cipher_suite_iana, conn->secure->cipher_suite->iana_value, S2N_TLS_CIPHER_SUITE_LEN);

    /* Now choose the ciphers we have certs for. */
    POSIX_GUARD(s2n_set_cipher_as_tls_server(conn, client_hello->cipher_suites.data,
            client_hello->cipher_suites.size / 2));

    /* Check if this is the second client hello in a hello retry handshake */
    if (s2n_is_hello_retry_handshake(conn) && conn->handshake.message_number > 0) {
        /**
         *= https://www.rfc-editor.org/rfc/rfc8446#4.1.4
         *# Servers MUST ensure that they negotiate the
         *# same cipher suite when receiving a conformant updated ClientHello (if
         *# the server selects the cipher suite as the first step in the
         *# negotiation, then this will happen automatically).
         **/
        POSIX_ENSURE(s2n_constant_time_equals(previous_cipher_suite_iana, conn->secure->cipher_suite->iana_value,
                             S2N_TLS_CIPHER_SUITE_LEN),
                S2N_ERR_BAD_MESSAGE);
    }

    /* If we're using a PSK, we don't need to choose a signature algorithm or certificate,
     * because no additional auth is required. */
    if (conn->psk_params.chosen_psk != NULL) {
        return S2N_SUCCESS;
    }

    /* And set the signature and hash algorithm used for key exchange signatures */
    POSIX_GUARD_RESULT(s2n_signature_algorithm_select(conn));

    /* And finally, set the certs specified by the final auth + sig_alg combo. */
    POSIX_GUARD(s2n_select_certs_for_server_auth(conn, &conn->handshake_params.our_chain_and_key));

    return S2N_SUCCESS;
}

static S2N_RESULT s2n_client_hello_process_cb_response(struct s2n_connection *conn, int rc)
{
    if (rc < 0) {
        goto fail;
    }
    switch (conn->config->client_hello_cb_mode) {
        case S2N_CLIENT_HELLO_CB_BLOCKING: {
            if (rc) {
                conn->server_name_used = 1;
            }
            return S2N_RESULT_OK;
        }
        case S2N_CLIENT_HELLO_CB_NONBLOCKING: {
            if (conn->client_hello.callback_async_done) {
                return S2N_RESULT_OK;
            }
            conn->client_hello.callback_async_blocked = 1;
            RESULT_BAIL(S2N_ERR_ASYNC_BLOCKED);
        }
    }
fail:
    /* rc < 0 */
    RESULT_GUARD_POSIX(s2n_queue_reader_handshake_failure_alert(conn));
    RESULT_BAIL(S2N_ERR_CANCELLED);
}

int s2n_client_hello_recv(struct s2n_connection *conn)
{
    POSIX_ENSURE(!conn->client_hello.callback_async_blocked, S2N_ERR_ASYNC_BLOCKED);

    /* Only parse the ClientHello once */
    if (!conn->client_hello.parsed) {
        POSIX_GUARD(s2n_parse_client_hello(conn));
        /* Mark the collected client hello as available when parsing is done and before the client hello callback */
        conn->client_hello.parsed = true;
    }

    /* Only invoke the ClientHello callback once.
     * This means that we do NOT invoke the callback again on the second ClientHello
     * in a TLS1.3 retry handshake. We explicitly check for a retry because the
     * callback state may have been cleared while parsing the second ClientHello.
     */
    if (!conn->client_hello.callback_invoked && !IS_HELLO_RETRY_HANDSHAKE(conn)) {
        /* Mark the client hello callback as invoked to avoid calling it again. */
        conn->client_hello.callback_invoked = true;

        /* Do NOT move this null check. A test exists to assert that a server connection can get
         * as far as the client hello callback without using its config. To do this we need a
         * specific error for a null config just before the client hello callback. The test's
         * assertions are weakened if this check is moved. */
        POSIX_ENSURE(conn->config, S2N_ERR_CONFIG_NULL_BEFORE_CH_CALLBACK);

        /* Call client_hello_cb if exists, letting application to modify s2n_connection or swap s2n_config */
        if (conn->config->client_hello_cb) {
            int rc = conn->config->client_hello_cb(conn, conn->config->client_hello_cb_ctx);
            POSIX_GUARD_RESULT(s2n_client_hello_process_cb_response(conn, rc));
        }
    }

    if (conn->client_hello_version != S2N_SSLv2) {
        POSIX_GUARD(s2n_process_client_hello(conn));
    }

    return 0;
}

S2N_RESULT s2n_cipher_suite_validate_available(struct s2n_connection *conn, struct s2n_cipher_suite *cipher)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(cipher);
    RESULT_ENSURE_EQ(cipher->available, true);
    RESULT_ENSURE_LTE(cipher->minimum_required_tls_version, conn->client_protocol_version);
    if (s2n_connection_is_quic_enabled(conn)) {
        RESULT_ENSURE_GTE(cipher->minimum_required_tls_version, S2N_TLS13);
    }
    return S2N_RESULT_OK;
}

int s2n_client_hello_send(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    const struct s2n_security_policy *security_policy = NULL;
    POSIX_GUARD(s2n_connection_get_security_policy(conn, &security_policy));

    const struct s2n_cipher_preferences *cipher_preferences = security_policy->cipher_preferences;
    POSIX_ENSURE_REF(cipher_preferences);

    if (!s2n_connection_supports_tls13(conn) || !s2n_security_policy_supports_tls13(security_policy)) {
        conn->client_protocol_version = MIN(conn->client_protocol_version, S2N_TLS12);
        conn->actual_protocol_version = MIN(conn->actual_protocol_version, S2N_TLS12);
    }

    struct s2n_stuffer *out = &conn->handshake.io;
    uint8_t client_protocol_version[S2N_TLS_PROTOCOL_VERSION_LEN] = { 0 };

    uint8_t reported_protocol_version = MIN(conn->client_protocol_version, S2N_TLS12);
    client_protocol_version[0] = reported_protocol_version / 10;
    client_protocol_version[1] = reported_protocol_version % 10;
    conn->client_hello_version = reported_protocol_version;
    POSIX_GUARD(s2n_stuffer_write_bytes(out, client_protocol_version, S2N_TLS_PROTOCOL_VERSION_LEN));

    struct s2n_blob client_random = { 0 };
    POSIX_GUARD(s2n_blob_init(&client_random, conn->handshake_params.client_random, S2N_TLS_RANDOM_DATA_LEN));
    if (!s2n_is_hello_retry_handshake(conn)) {
        /* Only generate the random data for our first client hello.
         * If we retry, we'll reuse the value. */
        POSIX_GUARD_RESULT(s2n_get_public_random_data(&client_random));
    }
    POSIX_GUARD(s2n_stuffer_write(out, &client_random));

    POSIX_GUARD_RESULT(s2n_generate_client_session_id(conn));
    POSIX_GUARD(s2n_stuffer_write_uint8(out, conn->session_id_len));
    if (conn->session_id_len > 0) {
        POSIX_GUARD(s2n_stuffer_write_bytes(out, conn->session_id, conn->session_id_len));
    }

    /* Reserve space for size of the list of available ciphers */
    struct s2n_stuffer_reservation available_cipher_suites_size;
    POSIX_GUARD(s2n_stuffer_reserve_uint16(out, &available_cipher_suites_size));

    /* Now, write the IANA values of every available cipher suite in our list */
    struct s2n_cipher_suite *cipher = NULL;
    bool tls12_is_possible = false;
    for (size_t i = 0; i < security_policy->cipher_preferences->count; i++) {
        cipher = cipher_preferences->suites[i];
        if (s2n_result_is_error(s2n_cipher_suite_validate_available(conn, cipher))) {
            continue;
        }
        if (cipher->minimum_required_tls_version < S2N_TLS13) {
            tls12_is_possible = true;
        }
        POSIX_GUARD(s2n_stuffer_write_bytes(out, cipher->iana_value, S2N_TLS_CIPHER_SUITE_LEN));
    }

    /**
     * For initial handshakes:
     *= https://www.rfc-editor.org/rfc/rfc5746#3.4
     *# o  The client MUST include either an empty "renegotiation_info"
     *#    extension, or the TLS_EMPTY_RENEGOTIATION_INFO_SCSV signaling
     *#    cipher suite value in the ClientHello.  Including both is NOT
     *#    RECOMMENDED.
     * For maximum backwards compatibility, we choose to use the TLS_EMPTY_RENEGOTIATION_INFO_SCSV cipher suite
     * rather than the "renegotiation_info" extension.
     *
     * For renegotiation handshakes:
     *= https://www.rfc-editor.org/rfc/rfc5746#3.5
     *# The SCSV MUST NOT be included.
     */
    if (tls12_is_possible && !s2n_handshake_is_renegotiation(conn)) {
        uint8_t renegotiation_info_scsv[S2N_TLS_CIPHER_SUITE_LEN] = { TLS_EMPTY_RENEGOTIATION_INFO_SCSV };
        POSIX_GUARD(s2n_stuffer_write_bytes(out, renegotiation_info_scsv, S2N_TLS_CIPHER_SUITE_LEN));
    }

    /* Write size of the list of available ciphers */
    uint32_t ciphers_size = 0;
    POSIX_GUARD(s2n_stuffer_get_vector_size(&available_cipher_suites_size, &ciphers_size));
    POSIX_ENSURE(ciphers_size > 0, S2N_ERR_INVALID_CIPHER_PREFERENCES);
    POSIX_GUARD(s2n_stuffer_write_reservation(&available_cipher_suites_size, ciphers_size));

    /* Zero compression methods */
    POSIX_GUARD(s2n_stuffer_write_uint8(out, 1));
    POSIX_GUARD(s2n_stuffer_write_uint8(out, 0));

    /* Write the extensions */
    POSIX_GUARD(s2n_extension_list_send(S2N_EXTENSION_LIST_CLIENT_HELLO, conn, out));

    /* Once the message is complete, finish calculating the PSK binders.
     *
     * The PSK binders require all the sizes in the ClientHello to be written correctly,
     * including the extension size and extension list size, and therefore have
     * to be calculated AFTER we finish writing the entire extension list. */
    POSIX_GUARD_RESULT(s2n_finish_psk_extension(conn));

    /* If early data was not requested as part of the ClientHello, it never will be. */
    if (conn->early_data_state == S2N_UNKNOWN_EARLY_DATA_STATE) {
        POSIX_GUARD_RESULT(s2n_connection_set_early_data_state(conn, S2N_EARLY_DATA_NOT_REQUESTED));
    }

    return S2N_SUCCESS;
}

/*
 * s2n-tls does NOT support SSLv2. However, it does support SSLv2 ClientHellos.
 * Clients may send SSLv2 ClientHellos advertising higher protocol versions for
 * backwards compatibility reasons. See https://tools.ietf.org/rfc/rfc2246 Appendix E.
 *
 * In this case, conn->client_hello_version will be SSLv2, but conn->client_protocol_version
 * will likely be higher.
 *
 * See http://www-archive.mozilla.org/projects/security/pki/nss/ssl/draft02.html Section 2.5
 * for a description of the expected SSLv2 format.
 * Alternatively, the TLS1.0 RFC includes a more modern description of the format:
 * https://tools.ietf.org/rfc/rfc2246 Appendix E.1
 */
int s2n_sslv2_client_hello_recv(struct s2n_connection *conn)
{
    struct s2n_client_hello *client_hello = &conn->client_hello;
    client_hello->sslv2 = true;

    struct s2n_stuffer in_stuffer = { 0 };
    POSIX_GUARD(s2n_stuffer_init(&in_stuffer, &client_hello->raw_message));
    POSIX_GUARD(s2n_stuffer_skip_write(&in_stuffer, client_hello->raw_message.size));
    struct s2n_stuffer *in = &in_stuffer;

    const struct s2n_security_policy *security_policy = NULL;
    POSIX_GUARD(s2n_connection_get_security_policy(conn, &security_policy));

    if (conn->client_protocol_version < security_policy->minimum_protocol_version) {
        POSIX_GUARD(s2n_queue_reader_unsupported_protocol_version_alert(conn));
        POSIX_BAIL(S2N_ERR_PROTOCOL_VERSION_UNSUPPORTED);
    }
    conn->actual_protocol_version = MIN(conn->client_protocol_version, conn->server_protocol_version);

    /* We start 5 bytes into the record */
    uint16_t cipher_suites_length = 0;
    POSIX_GUARD(s2n_stuffer_read_uint16(in, &cipher_suites_length));
    POSIX_ENSURE(cipher_suites_length > 0, S2N_ERR_BAD_MESSAGE);
    POSIX_ENSURE(cipher_suites_length % S2N_SSLv2_CIPHER_SUITE_LEN == 0, S2N_ERR_BAD_MESSAGE);

    uint16_t session_id_length = 0;
    POSIX_GUARD(s2n_stuffer_read_uint16(in, &session_id_length));

    uint16_t challenge_length = 0;
    POSIX_GUARD(s2n_stuffer_read_uint16(in, &challenge_length));

    S2N_ERROR_IF(challenge_length > S2N_TLS_RANDOM_DATA_LEN, S2N_ERR_BAD_MESSAGE);

    client_hello->cipher_suites.size = cipher_suites_length;
    client_hello->cipher_suites.data = s2n_stuffer_raw_read(in, cipher_suites_length);
    POSIX_ENSURE_REF(client_hello->cipher_suites.data);

    /* Find potential certificate matches before we choose the cipher. */
    POSIX_GUARD(s2n_conn_find_name_matching_certs(conn));

    POSIX_GUARD(s2n_set_cipher_as_sslv2_server(conn, client_hello->cipher_suites.data,
            client_hello->cipher_suites.size / S2N_SSLv2_CIPHER_SUITE_LEN));
    POSIX_GUARD_RESULT(s2n_signature_algorithm_select(conn));
    POSIX_GUARD(s2n_select_certs_for_server_auth(conn, &conn->handshake_params.our_chain_and_key));

    S2N_ERROR_IF(session_id_length > s2n_stuffer_data_available(in), S2N_ERR_BAD_MESSAGE);
    POSIX_GUARD(s2n_blob_init(&client_hello->session_id, s2n_stuffer_raw_read(in, session_id_length), session_id_length));
    if (session_id_length > 0 && session_id_length <= S2N_TLS_SESSION_ID_MAX_LEN) {
        POSIX_CHECKED_MEMCPY(conn->session_id, client_hello->session_id.data, session_id_length);
        conn->session_id_len = (uint8_t) session_id_length;
    }

    struct s2n_blob b = { 0 };
    POSIX_GUARD(s2n_blob_init(&b, conn->handshake_params.client_random, S2N_TLS_RANDOM_DATA_LEN));

    b.data += S2N_TLS_RANDOM_DATA_LEN - challenge_length;
    b.size -= S2N_TLS_RANDOM_DATA_LEN - challenge_length;

    POSIX_GUARD(s2n_stuffer_read(in, &b));

    return 0;
}

int s2n_client_hello_get_parsed_extension(s2n_tls_extension_type extension_type,
        s2n_parsed_extensions_list *parsed_extension_list, s2n_parsed_extension **parsed_extension)
{
    POSIX_ENSURE_REF(parsed_extension_list);
    POSIX_ENSURE_REF(parsed_extension);

    s2n_extension_type_id extension_type_id = 0;
    POSIX_GUARD(s2n_extension_supported_iana_value_to_id(extension_type, &extension_type_id));

    s2n_parsed_extension *found_parsed_extension = &parsed_extension_list->parsed_extensions[extension_type_id];
    POSIX_ENSURE(found_parsed_extension->extension.data, S2N_ERR_EXTENSION_NOT_RECEIVED);
    POSIX_ENSURE(found_parsed_extension->extension_type == extension_type, S2N_ERR_INVALID_PARSED_EXTENSIONS);

    *parsed_extension = found_parsed_extension;
    return S2N_SUCCESS;
}

ssize_t s2n_client_hello_get_extension_length(struct s2n_client_hello *ch, s2n_tls_extension_type extension_type)
{
    POSIX_ENSURE_REF(ch);

    s2n_parsed_extension *parsed_extension = NULL;
    if (s2n_client_hello_get_parsed_extension(extension_type, &ch->extensions, &parsed_extension) != S2N_SUCCESS) {
        return 0;
    }

    return parsed_extension->extension.size;
}

ssize_t s2n_client_hello_get_extension_by_id(struct s2n_client_hello *ch, s2n_tls_extension_type extension_type, uint8_t *out, uint32_t max_length)
{
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(out);

    s2n_parsed_extension *parsed_extension = NULL;
    if (s2n_client_hello_get_parsed_extension(extension_type, &ch->extensions, &parsed_extension) != S2N_SUCCESS) {
        return 0;
    }

    uint32_t len = min_size(&parsed_extension->extension, max_length);
    POSIX_CHECKED_MEMCPY(out, parsed_extension->extension.data, len);
    return len;
}

int s2n_client_hello_get_session_id_length(struct s2n_client_hello *ch, uint32_t *out_length)
{
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(out_length);
    *out_length = ch->session_id.size;
    return S2N_SUCCESS;
}

int s2n_client_hello_get_session_id(struct s2n_client_hello *ch, uint8_t *out, uint32_t *out_length, uint32_t max_length)
{
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(out);
    POSIX_ENSURE_REF(out_length);

    uint32_t len = min_size(&ch->session_id, max_length);
    POSIX_CHECKED_MEMCPY(out, ch->session_id.data, len);
    *out_length = len;

    return S2N_SUCCESS;
}

int s2n_client_hello_get_compression_methods_length(struct s2n_client_hello *ch, uint32_t *out_length)
{
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(out_length);
    *out_length = ch->compression_methods.size;
    return S2N_SUCCESS;
}

int s2n_client_hello_get_compression_methods(struct s2n_client_hello *ch, uint8_t *list, uint32_t list_length, uint32_t *out_length)
{
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(list);
    POSIX_ENSURE_REF(out_length);

    POSIX_ENSURE(list_length >= ch->compression_methods.size, S2N_ERR_INSUFFICIENT_MEM_SIZE);
    POSIX_CHECKED_MEMCPY(list, ch->compression_methods.data, ch->compression_methods.size);
    *out_length = ch->compression_methods.size;
    return S2N_SUCCESS;
}

int s2n_client_hello_get_legacy_protocol_version(struct s2n_client_hello *ch, uint8_t *out)
{
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(out);
    *out = ch->legacy_version;
    return S2N_SUCCESS;
}

int s2n_client_hello_get_legacy_record_version(struct s2n_client_hello *ch, uint8_t *out)
{
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(out);
    POSIX_ENSURE(ch->record_version_recorded, S2N_ERR_INVALID_ARGUMENT);
    *out = ch->legacy_record_version;
    return S2N_SUCCESS;
}

S2N_RESULT s2n_client_hello_get_raw_extension(uint16_t extension_iana,
        struct s2n_blob *raw_extensions, struct s2n_blob *extension)
{
    RESULT_ENSURE_REF(raw_extensions);
    RESULT_ENSURE_REF(extension);

    *extension = (struct s2n_blob){ 0 };

    struct s2n_stuffer raw_extensions_stuffer = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init(&raw_extensions_stuffer, raw_extensions));
    RESULT_GUARD_POSIX(s2n_stuffer_skip_write(&raw_extensions_stuffer, raw_extensions->size));

    while (s2n_stuffer_data_available(&raw_extensions_stuffer) > 0) {
        uint16_t extension_type = 0;
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(&raw_extensions_stuffer, &extension_type));

        uint16_t extension_size = 0;
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(&raw_extensions_stuffer, &extension_size));

        uint8_t *extension_data = s2n_stuffer_raw_read(&raw_extensions_stuffer, extension_size);
        RESULT_ENSURE_REF(extension_data);

        if (extension_iana == extension_type) {
            RESULT_GUARD_POSIX(s2n_blob_init(extension, extension_data, extension_size));
            return S2N_RESULT_OK;
        }
    }
    return S2N_RESULT_OK;
}

int s2n_client_hello_has_extension(struct s2n_client_hello *ch, uint16_t extension_iana, bool *exists)
{
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(exists);

    *exists = false;

    s2n_extension_type_id extension_type_id = s2n_unsupported_extension;
    if (s2n_extension_supported_iana_value_to_id(extension_iana, &extension_type_id) == S2N_SUCCESS) {
        s2n_parsed_extension *parsed_extension = NULL;
        if (s2n_client_hello_get_parsed_extension(extension_iana, &ch->extensions, &parsed_extension) == S2N_SUCCESS) {
            *exists = true;
        }
        return S2N_SUCCESS;
    }

    struct s2n_blob extension = { 0 };
    POSIX_GUARD_RESULT(s2n_client_hello_get_raw_extension(extension_iana, &ch->extensions.raw, &extension));
    if (extension.data != NULL) {
        *exists = true;
    }
    return S2N_SUCCESS;
}

int s2n_client_hello_get_supported_groups(struct s2n_client_hello *ch, uint16_t *groups,
        uint16_t groups_count_max, uint16_t *groups_count_out)
{
    POSIX_ENSURE_REF(groups_count_out);
    *groups_count_out = 0;
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(groups);

    s2n_parsed_extension *supported_groups_extension = NULL;
    POSIX_GUARD(s2n_client_hello_get_parsed_extension(S2N_EXTENSION_SUPPORTED_GROUPS, &ch->extensions, &supported_groups_extension));
    POSIX_ENSURE_REF(supported_groups_extension);

    struct s2n_stuffer extension_stuffer = { 0 };
    POSIX_GUARD(s2n_stuffer_init_written(&extension_stuffer, &supported_groups_extension->extension));

    uint16_t supported_groups_count = 0;
    POSIX_GUARD_RESULT(s2n_supported_groups_parse_count(&extension_stuffer, &supported_groups_count));
    POSIX_ENSURE(supported_groups_count <= groups_count_max, S2N_ERR_INSUFFICIENT_MEM_SIZE);

    for (size_t i = 0; i < supported_groups_count; i++) {
        /* s2n_stuffer_read_uint16 is used to read each of the supported groups in network-order
         * endianness.
         */
        POSIX_GUARD(s2n_stuffer_read_uint16(&extension_stuffer, &groups[i]));
    }

    *groups_count_out = supported_groups_count;

    return S2N_SUCCESS;
}

int s2n_client_hello_get_server_name_length(struct s2n_client_hello *ch, uint16_t *length)
{
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(length);
    *length = 0;

    s2n_parsed_extension *server_name_extension = NULL;
    POSIX_GUARD(s2n_client_hello_get_parsed_extension(S2N_EXTENSION_SERVER_NAME, &ch->extensions, &server_name_extension));
    POSIX_ENSURE_REF(server_name_extension);

    struct s2n_stuffer extension_stuffer = { 0 };
    POSIX_GUARD(s2n_stuffer_init_written(&extension_stuffer, &server_name_extension->extension));

    struct s2n_blob blob = { 0 };
    POSIX_GUARD_RESULT(s2n_client_server_name_parse(&extension_stuffer, &blob));
    *length = blob.size;

    return S2N_SUCCESS;
}

int s2n_client_hello_get_server_name(struct s2n_client_hello *ch, uint8_t *server_name, uint16_t length, uint16_t *out_length)
{
    POSIX_ENSURE_REF(out_length);
    POSIX_ENSURE_REF(ch);
    POSIX_ENSURE_REF(server_name);
    *out_length = 0;

    s2n_parsed_extension *server_name_extension = NULL;
    POSIX_GUARD(s2n_client_hello_get_parsed_extension(S2N_EXTENSION_SERVER_NAME, &ch->extensions, &server_name_extension));
    POSIX_ENSURE_REF(server_name_extension);

    struct s2n_stuffer extension_stuffer = { 0 };
    POSIX_GUARD(s2n_stuffer_init_written(&extension_stuffer, &server_name_extension->extension));

    struct s2n_blob blob = { 0 };
    POSIX_GUARD_RESULT(s2n_client_server_name_parse(&extension_stuffer, &blob));
    POSIX_ENSURE_LTE(blob.size, length);
    POSIX_CHECKED_MEMCPY(server_name, blob.data, blob.size);

    *out_length = blob.size;

    return S2N_SUCCESS;
}
