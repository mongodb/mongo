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

#include "tls/extensions/s2n_client_renegotiation_info.h"

#include <stdint.h>
#include <sys/param.h>

#include "tls/s2n_tls.h"
#include "utils/s2n_safety.h"

static int s2n_client_renegotiation_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_client_renegotiation_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);
static bool s2n_client_renegotiation_should_send(struct s2n_connection *conn);
static int s2n_client_renegotiation_if_missing(struct s2n_connection *conn);

const s2n_extension_type s2n_client_renegotiation_info_extension = {
    .iana_value = TLS_EXTENSION_RENEGOTIATION_INFO,
    .is_response = false,
    .send = s2n_client_renegotiation_send,
    .recv = s2n_client_renegotiation_recv,
    .should_send = s2n_client_renegotiation_should_send,
    .if_missing = s2n_client_renegotiation_if_missing,
};

/**
 *= https://www.rfc-editor.org/rfc/rfc5746#3.5
 *# o  The client MUST include the "renegotiation_info" extension in the
 *#    ClientHello
 */
static bool s2n_client_renegotiation_should_send(struct s2n_connection *conn)
{
    return conn && s2n_handshake_is_renegotiation(conn);
}

/**
 *= https://www.rfc-editor.org/rfc/rfc5746#3.5
 *# o  The client MUST include the "renegotiation_info" extension in the
 *#    ClientHello, containing the saved client_verify_data.
 */
static int s2n_client_renegotiation_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);

    /**
     *= https://www.rfc-editor.org/rfc/rfc5746#3.5
     *# This text applies if the connection's "secure_renegotiation" flag is
     *# set to TRUE (if it is set to FALSE, see Section 4.2).
     */
    POSIX_ENSURE(conn->secure_renegotiation, S2N_ERR_NO_RENEGOTIATION);

    uint8_t renegotiated_connection_len = conn->handshake.finished_len;
    POSIX_ENSURE_GT(renegotiated_connection_len, 0);
    POSIX_GUARD(s2n_stuffer_write_uint8(out, renegotiated_connection_len));
    POSIX_GUARD(s2n_stuffer_write_bytes(out, conn->handshake.client_finished, renegotiated_connection_len));

    return S2N_SUCCESS;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc5746#3.6
 *# o  The server MUST check if the "renegotiation_info" extension is
 *# included in the ClientHello.
 *
 * Note that this extension must also work for SSLv3:
 *= https://www.rfc-editor.org/rfc/rfc5746#4.5
 *# TLS servers that support secure renegotiation and support SSLv3 MUST accept SCSV or the
 *# "renegotiation_info" extension and respond as described in this
 *# specification even if the offered client version is {0x03, 0x00}.
 */
static int s2n_client_renegotiation_recv_initial(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    /**
     *= https://www.rfc-editor.org/rfc/rfc5746#3.6
     *# The server MUST then verify
     *# that the length of the "renegotiated_connection" field is zero,
     *# and if it is not, MUST abort the handshake.
     */
    uint8_t renegotiated_connection_len = 0;
    POSIX_GUARD(s2n_stuffer_read_uint8(extension, &renegotiated_connection_len));
    POSIX_ENSURE(s2n_stuffer_data_available(extension) == 0, S2N_ERR_NON_EMPTY_RENEGOTIATION_INFO);
    POSIX_ENSURE(renegotiated_connection_len == 0, S2N_ERR_NON_EMPTY_RENEGOTIATION_INFO);

    /**
     *= https://www.rfc-editor.org/rfc/rfc5746#3.6
     *# If the extension is present, set secure_renegotiation flag to TRUE.
     */
    conn->secure_renegotiation = 1;

    return S2N_SUCCESS;
}

static int s2n_client_renegotiation_recv_renegotiation(struct s2n_connection *conn, struct s2n_stuffer *extension)
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
     *# o  The server MUST verify that the value of the
     *#    "renegotiated_connection" field is equal to the saved
     *#    client_verify_data value; if it is not, the server MUST abort the
     *#    handshake.
     */

    uint8_t verify_data_len = conn->handshake.finished_len;
    POSIX_ENSURE_GT(verify_data_len, 0);

    uint8_t renegotiated_connection_len = 0;
    POSIX_GUARD(s2n_stuffer_read_uint8(extension, &renegotiated_connection_len));
    POSIX_ENSURE(verify_data_len == renegotiated_connection_len, S2N_ERR_BAD_MESSAGE);

    uint8_t *renegotiated_connection = s2n_stuffer_raw_read(extension, verify_data_len);
    POSIX_ENSURE_REF(renegotiated_connection);
    POSIX_ENSURE(s2n_constant_time_equals(renegotiated_connection, conn->handshake.client_finished, verify_data_len),
            S2N_ERR_BAD_MESSAGE);

    return S2N_SUCCESS;
}

static int s2n_client_renegotiation_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    if (s2n_handshake_is_renegotiation(conn)) {
        POSIX_GUARD(s2n_client_renegotiation_recv_renegotiation(conn, extension));
    } else {
        POSIX_GUARD(s2n_client_renegotiation_recv_initial(conn, extension));
    }
    POSIX_ENSURE(s2n_stuffer_data_available(extension) == 0, S2N_ERR_BAD_MESSAGE);
    return S2N_SUCCESS;
}

static int s2n_client_renegotiation_if_missing(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    if (s2n_handshake_is_renegotiation(conn)) {
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
         *# o  The server MUST verify that the "renegotiation_info" extension is
         *#     present; if it is not, the server MUST abort the handshake.
         */
        POSIX_BAIL(S2N_ERR_MISSING_EXTENSION);
    } else {
        /**
         *= https://www.rfc-editor.org/rfc/rfc5746#3.6
         *# o  If neither the TLS_EMPTY_RENEGOTIATION_INFO_SCSV SCSV nor the
         *#    "renegotiation_info" extension was included, set the
         *#    secure_renegotiation flag to FALSE.  In this case, some servers
         *#    may want to terminate the handshake instead of continuing
         *
         * We do not terminate the handshake for compatibility reasons.
         * See https://github.com/aws/s2n-tls/issues/3528
         */
        conn->secure_renegotiation = false;
        return S2N_SUCCESS;
    }
}
