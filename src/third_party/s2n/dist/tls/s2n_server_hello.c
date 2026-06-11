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

#include <sys/param.h>
#include <time.h>

#include "api/s2n.h"
#include "crypto/s2n_fips.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_alerts.h"
#include "tls/s2n_cipher_preferences.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_security_policies.h"
#include "tls/s2n_server_extensions.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls13.h"
#include "tls/s2n_tls13_handshake.h"
#include "tls/s2n_tls13_key_schedule.h"
#include "utils/s2n_bitmap.h"
#include "utils/s2n_random.h"
#include "utils/s2n_safety.h"

/* From RFC5246 7.4.1.2. */
#define S2N_TLS_COMPRESSION_METHOD_NULL 0

/* From RFC8446 4.1.3. */
#define S2N_DOWNGRADE_PROTECTION_SIZE 8
const uint8_t tls12_downgrade_protection_bytes[] = {
    0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x01
};

const uint8_t tls11_downgrade_protection_bytes[] = {
    0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x00
};

static int s2n_random_value_is_hello_retry(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    POSIX_ENSURE(s2n_constant_time_equals(hello_retry_req_random, conn->handshake_params.server_random, S2N_TLS_RANDOM_DATA_LEN),
            S2N_ERR_INVALID_HELLO_RETRY);

    return S2N_SUCCESS;
}

static int s2n_client_detect_downgrade_mechanism(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    uint8_t *downgrade_bytes = &conn->handshake_params.server_random[S2N_TLS_RANDOM_DATA_LEN - S2N_DOWNGRADE_PROTECTION_SIZE];

    /* Detect downgrade attacks according to RFC 8446 section 4.1.3 */
    if (conn->client_protocol_version == S2N_TLS13 && conn->server_protocol_version == S2N_TLS12) {
        if (s2n_constant_time_equals(downgrade_bytes, tls12_downgrade_protection_bytes, S2N_DOWNGRADE_PROTECTION_SIZE)) {
            POSIX_BAIL(S2N_ERR_PROTOCOL_DOWNGRADE_DETECTED);
        }
    } else if (conn->client_protocol_version == S2N_TLS13 && conn->server_protocol_version <= S2N_TLS11) {
        if (s2n_constant_time_equals(downgrade_bytes, tls11_downgrade_protection_bytes, S2N_DOWNGRADE_PROTECTION_SIZE)) {
            POSIX_BAIL(S2N_ERR_PROTOCOL_DOWNGRADE_DETECTED);
        }
    }

    return 0;
}

static int s2n_server_add_downgrade_mechanism(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    uint8_t *downgrade_bytes = &conn->handshake_params.server_random[S2N_TLS_RANDOM_DATA_LEN - S2N_DOWNGRADE_PROTECTION_SIZE];

    /* Protect against downgrade attacks according to RFC 8446 section 4.1.3 */
    if (conn->server_protocol_version >= S2N_TLS13 && conn->actual_protocol_version == S2N_TLS12) {
        /* TLS1.3 servers MUST use a special random value when negotiating TLS1.2 */
        POSIX_CHECKED_MEMCPY(downgrade_bytes, tls12_downgrade_protection_bytes, S2N_DOWNGRADE_PROTECTION_SIZE);
    } else if (conn->server_protocol_version >= S2N_TLS13 && conn->actual_protocol_version <= S2N_TLS11) {
        /* TLS1.3 servers MUST, use a special random value when negotiating TLS1.1 or below */
        POSIX_CHECKED_MEMCPY(downgrade_bytes, tls11_downgrade_protection_bytes, S2N_DOWNGRADE_PROTECTION_SIZE);
    }

    return 0;
}

