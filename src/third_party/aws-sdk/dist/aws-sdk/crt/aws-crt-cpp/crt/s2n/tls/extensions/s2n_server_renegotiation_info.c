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

#include "tls/extensions/s2n_server_renegotiation_info.h"

#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_safety.h"

/**
 * s2n-tls servers do NOT support renegotiation.
 *
 * We implement this extension to handle clients that require secure renegotiation support:
 *= https://www.rfc-editor.org/rfc/rfc5746#4.3
 *# In order to enable clients to probe, even servers that do not support
 *# renegotiation MUST implement the minimal version of the extension
 *# described in this document for initial handshakes, thus signaling
 *# that they have been upgraded.
 */

static bool s2n_renegotiation_info_should_send(struct s2n_connection *conn);
static int s2n_renegotiation_info_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_renegotiation_info_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);
static int s2n_renegotiation_info_if_missing(struct s2n_connection *conn);

const s2n_extension_type s2n_server_renegotiation_info_extension = {
    .iana_value = TLS_EXTENSION_RENEGOTIATION_INFO,
    .send = s2n_renegotiation_info_send,
    .recv = s2n_renegotiation_info_recv,
    .should_send = s2n_renegotiation_info_should_send,
    .if_missing = s2n_renegotiation_info_if_missing,

    /**
     *= https://www.rfc-editor.org/rfc/rfc5746#3.6
     *# Note that sending a "renegotiation_info" extension in response to a
     *# ClientHello containing only the SCSV is an explicit exception to the
     *# prohibition in RFC 5246, Section 7.4.1.4, on the server sending
     *# unsolicited extensions and is only allowed because the client is
     *# signaling its willingness to receive the extension via the
     *# TLS_EMPTY_RENEGOTIATION_INFO_SCSV SCSV.
     *
     * This extension is technically a response extension, but doesn't
     * follow any of the usual response extension rules.
     * s2n-tls will therefore not treat it as a response extension.
     */
    .is_response = false,
};

/**
 *= https://www.rfc-editor.org/rfc/rfc5746#3.6
 *# o  If the secure_renegotiation flag is set to TRUE, the server MUST
 *#    include an empty "renegotiation_info" extension in the ServerHello
 *#    message.
 */
static bool s2n_renegotiation_info_should_send(struct s2n_connection *conn)
{
    return conn && conn->secure_renegotiation && s2n_connection_get_protocol_version(conn) < S2N_TLS13;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc5746#3.6
 *# o  If the secure_renegotiation flag is set to TRUE, the server MUST
 *#    include an empty "renegotiation_info" extension in the ServerHello
 *#    message.
 */
static int s2n_renegotiation_info_send_initial(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_GUARD(s2n_stuffer_write_uint8(out, 0));
    return S2N_SUCCESS;
}

static int s2n_renegotiation_info_send_renegotiation(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);

    /* s2n-tls servers do not support renegotiation.
     * We add the renegotiation version of this logic only for testing.
     */
    POSIX_ENSURE(s2n_in_unit_test(), S2N_ERR_NOT_IN_UNIT_TEST);

    /**
     *= https://www.rfc-editor.org/rfc/rfc5746#3.7
     *# This text applies if the connection's "secure_renegotiation" flag is
     *# set to TRUE (if it is set to FALSE, see Section 4.4).
     */
    POSIX_ENSURE(conn->secure_renegotiation, S2N_ERR_NO_RENEGOTIATION);

    /**
     *= https://www.rfc-editor.org/rfc/rfc5746#3.7
     *# o  The server MUST include a "renegotiation_info" extension
     *#    containing the saved client_verify_data and server_verify_data in
     *#    the ServerHello.
     */
    const uint8_t verify_data_len = conn->handshake.finished_len;
    POSIX_ENSURE_GT(verify_data_len, 0);
    POSIX_GUARD(s2n_stuffer_write_uint8(out, verify_data_len * 2));
    POSIX_GUARD(s2n_stuffer_write_bytes(out, conn->handshake.client_finished, verify_data_len));
    POSIX_GUARD(s2n_stuffer_write_bytes(out, conn->handshake.server_finished, verify_data_len));

    return S2N_SUCCESS;
}

static int s2n_renegotiation_info_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    if (s2n_handshake_is_renegotiation(conn)) {
        POSIX_GUARD(s2n_renegotiation_info_send_renegotiation(conn, out));
    } else {
        POSIX_GUARD(s2n_renegotiation_info_send_initial(conn, out));
    }
    return S2N_SUCCESS;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc5746#3.4
 *# o  When a ServerHello is received, the client MUST check if it
 *#    includes the "renegotiation_info" extension:
 */
