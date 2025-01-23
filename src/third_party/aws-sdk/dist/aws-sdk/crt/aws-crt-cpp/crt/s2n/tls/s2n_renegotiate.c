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

#include "tls/s2n_renegotiate.h"

#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_connection.h"
#include "utils/s2n_safety.h"

/* We don't want to introduce a new blocked status for renegotiation
 * because that would potentially require applications to update their
 * blocked status handling logic for no reason.
 *
 * It is impossible to use both early data and renegotiation for the same handshake,
 * and both are just application data received during the handshake.
 * Therefore, we will alias S2N_BLOCKED_ON_EARLY_DATA for reuse with renegotiation.
 */
const s2n_blocked_status S2N_BLOCKED_ON_APPLICATION_DATA = S2N_BLOCKED_ON_EARLY_DATA;

S2N_RESULT s2n_renegotiate_validate(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE(conn->mode == S2N_CLIENT, S2N_ERR_NO_RENEGOTIATION);
    RESULT_ENSURE(conn->secure_renegotiation, S2N_ERR_NO_RENEGOTIATION);
    RESULT_ENSURE(conn->handshake.renegotiation, S2N_ERR_INVALID_STATE);
    RESULT_ENSURE(!conn->ktls_send_enabled, S2N_ERR_KTLS_RENEG);
    RESULT_ENSURE(!conn->ktls_recv_enabled, S2N_ERR_KTLS_RENEG);
    return S2N_RESULT_OK;
}

/*
 * Prepare a connection to be reused for a second handshake.
 *
 * s2n-tls was not originally designed to support renegotiation.
 * s2n_connection is a very large structure with configuration fields set by the application
 * mixed in with internal state fields set by the handshake.
 * Ensuring all existing internal state fields (and any new fields added) are safe to reuse for
 * a renegotiated handshake would be extremely prone to errors.
 * For safety, we instead wipe the entire connection and only restore fields we know we need
 * in order to continue sending and receiving encrypted data.
 *
 * Any configuration fields set by the application will need to be set by the application again.
 */