static int s2n_server_hello_parse(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);

    struct s2n_stuffer *in = &conn->handshake.io;
    uint8_t compression_method = 0;
    uint8_t session_id_len = 0;
    uint8_t protocol_version[S2N_TLS_PROTOCOL_VERSION_LEN];
    uint8_t session_id[S2N_TLS_SESSION_ID_MAX_LEN];

    POSIX_GUARD(s2n_stuffer_read_bytes(in, protocol_version, S2N_TLS_PROTOCOL_VERSION_LEN));
    POSIX_GUARD(s2n_stuffer_read_bytes(in, conn->handshake_params.server_random, S2N_TLS_RANDOM_DATA_LEN));

    uint8_t legacy_version = (uint8_t) (protocol_version[0] * 10) + protocol_version[1];

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#4.1.3
     *# Upon receiving a message with type server_hello, implementations MUST
     *# first examine the Random value and, if it matches this value, process
     *# it as described in Section 4.1.4).
     **/
    if (s2n_random_value_is_hello_retry(conn) == S2N_SUCCESS) {
        /**
         *= https://www.rfc-editor.org/rfc/rfc8446#4.1.4
         *# If a client receives a second
         *# HelloRetryRequest in the same connection (i.e., where the ClientHello
         *# was itself in response to a HelloRetryRequest), it MUST abort the
         *# handshake with an "unexpected_message" alert.
         **/
        POSIX_ENSURE(!s2n_is_hello_retry_handshake(conn), S2N_ERR_INVALID_HELLO_RETRY);

        /**
         *= https://www.rfc-editor.org/rfc/rfc8446#4.1.4
         *# Upon receipt of a HelloRetryRequest, the client MUST check the
         *# legacy_version
         **/
        POSIX_ENSURE(legacy_version == S2N_TLS12, S2N_ERR_INVALID_HELLO_RETRY);

        POSIX_GUARD(s2n_set_hello_retry_required(conn));
    }

    POSIX_GUARD(s2n_stuffer_read_uint8(in, &session_id_len));
    S2N_ERROR_IF(session_id_len > S2N_TLS_SESSION_ID_MAX_LEN, S2N_ERR_BAD_MESSAGE);
    POSIX_GUARD(s2n_stuffer_read_bytes(in, session_id, session_id_len));

    uint8_t *cipher_suite_wire = s2n_stuffer_raw_read(in, S2N_TLS_CIPHER_SUITE_LEN);
    POSIX_ENSURE_REF(cipher_suite_wire);

    POSIX_GUARD(s2n_stuffer_read_uint8(in, &compression_method));

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#4.1.3
     *# legacy_compression_method:  A single byte which MUST have the
     *# value 0.
     *
     *= https://www.rfc-editor.org/rfc/rfc8446#4.1.4
     *# Upon receipt of a HelloRetryRequest, the client MUST check the
     *# legacy_version, legacy_session_id_echo, cipher_suite, and
     *# legacy_compression_method
     **/
    S2N_ERROR_IF(compression_method != S2N_TLS_COMPRESSION_METHOD_NULL, S2N_ERR_BAD_MESSAGE);

    bool session_ids_match = session_id_len != 0 && session_id_len == conn->session_id_len
            && s2n_constant_time_equals(session_id, conn->session_id, session_id_len);
    if (!session_ids_match) {
        conn->ems_negotiated = false;
    }

    POSIX_GUARD(s2n_server_extensions_recv(conn, in));

    /**
    *= https://www.rfc-editor.org/rfc/rfc8446#4.1.4
    *# The server's extensions MUST contain "supported_versions".
    **/
    if (s2n_is_hello_retry_message(conn)) {
        s2n_extension_type_id supported_versions_id = s2n_unsupported_extension;
        POSIX_GUARD(s2n_extension_supported_iana_value_to_id(TLS_EXTENSION_SUPPORTED_VERSIONS, &supported_versions_id));
        POSIX_ENSURE(S2N_CBIT_TEST(conn->extension_responses_received, supported_versions_id),
                S2N_ERR_MISSING_EXTENSION);
    }

    if (conn->server_protocol_version >= S2N_TLS13) {
        POSIX_ENSURE(!conn->handshake.renegotiation, S2N_ERR_PROTOCOL_VERSION_UNSUPPORTED);

        /**
         *= https://www.rfc-editor.org/rfc/rfc8446#section-4.1.3
         *# A client which
         *# receives a legacy_session_id_echo field that does not match what
         *# it sent in the ClientHello MUST abort the handshake with an
         *# "illegal_parameter" alert.
         *
         *= https://www.rfc-editor.org/rfc/rfc8446#4.1.4
         *# Upon receipt of a HelloRetryRequest, the client MUST check the
         *# legacy_version, legacy_session_id_echo
         **/
        POSIX_ENSURE(session_ids_match || (session_id_len == 0 && conn->session_id_len == 0), S2N_ERR_BAD_MESSAGE);

        conn->actual_protocol_version = conn->server_protocol_version;
        POSIX_GUARD(s2n_set_cipher_as_client(conn, cipher_suite_wire));

        /* Erase TLS 1.2 client session ticket which might have been set for session resumption */
        POSIX_GUARD(s2n_free(&conn->client_ticket));
    } else {
        conn->server_protocol_version = legacy_version;

        POSIX_ENSURE(!s2n_client_detect_downgrade_mechanism(conn), S2N_ERR_PROTOCOL_DOWNGRADE_DETECTED);
        POSIX_ENSURE(!s2n_connection_is_quic_enabled(conn), S2N_ERR_PROTOCOL_VERSION_UNSUPPORTED);

        /* Hello retries are only supported in >=TLS1.3. */
        POSIX_ENSURE(!s2n_is_hello_retry_handshake(conn), S2N_ERR_BAD_MESSAGE);

        /*
         *= https://www.rfc-editor.org/rfc/rfc8446#appendix-D.3
         *# A client that attempts to send 0-RTT data MUST fail a connection if
         *# it receives a ServerHello with TLS 1.2 or older.
         */
        POSIX_ENSURE(conn->early_data_state != S2N_EARLY_DATA_REQUESTED, S2N_ERR_PROTOCOL_VERSION_UNSUPPORTED);

        const struct s2n_security_policy *security_policy = NULL;
        POSIX_GUARD(s2n_connection_get_security_policy(conn, &security_policy));

        if (conn->server_protocol_version < security_policy->minimum_protocol_version
                || conn->server_protocol_version > conn->client_protocol_version) {
            POSIX_GUARD(s2n_queue_reader_unsupported_protocol_version_alert(conn));
            POSIX_BAIL(S2N_ERR_PROTOCOL_VERSION_UNSUPPORTED);
        }

        conn->actual_protocol_version = MIN(conn->server_protocol_version, conn->client_protocol_version);

        /*
         *= https://www.rfc-editor.org/rfc/rfc5077#section-3.4
         *# If the server accepts the ticket
         *# and the Session ID is not empty, then it MUST respond with the same
         *# Session ID present in the ClientHello.  This allows the client to
         *# easily differentiate when the server is resuming a session from when
         *# it is falling back to a full handshake.
         */
        if (session_ids_match) {
            /* check if the resumed session state is valid */
            POSIX_ENSURE(conn->resume_protocol_version == conn->actual_protocol_version, S2N_ERR_BAD_MESSAGE);
            POSIX_ENSURE(s2n_constant_time_equals(conn->secure->cipher_suite->iana_value, cipher_suite_wire, S2N_TLS_CIPHER_SUITE_LEN),
                    S2N_ERR_BAD_MESSAGE);

            /* Session is resumed */
            conn->client_session_resumed = 1;
        } else {
            conn->session_id_len = session_id_len;
            POSIX_CHECKED_MEMCPY(conn->session_id, session_id, session_id_len);
            POSIX_GUARD(s2n_set_cipher_as_client(conn, cipher_suite_wire));
            /* Erase master secret which might have been set for session resumption */
            POSIX_CHECKED_MEMSET((uint8_t *) conn->secrets.version.tls12.master_secret, 0, S2N_TLS_SECRET_LEN);

            /* Erase client session ticket which might have been set for session resumption */
            POSIX_GUARD(s2n_free(&conn->client_ticket));
        }
    }

    /* If it is not possible to accept early data on this connection
     * (for example, because no PSK was negotiated) we need to reject early data now.
     * Otherwise, early data logic may make certain invalid assumptions about the
     * state of the connection (for example, that the prf is the early data prf).
     */
    POSIX_GUARD_RESULT(s2n_early_data_accept_or_reject(conn));
    if (conn->early_data_state == S2N_EARLY_DATA_REJECTED) {
        POSIX_GUARD_RESULT(s2n_tls13_key_schedule_reset(conn));
    }

    return 0;
}