static int s2n_renegotiation_info_recv_initial(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);

    /**
     *= https://www.rfc-editor.org/rfc/rfc5746#3.4
     *# *  The client MUST then verify that the length of the
     *#    "renegotiated_connection" field is zero, and if it is not, MUST
     *#    abort the handshake (by sending a fatal handshake_failure alert).
     */
    uint8_t renegotiated_connection_len = 0;
    POSIX_GUARD(s2n_stuffer_read_uint8(extension, &renegotiated_connection_len));
    POSIX_ENSURE(s2n_stuffer_data_available(extension) == 0, S2N_ERR_NON_EMPTY_RENEGOTIATION_INFO);
    POSIX_ENSURE(renegotiated_connection_len == 0, S2N_ERR_NON_EMPTY_RENEGOTIATION_INFO);

    /**
     *= https://www.rfc-editor.org/rfc/rfc5746#3.4
     *# *  If the extension is present, set the secure_renegotiation flag to TRUE.
     */
    conn->secure_renegotiation = 1;
    return S2N_SUCCESS;
}

static int s2n_renegotiation_info_recv_renegotiation(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);
    uint8_t verify_data_len = conn->handshake.finished_len;
    POSIX_ENSURE_GT(verify_data_len, 0);

    /**
     *= https://www.rfc-editor.org/rfc/rfc5746#3.5
     *# This text applies if the connection's "secure_renegotiation" flag is
     *# set to TRUE (if it is set to FALSE, see Section 4.2).
     */
    POSIX_ENSURE(conn->secure_renegotiation, S2N_ERR_NO_RENEGOTIATION);

    /**
     *= https://www.rfc-editor.org/rfc/rfc5746#3.5
     *# o  The client MUST then verify that the first half of the
     *#    "renegotiated_connection" field is equal to the saved
     *#    client_verify_data value, and the second half is equal to the
     *#    saved server_verify_data value.  If they are not, the client MUST
     *#    abort the handshake.
     */

    uint8_t renegotiated_connection_len = 0;
    POSIX_GUARD(s2n_stuffer_read_uint8(extension, &renegotiated_connection_len));
    POSIX_ENSURE(verify_data_len * 2 == renegotiated_connection_len, S2N_ERR_BAD_MESSAGE);

    uint8_t *first_half = s2n_stuffer_raw_read(extension, verify_data_len);
    POSIX_ENSURE_REF(first_half);
    POSIX_ENSURE(s2n_constant_time_equals(first_half, conn->handshake.client_finished, verify_data_len),
            S2N_ERR_BAD_MESSAGE);

    uint8_t *second_half = s2n_stuffer_raw_read(extension, verify_data_len);
    POSIX_ENSURE_REF(second_half);
    POSIX_ENSURE(s2n_constant_time_equals(second_half, conn->handshake.server_finished, verify_data_len),
            S2N_ERR_BAD_MESSAGE);

    return S2N_SUCCESS;
}

/**
 * Note that this extension must also work for SSLv3:
 *= https://www.rfc-editor.org/rfc/rfc5746#4.5
 *# Clients that support SSLv3 and offer secure renegotiation (either via SCSV or
 *# "renegotiation_info") MUST accept the "renegotiation_info" extension
 *# from the server, even if the server version is {0x03, 0x00}, and
 *# behave as described in this specification.
 */
static int s2n_renegotiation_info_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    if (s2n_handshake_is_renegotiation(conn)) {
        POSIX_GUARD(s2n_renegotiation_info_recv_renegotiation(conn, extension));
    } else {
        POSIX_GUARD(s2n_renegotiation_info_recv_initial(conn, extension));
    }
    return S2N_SUCCESS;
}

static int s2n_renegotiation_info_if_missing(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    if (s2n_handshake_is_renegotiation(conn)) {
        /**
         *= https://www.rfc-editor.org/rfc/rfc5746#3.5
         *# o  When a ServerHello is received, the client MUST verify that the
         *#    "renegotiation_info" extension is present; if it is not, the
         *#    client MUST abort the handshake.
         */
        POSIX_BAIL(S2N_ERR_NO_RENEGOTIATION);
    } else {
        /**
         *= https://www.rfc-editor.org/rfc/rfc5746#3.4
         *# *  If the extension is not present, the server does not support
         *#    secure renegotiation; set secure_renegotiation flag to FALSE.
         *#    In this case, some clients may want to terminate the handshake
         *#    instead of continuing; see Section 4.1 for discussion.
         */
        conn->secure_renegotiation = false;

        /**
         *= https://www.rfc-editor.org/rfc/rfc5746#4.1
         *= type=exception
         *= reason=Avoid interoperability problems
         *# If clients wish to ensure that such attacks are impossible, they need
         *# to terminate the connection immediately upon failure to receive the
         *# extension without completing the handshake.  Such clients MUST
         *# generate a fatal "handshake_failure" alert prior to terminating the
         *# connection.  However, it is expected that many TLS servers that do
         *# not support renegotiation (and thus are not vulnerable) will not
         *# support this extension either, so in general, clients that implement
         *# this behavior will encounter interoperability problems.
         *
         * TODO: https://github.com/aws/s2n-tls/issues/3528
         */
        return S2N_SUCCESS;
    }
}

/* Old-style extension functions -- remove after extensions refactor is complete */

int s2n_recv_server_renegotiation_info_ext(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    return s2n_extension_recv(&s2n_server_renegotiation_info_extension, conn, extension);
}
