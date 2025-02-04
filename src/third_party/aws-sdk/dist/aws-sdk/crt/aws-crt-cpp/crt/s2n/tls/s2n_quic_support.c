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

#include "tls/s2n_quic_support.h"

#include "tls/s2n_connection.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls13.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

/* When reading and writing records with TCP, S2N sets its input and output buffers
 * to the maximum record fragment size to prevent resizing those buffers later.
 *
 * However, because S2N with QUIC reads and writes messages instead of records,
 * the "maximum size" for the input and output buffers would be the maximum message size: 64k.
 * Since most messages are MUCH smaller than that (<3k), setting the buffer that large is wasteful.
 *
 * Instead, we intentionally choose a smaller size and accept that an abnormally large message
 * could cause the buffer to resize. */
#define S2N_EXPECTED_QUIC_MESSAGE_SIZE S2N_DEFAULT_FRAGMENT_LENGTH

S2N_RESULT s2n_read_in_bytes(struct s2n_connection *conn, struct s2n_stuffer *output, uint32_t length);

int s2n_config_enable_quic(struct s2n_config *config)
{
    POSIX_ENSURE_REF(config);
    config->quic_enabled = true;
    return S2N_SUCCESS;
}

int s2n_connection_enable_quic(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_GUARD_RESULT(s2n_connection_validate_tls13_support(conn));
    /* QUIC support is not currently compatible with recv_buffering */
    POSIX_ENSURE(!conn->recv_buffering, S2N_ERR_INVALID_STATE);
    conn->quic_enabled = true;
    return S2N_SUCCESS;
}

bool s2n_connection_is_quic_enabled(struct s2n_connection *conn)
{
    return (conn && conn->quic_enabled) || (conn && conn->config && conn->config->quic_enabled);
}

bool s2n_connection_are_session_tickets_enabled(struct s2n_connection *conn)
{
    return conn && conn->config && conn->config->use_tickets;
}

int s2n_connection_set_quic_transport_parameters(struct s2n_connection *conn,
        const uint8_t *data_buffer, uint16_t data_len)
{
    POSIX_ENSURE_REF(conn);

    POSIX_GUARD(s2n_free(&conn->our_quic_transport_parameters));
    POSIX_GUARD(s2n_alloc(&conn->our_quic_transport_parameters, data_len));
    POSIX_CHECKED_MEMCPY(conn->our_quic_transport_parameters.data, data_buffer, data_len);

    return S2N_SUCCESS;
}

int s2n_connection_get_quic_transport_parameters(struct s2n_connection *conn,
        const uint8_t **data_buffer, uint16_t *data_len)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(data_buffer);
    POSIX_ENSURE_REF(data_len);

    *data_buffer = conn->peer_quic_transport_parameters.data;
    *data_len = conn->peer_quic_transport_parameters.size;

    return S2N_SUCCESS;
}

int s2n_connection_set_secret_callback(struct s2n_connection *conn, s2n_secret_cb cb_func, void *ctx)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(cb_func);

    conn->secret_cb = cb_func;
    conn->secret_cb_context = ctx;

    return S2N_SUCCESS;
}

/* Currently we need an API that quic can call to process post-handshake messages. Ideally
 * we could re-use the s2n_recv API but that function needs to be refactored to support quic.
 * For now we just call this API.
 */
int s2n_recv_quic_post_handshake_message(struct s2n_connection *conn, s2n_blocked_status *blocked)
{
    POSIX_ENSURE_REF(conn);

    *blocked = S2N_BLOCKED_ON_READ;

    uint8_t message_type = 0;
    /* This function uses the stuffer conn->handshake.io to read in the header. This stuffer is also used 
     * for sending post-handshake messages. This could cause a concurrency issue if we start both sending
     * and receiving post-handshake messages while quic is enabled. Currently there's no post-handshake
     * message that is both sent and received in quic (servers only send session tickets
     * and clients only receive session tickets.) Therefore it is safe for us
     * to use the stuffer here.
     */
    POSIX_GUARD_RESULT(s2n_quic_read_handshake_message(conn, &message_type));

    /* The only post-handshake messages we support from QUIC currently are session tickets */
    POSIX_ENSURE(message_type == TLS_SERVER_NEW_SESSION_TICKET, S2N_ERR_UNSUPPORTED_WITH_QUIC);
    POSIX_GUARD_RESULT(s2n_post_handshake_process(conn, &conn->in, message_type));

    *blocked = S2N_NOT_BLOCKED;

    return S2N_SUCCESS;
}

/* When using QUIC, S2N reads unencrypted handshake messages instead of encrypted records.
 * This method sets up the S2N input buffers to match the results of using s2n_read_full_record.
 */
S2N_RESULT s2n_quic_read_handshake_message(struct s2n_connection *conn, uint8_t *message_type)
{
    RESULT_ENSURE_REF(conn);
    /* The use of handshake.io here would complicate recv_buffering, and there's
     * no real use case for recv_buffering when QUIC is handling the IO.
     */
    RESULT_ENSURE(!conn->recv_buffering, S2N_ERR_INVALID_STATE);

    /* Allocate stuffer space now so that we don't have to realloc later in the handshake. */
    RESULT_GUARD_POSIX(s2n_stuffer_resize_if_empty(&conn->buffer_in, S2N_EXPECTED_QUIC_MESSAGE_SIZE));

    RESULT_GUARD(s2n_read_in_bytes(conn, &conn->handshake.io, TLS_HANDSHAKE_HEADER_LENGTH));

    uint32_t message_len = 0;
    RESULT_GUARD(s2n_handshake_parse_header(&conn->handshake.io, message_type, &message_len));
    RESULT_GUARD_POSIX(s2n_stuffer_reread(&conn->handshake.io));

    RESULT_ENSURE(message_len < S2N_MAXIMUM_HANDSHAKE_MESSAGE_LENGTH, S2N_ERR_BAD_MESSAGE);
    RESULT_GUARD(s2n_read_in_bytes(conn, &conn->buffer_in, message_len));

    /* Although we call s2n_read_in_bytes, recv_greedy is always disabled for quic.
     * Therefore buffer_in will always contain exactly message_len bytes of data.
     * So we don't need to handle the possibility of extra data in buffer_in.
     */
    RESULT_ENSURE_EQ(s2n_stuffer_data_available(&conn->buffer_in), message_len);
    RESULT_GUARD(s2n_recv_in_init(conn, message_len, message_len));
    return S2N_RESULT_OK;
}

/* When using QUIC, S2N writes unencrypted handshake messages instead of encrypted records.
 * This method sets up the S2N output buffer to match the result of using s2n_record_write.
 */
S2N_RESULT s2n_quic_write_handshake_message(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);

    /* Allocate stuffer space now so that we don't have to realloc later in the handshake. */
    RESULT_GUARD_POSIX(s2n_stuffer_resize_if_empty(&conn->out, S2N_EXPECTED_QUIC_MESSAGE_SIZE));

    RESULT_GUARD_POSIX(s2n_stuffer_copy(&conn->handshake.io, &conn->out,
            s2n_stuffer_data_available(&conn->handshake.io)));
    return S2N_RESULT_OK;
}