int s2n_server_hello_recv(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    /* Read the message off the wire */
    POSIX_GUARD(s2n_server_hello_parse(conn));

    conn->actual_protocol_version_established = 1;

    POSIX_GUARD(s2n_conn_set_handshake_type(conn));

    /* If this is a HelloRetryRequest, we don't process the ServerHello.
     * Instead we proceed with retry logic. */
    if (s2n_is_hello_retry_message(conn)) {
        POSIX_GUARD(s2n_server_hello_retry_recv(conn));
        return 0;
    }

    if (conn->actual_protocol_version < S2N_TLS13 && s2n_connection_is_session_resumed(conn)) {
        POSIX_GUARD(s2n_prf_key_expansion(conn));
    }

    /* Update the required hashes for this connection */
    POSIX_GUARD(s2n_conn_update_required_handshake_hashes(conn));

    return 0;
}

int s2n_server_hello_write_message(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);

    /* The actual_protocol_version is set while processing the CLIENT_HELLO message, so
     * it could be S2N_TLS13. SERVER_HELLO should always respond with the legacy version.
     * https://tools.ietf.org/html/rfc8446#section-4.1.3 */
    const uint16_t legacy_protocol_version = MIN(conn->actual_protocol_version, S2N_TLS12);
    uint8_t protocol_version[S2N_TLS_PROTOCOL_VERSION_LEN];
    protocol_version[0] = (uint8_t) (legacy_protocol_version / 10);
    protocol_version[1] = (uint8_t) (legacy_protocol_version % 10);

    POSIX_GUARD(s2n_stuffer_write_bytes(&conn->handshake.io, protocol_version, S2N_TLS_PROTOCOL_VERSION_LEN));
    POSIX_GUARD(s2n_stuffer_write_bytes(&conn->handshake.io, conn->handshake_params.server_random, S2N_TLS_RANDOM_DATA_LEN));
    POSIX_GUARD(s2n_stuffer_write_uint8(&conn->handshake.io, conn->session_id_len));
    POSIX_GUARD(s2n_stuffer_write_bytes(&conn->handshake.io, conn->session_id, conn->session_id_len));
    POSIX_GUARD(s2n_stuffer_write_bytes(&conn->handshake.io, conn->secure->cipher_suite->iana_value, S2N_TLS_CIPHER_SUITE_LEN));
    POSIX_GUARD(s2n_stuffer_write_uint8(&conn->handshake.io, S2N_TLS_COMPRESSION_METHOD_NULL));

    return 0;
}

int s2n_server_hello_send(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    struct s2n_stuffer server_random = { 0 };
    struct s2n_blob b = { 0 };
    POSIX_GUARD(s2n_blob_init(&b, conn->handshake_params.server_random, S2N_TLS_RANDOM_DATA_LEN));

    /* Create the server random data */
    POSIX_GUARD(s2n_stuffer_init(&server_random, &b));

    struct s2n_blob rand_data = { 0 };
    POSIX_GUARD(s2n_blob_init(&rand_data, s2n_stuffer_raw_write(&server_random, S2N_TLS_RANDOM_DATA_LEN), S2N_TLS_RANDOM_DATA_LEN));
    POSIX_ENSURE_REF(rand_data.data);
    POSIX_GUARD_RESULT(s2n_get_public_random_data(&rand_data));

    /* Add a downgrade detection mechanism if required */
    POSIX_GUARD(s2n_server_add_downgrade_mechanism(conn));

    POSIX_GUARD(s2n_server_hello_write_message(conn));

    POSIX_GUARD(s2n_server_extensions_send(conn, &conn->handshake.io));

    conn->actual_protocol_version_established = 1;

    return 0;
}