int s2n_renegotiate_wipe(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    /* We use this method to reset both clients and servers when testing.
     * However, outside of tests, it should only be called for client connections
     * because we only support renegotiation for clients.
     */
    POSIX_ENSURE(conn->mode == S2N_CLIENT || s2n_in_unit_test(), S2N_ERR_NO_RENEGOTIATION);

    /* Best effort check for pending input or output data.
     * This method should not be called until the application has stopped sending and receiving.
     * Saving partial read or parital write state would complicate this problem.
     */
    POSIX_ENSURE(s2n_stuffer_data_available(&conn->header_in) == 0, S2N_ERR_INVALID_STATE);
    POSIX_ENSURE(s2n_stuffer_data_available(&conn->in) == 0, S2N_ERR_INVALID_STATE);
    POSIX_ENSURE(s2n_stuffer_data_available(&conn->out) == 0, S2N_ERR_INVALID_STATE);

    /* buffer_in might contain data needed to read the next records. */
    DEFER_CLEANUP(struct s2n_stuffer buffer_in = conn->buffer_in, s2n_stuffer_free);
    conn->buffer_in = (struct s2n_stuffer){ 0 };
    POSIX_GUARD(s2n_stuffer_growable_alloc(&conn->buffer_in, 0));

    /* Save the crypto parameters.
     * We need to continue encrypting / decrypting with the old secure parameters.
     */
    DEFER_CLEANUP(struct s2n_crypto_parameters *secure_crypto_params = conn->secure,
            s2n_crypto_parameters_free);
    conn->secure = NULL;

    /* Save the fragment length so we continue properly fragmenting our records
     * until a new fragment length is chosen.
     */
    uint16_t max_frag_len = conn->max_outgoing_fragment_length;

    /* Save the protocol versions.
     * Various checks when sending and receiving records rely on the protocol version. */
    uint8_t actual_protocol_version = conn->actual_protocol_version;
    uint8_t server_protocol_version = conn->server_protocol_version;
    uint8_t client_protocol_version = conn->client_protocol_version;
    POSIX_ENSURE(actual_protocol_version < S2N_TLS13, S2N_ERR_PROTOCOL_VERSION_UNSUPPORTED);

    /* Save byte tracking.
     * This isn't strictly necessary, but potentially useful. */
    uint64_t wire_bytes_in = conn->wire_bytes_in;
    uint64_t wire_bytes_out = conn->wire_bytes_out;

    /* Save io settings */
    bool send_managed = conn->managed_send_io;
    s2n_send_fn *send_fn = conn->send;
    void *send_ctx = conn->send_io_context;
    bool recv_managed = conn->managed_recv_io;
    s2n_recv_fn *recv_fn = conn->recv;
    void *recv_ctx = conn->recv_io_context;
    /* Treat IO as unmanaged, since we don't want to clean it up yet */
    conn->managed_send_io = false;
    conn->managed_recv_io = false;

    /* Save the secure_renegotiation flag.
     * This flag should always be true, since we don't support insecure renegotiation,
     * but copying its value seems safer than just setting it to 'true'.
     */
    bool secure_renegotiation = conn->secure_renegotiation;
    POSIX_ENSURE(secure_renegotiation, S2N_ERR_NO_RENEGOTIATION);

    /* Save the finished data.
     * This is required for the renegotiate_info extension on the new handshake.
     */
    uint8_t finished_len = conn->handshake.finished_len;
    uint8_t client_finished[sizeof(conn->handshake.client_finished)] = { 0 };
    POSIX_CHECKED_MEMCPY(client_finished, conn->handshake.client_finished, finished_len);
    uint8_t server_finished[sizeof(conn->handshake.server_finished)] = { 0 };
    POSIX_CHECKED_MEMCPY(server_finished, conn->handshake.server_finished, finished_len);

    POSIX_GUARD(s2n_connection_wipe(conn));

    /* Setup the new crypto parameters.
     * The new handshake will negotiate new secure crypto parameters,
     * so the current secure crypto parameters become the initial crypto parameters.
     */
    POSIX_GUARD_RESULT(s2n_crypto_parameters_free(&conn->initial));
    conn->initial = secure_crypto_params;
    ZERO_TO_DISABLE_DEFER_CLEANUP(secure_crypto_params);
    conn->client = conn->initial;
    conn->server = conn->initial;

    /* Restore saved values */
    POSIX_GUARD_RESULT(s2n_connection_set_max_fragment_length(conn, max_frag_len));
    POSIX_CHECKED_MEMCPY(conn->handshake.client_finished, client_finished, finished_len);
    POSIX_CHECKED_MEMCPY(conn->handshake.server_finished, server_finished, finished_len);
    conn->handshake.finished_len = finished_len;
    conn->actual_protocol_version = actual_protocol_version;
    conn->server_protocol_version = server_protocol_version;
    conn->client_protocol_version = client_protocol_version;
    conn->wire_bytes_in = wire_bytes_in;
    conn->wire_bytes_out = wire_bytes_out;
    conn->managed_send_io = send_managed;
    conn->send = send_fn;
    conn->send_io_context = send_ctx;
    conn->managed_recv_io = recv_managed;
    conn->recv = recv_fn;
    conn->recv_io_context = recv_ctx;
    conn->secure_renegotiation = secure_renegotiation;
    conn->buffer_in = buffer_in;
    ZERO_TO_DISABLE_DEFER_CLEANUP(buffer_in);

    conn->handshake.renegotiation = true;
    return S2N_SUCCESS;
}

static S2N_RESULT s2n_renegotiate_read_app_data(struct s2n_connection *conn, uint8_t *app_data_buf, ssize_t app_data_buf_size,
        ssize_t *app_data_size, s2n_blocked_status *blocked)
{
    RESULT_ENSURE_REF(blocked);

    ssize_t r = s2n_recv(conn, app_data_buf, app_data_buf_size, blocked);
    RESULT_GUARD_POSIX(r);
    *app_data_size = r;

    *blocked = S2N_BLOCKED_ON_APPLICATION_DATA;
    RESULT_BAIL(S2N_ERR_APP_DATA_BLOCKED);
}

int s2n_renegotiate(struct s2n_connection *conn, uint8_t *app_data_buf, ssize_t app_data_buf_size,
        ssize_t *app_data_size, s2n_blocked_status *blocked)
{
    POSIX_GUARD_RESULT(s2n_renegotiate_validate(conn));

    POSIX_ENSURE_REF(app_data_size);
    *app_data_size = 0;

    /* If there is outstanding application data, pass it back to the application.
     * We can't read the next handshake record until we drain the buffer.
     */
    if (s2n_peek(conn) > 0) {
        POSIX_GUARD_RESULT(s2n_renegotiate_read_app_data(conn,
                app_data_buf, app_data_buf_size, app_data_size, blocked));
    }

    int result = s2n_negotiate(conn, blocked);

    /* If we encounter application data while reading handshake records,
     * pass it back to the application.
     */
    if (result != S2N_SUCCESS && s2n_errno == S2N_ERR_APP_DATA_BLOCKED) {
        POSIX_GUARD_RESULT(s2n_renegotiate_read_app_data(conn,
                app_data_buf, app_data_buf_size, app_data_size, blocked));
    }

    return result;
}
