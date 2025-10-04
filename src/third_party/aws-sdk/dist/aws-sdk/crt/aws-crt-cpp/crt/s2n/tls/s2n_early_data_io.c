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

#include "tls/s2n_connection.h"
#include "tls/s2n_early_data.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

int s2n_end_of_early_data_send(struct s2n_connection *conn)
{
    if (conn->early_data_expected) {
        POSIX_GUARD(s2n_stuffer_wipe(&conn->handshake.io));
        POSIX_BAIL(S2N_ERR_EARLY_DATA_BLOCKED);
    }

    POSIX_GUARD_RESULT(s2n_connection_set_early_data_state(conn, S2N_END_OF_EARLY_DATA));
    return S2N_SUCCESS;
}

int s2n_end_of_early_data_recv(struct s2n_connection *conn)
{
    POSIX_ENSURE(!s2n_connection_is_quic_enabled(conn), S2N_ERR_BAD_MESSAGE);
    POSIX_GUARD_RESULT(s2n_connection_set_early_data_state(conn, S2N_END_OF_EARLY_DATA));
    return S2N_SUCCESS;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
 *# If the client attempts a 0-RTT handshake but the server
 *# rejects it, the server will generally not have the 0-RTT record
 *# protection keys and must instead use trial decryption (either with
 *# the 1-RTT handshake keys or by looking for a cleartext ClientHello in
 *# the case of a HelloRetryRequest) to find the first non-0-RTT message.
 */
bool s2n_early_data_is_trial_decryption_allowed(struct s2n_connection *conn, uint8_t record_type)
{
    return conn && (conn->early_data_state == S2N_EARLY_DATA_REJECTED)
            && record_type == TLS_APPLICATION_DATA
            /* Only servers receive early data. */
            && (conn->mode == S2N_SERVER)
            /* Early data is only expected during the handshake. */
            && (s2n_conn_get_current_message_type(conn) != APPLICATION_DATA);
}

static bool s2n_is_early_data_io(struct s2n_connection *conn)
{
    if (s2n_conn_get_current_message_type(conn) == APPLICATION_DATA) {
        return false;
    }

    /* It would be more accurate to not include this check.
     * However, before the early data feature was added, s2n_send and s2n_recv
     * did not verify that they were being called after a complete handshake.
     * Enforcing that broke several S2N tests, and might have broken customers too.
     *
     * Therefore, only consider this early data if the customer has indicated that
     * they are aware of early data, either because early data is currently expected
     * or early data is in a state that indicates that early data was previously expected.
     */
    if (conn->early_data_expected
            || (conn->mode == S2N_CLIENT && conn->early_data_state == S2N_EARLY_DATA_REQUESTED)
            || conn->early_data_state == S2N_EARLY_DATA_ACCEPTED
            || conn->early_data_state == S2N_END_OF_EARLY_DATA) {
        return true;
    }
    return false;
}

S2N_RESULT s2n_early_data_record_bytes(struct s2n_connection *conn, ssize_t data_len)
{
    RESULT_ENSURE_REF(conn);
    if (data_len < 0 || !s2n_is_early_data_io(conn)) {
        return S2N_RESULT_OK;
    }

    /* Ensure the bytes read are within the bounds of what we can actually record. */
    if ((size_t) data_len > (UINT64_MAX - conn->early_data_bytes)) {
        conn->early_data_bytes = UINT64_MAX;
        RESULT_BAIL(S2N_ERR_INTEGER_OVERFLOW);
    }

    /* Record the early data bytes read, even if they exceed the max_early_data_size.
     * This will ensure that if this method is called again, it will fail again:
     * Once we receive too many bytes, we can't proceed with the connection. */
    conn->early_data_bytes += data_len;

    uint32_t max_early_data_size = 0;
    RESULT_GUARD_POSIX(s2n_connection_get_max_early_data_size(conn, &max_early_data_size));
    RESULT_ENSURE(conn->early_data_bytes <= max_early_data_size, S2N_ERR_MAX_EARLY_DATA_SIZE);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_early_data_validate_send(struct s2n_connection *conn, uint32_t bytes_to_send)
{
    RESULT_ENSURE_REF(conn);
    if (!s2n_is_early_data_io(conn)) {
        return S2N_RESULT_OK;
    }

    RESULT_ENSURE(conn->early_data_expected, S2N_ERR_EARLY_DATA_NOT_ALLOWED);
    RESULT_ENSURE(conn->mode == S2N_CLIENT, S2N_ERR_EARLY_DATA_NOT_ALLOWED);
    RESULT_ENSURE(conn->early_data_state == S2N_EARLY_DATA_REQUESTED
                    || conn->early_data_state == S2N_EARLY_DATA_ACCEPTED,
            S2N_ERR_EARLY_DATA_NOT_ALLOWED);

    uint32_t allowed_early_data_size = 0;
    RESULT_GUARD_POSIX(s2n_connection_get_remaining_early_data_size(conn, &allowed_early_data_size));
    RESULT_ENSURE(bytes_to_send <= allowed_early_data_size, S2N_ERR_MAX_EARLY_DATA_SIZE);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_early_data_validate_recv(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    if (!s2n_is_early_data_io(conn)) {
        return S2N_RESULT_OK;
    }

    RESULT_ENSURE(conn->early_data_expected, S2N_ERR_EARLY_DATA_NOT_ALLOWED);
    RESULT_ENSURE(conn->mode == S2N_SERVER, S2N_ERR_EARLY_DATA_NOT_ALLOWED);
    RESULT_ENSURE(conn->early_data_state == S2N_EARLY_DATA_ACCEPTED, S2N_ERR_EARLY_DATA_NOT_ALLOWED);
    RESULT_ENSURE(s2n_conn_get_current_message_type(conn) == END_OF_EARLY_DATA, S2N_ERR_EARLY_DATA_NOT_ALLOWED);
    return S2N_RESULT_OK;
}

static bool s2n_early_data_can_continue(struct s2n_connection *conn)
{
    uint32_t remaining_early_data_size = 0;
    return s2n_connection_get_remaining_early_data_size(conn, &remaining_early_data_size) >= S2N_SUCCESS
            && remaining_early_data_size > 0;
}

S2N_RESULT s2n_send_early_data_impl(struct s2n_connection *conn, const uint8_t *data, ssize_t data_len_signed,
        ssize_t *data_sent, s2n_blocked_status *blocked)
{
    RESULT_ENSURE_GTE(data_len_signed, 0);
    size_t data_len = data_len_signed;
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(blocked);
    *blocked = S2N_NOT_BLOCKED;
    RESULT_ENSURE_REF(data_sent);
    *data_sent = 0;

    RESULT_ENSURE(conn->mode == S2N_CLIENT, S2N_ERR_SERVER_MODE);
    RESULT_ENSURE(s2n_connection_supports_tls13(conn), S2N_ERR_PROTOCOL_VERSION_UNSUPPORTED);

    if (!s2n_early_data_can_continue(conn)) {
        return S2N_RESULT_OK;
    }

    /* Attempt to make progress in the handshake even if s2n_send eventually fails.
     * We only care about the result of this call if it would prevent us from calling s2n_send. */
    int negotiate_result = s2n_negotiate(conn, blocked);
    if (negotiate_result < S2N_SUCCESS) {
        if (s2n_error_get_type(s2n_errno) != S2N_ERR_T_BLOCKED) {
            RESULT_GUARD_POSIX(negotiate_result);
        } else if (*blocked != S2N_BLOCKED_ON_EARLY_DATA && *blocked != S2N_BLOCKED_ON_READ) {
            RESULT_GUARD_POSIX(negotiate_result);
        }
    }
    /* Save the error status for later */
    int negotiate_error = s2n_errno;
    s2n_blocked_status negotiate_blocked = *blocked;

    /* Attempt to send the early data.
     * We only care about the result of this call if it fails. */
    uint32_t early_data_to_send = 0;
    RESULT_GUARD_POSIX(s2n_connection_get_remaining_early_data_size(conn, &early_data_to_send));
    early_data_to_send = MIN(data_len, early_data_to_send);
    if (early_data_to_send) {
        ssize_t send_result = s2n_send(conn, data, early_data_to_send, blocked);
        RESULT_GUARD_POSIX(send_result);
        *data_sent = send_result;
    }
    *blocked = S2N_NOT_BLOCKED;

    /* Since the send was successful, report the result of the original negotiate call.
     * If we got this far, the result must have been success or a blocking error. */
    if (negotiate_result < S2N_SUCCESS) {
        RESULT_ENSURE_EQ(s2n_error_get_type(negotiate_error), S2N_ERR_T_BLOCKED);
        if (negotiate_blocked == S2N_BLOCKED_ON_EARLY_DATA) {
            return S2N_RESULT_OK;
        } else if (s2n_early_data_can_continue(conn)) {
            *blocked = negotiate_blocked;
            RESULT_BAIL(negotiate_error);
        } else {
            return S2N_RESULT_OK;
        }
    }
    return S2N_RESULT_OK;
}

int s2n_send_early_data(struct s2n_connection *conn, const uint8_t *data, ssize_t data_len,
        ssize_t *data_sent, s2n_blocked_status *blocked)
{
    POSIX_ENSURE_REF(conn);

    /* Calling this method indicates that we expect early data. */
    POSIX_GUARD(s2n_connection_set_early_data_expected(conn));

    s2n_result result = s2n_send_early_data_impl(conn, data, data_len, data_sent, blocked);

    /* Unless s2n_send_early_data is called again (undoing this), we are done sending early data.
     * If s2n_negotiate is called next, we could send the EndOfEarlyData message. */
    POSIX_GUARD(s2n_connection_set_end_of_early_data(conn));

    POSIX_GUARD_RESULT(result);
    return S2N_SUCCESS;
}

S2N_RESULT s2n_recv_early_data_impl(struct s2n_connection *conn, uint8_t *data, ssize_t max_data_len,
        ssize_t *data_received, s2n_blocked_status *blocked)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(blocked);
    *blocked = S2N_NOT_BLOCKED;
    RESULT_ENSURE_REF(data_received);
    *data_received = 0;

    RESULT_ENSURE(conn->mode == S2N_SERVER, S2N_ERR_CLIENT_MODE);

    if (!s2n_early_data_can_continue(conn)) {
        return S2N_RESULT_OK;
    }

    int negotiate_result = S2N_SUCCESS;
    while ((negotiate_result = s2n_negotiate(conn, blocked)) != S2N_SUCCESS) {
        if (s2n_error_get_type(s2n_errno) != S2N_ERR_T_BLOCKED) {
            RESULT_GUARD_POSIX(negotiate_result);
        } else if (max_data_len <= *data_received) {
            RESULT_GUARD_POSIX(negotiate_result);
        } else if (*blocked != S2N_BLOCKED_ON_EARLY_DATA) {
            if (s2n_early_data_can_continue(conn)) {
                RESULT_GUARD_POSIX(negotiate_result);
            } else {
                *blocked = S2N_NOT_BLOCKED;
                return S2N_RESULT_OK;
            }
        }

        ssize_t recv_result = s2n_recv(conn, data + *data_received,
                max_data_len - *data_received, blocked);
        RESULT_GUARD_POSIX(recv_result);
        *data_received += recv_result;
    }
    return S2N_RESULT_OK;
}

int s2n_recv_early_data(struct s2n_connection *conn, uint8_t *data, ssize_t max_data_len,
        ssize_t *data_received, s2n_blocked_status *blocked)
{
    /* Calling this method indicates that we expect early data. */
    POSIX_GUARD(s2n_connection_set_early_data_expected(conn));

    s2n_result result = s2n_recv_early_data_impl(conn, data, max_data_len, data_received, blocked);

    /* Unless s2n_recv_early_data is called again (undoing this), we are done accepting early data. */
    POSIX_GUARD(s2n_connection_set_end_of_early_data(conn));

    POSIX_GUARD_RESULT(result);
    return S2N_SUCCESS;
}
