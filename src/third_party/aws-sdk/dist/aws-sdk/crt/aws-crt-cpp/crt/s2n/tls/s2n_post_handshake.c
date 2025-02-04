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

#include "error/s2n_errno.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_key_update.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_safety.h"

S2N_RESULT s2n_post_handshake_process(struct s2n_connection *conn, struct s2n_stuffer *in, uint8_t message_type)
{
    RESULT_ENSURE_REF(conn);

    switch (message_type) {
        case TLS_KEY_UPDATE:
            RESULT_GUARD_POSIX(s2n_key_update_recv(conn, in));
            break;
        case TLS_SERVER_NEW_SESSION_TICKET:
            RESULT_GUARD(s2n_tls13_server_nst_recv(conn, in));
            break;
        case TLS_HELLO_REQUEST:
            RESULT_GUARD(s2n_client_hello_request_recv(conn));
            break;
        case TLS_CERT_REQ:
            /*
             * s2n-tls does not support post-handshake authentication.
             *
             *= https://www.rfc-editor.org/rfc/rfc8446#section-4.6.2
             *# A client that receives a CertificateRequest message without having
             *# sent the "post_handshake_auth" extension MUST send an
             *# "unexpected_message" fatal alert.
             */
            RESULT_BAIL(S2N_ERR_BAD_MESSAGE);
        default:
            /* All other messages are unexpected */
            RESULT_BAIL(S2N_ERR_BAD_MESSAGE);
    }

    return S2N_RESULT_OK;
}

/*
 * Read a handshake message from conn->in.
 *
 * Handshake messages can be fragmented, meaning that a single message
 * may be split between multiple records. conn->in only holds a single
 * record at a time, so we may need to call this method multiple
 * times to construct the complete message. We store the partial message
 * in conn->post_handshake.in between calls.
 */
S2N_RESULT s2n_post_handshake_message_recv(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);

    struct s2n_stuffer *in = &conn->in;
    struct s2n_stuffer *message = &conn->post_handshake.in;
    uint8_t message_type = 0;
    uint32_t message_len = 0;

    /* We always start reading from the beginning of the message.
     * Reset the read progress, but keep the write progress since
     * there may already be a partial message stored in `message`.
     */
    RESULT_GUARD_POSIX(s2n_stuffer_reread(message));

    /* At minimum, the message stuffer needs to have enough space to read the header.
     * For small messages like KeyUpdate and HelloRequest, this is all the space we will need.
     */
    if (s2n_stuffer_is_freed(message)) {
        struct s2n_blob b = { 0 };
        RESULT_GUARD_POSIX(s2n_blob_init(&b, conn->post_handshake.header_in,
                sizeof(conn->post_handshake.header_in)));
        RESULT_GUARD_POSIX(s2n_stuffer_init(message, &b));
    }

    /* Try to copy the header into the message stuffer.
     * The message stuffer may already contain some or all of the header if
     * we have read fragments of this message from previous records.
     */
    if (s2n_stuffer_data_available(message) < TLS_HANDSHAKE_HEADER_LENGTH) {
        uint32_t remaining = TLS_HANDSHAKE_HEADER_LENGTH - s2n_stuffer_data_available(message);
        uint32_t to_read = MIN(remaining, s2n_stuffer_data_available(in));
        RESULT_GUARD_POSIX(s2n_stuffer_copy(in, message, to_read));
    }
    RESULT_ENSURE(s2n_stuffer_data_available(message) >= TLS_HANDSHAKE_HEADER_LENGTH, S2N_ERR_IO_BLOCKED);

    /* Parse the header */
    RESULT_GUARD(s2n_handshake_parse_header(message, &message_type, &message_len));
    RESULT_ENSURE(message_len == 0 || s2n_stuffer_data_available(in), S2N_ERR_IO_BLOCKED);
    RESULT_ENSURE(message_len <= S2N_MAXIMUM_HANDSHAKE_MESSAGE_LENGTH, S2N_ERR_BAD_MESSAGE);

    /* If the message body is not fragmented, just process it directly from conn->in.
     * This will be the most common case, and does not require us to allocate any new memory.
     */
    if (s2n_stuffer_data_available(message) == 0 && s2n_stuffer_data_available(in) >= message_len) {
        struct s2n_stuffer full_message = { 0 };
        struct s2n_blob full_message_blob = { 0 };
        RESULT_GUARD_POSIX(s2n_blob_init(&full_message_blob, s2n_stuffer_raw_read(in, message_len), message_len));
        RESULT_GUARD_POSIX(s2n_stuffer_init(&full_message, &full_message_blob));
        RESULT_GUARD_POSIX(s2n_stuffer_skip_write(&full_message, message_len));
        RESULT_GUARD(s2n_post_handshake_process(conn, &full_message, message_type));
        return S2N_RESULT_OK;
    }

    /* If the message body is fragmented, then the current fragment will be wiped from conn->in
     * in order to read the next record. So the message stuffer needs enough space to store
     * the full message as we reconstruct it from multiple records.
     * For large messages like NewSessionTicket, this will require allocating new memory.
     */
    if (s2n_stuffer_space_remaining(message) < message_len) {
        /* We want to avoid servers allocating memory in response to post-handshake messages
         * to avoid a potential DDOS / resource exhaustion attack.
         *
         * Currently, s2n-tls servers only support the KeyUpdate message,
         * which should never require additional memory to parse.
         */
        RESULT_ENSURE(conn->mode == S2N_CLIENT, S2N_ERR_BAD_MESSAGE);

        uint32_t total_size = message_len + TLS_HANDSHAKE_HEADER_LENGTH;
        if (message->alloced) {
            RESULT_GUARD_POSIX(s2n_stuffer_resize(message, total_size));
        } else {
            /* Manually convert our static stuffer to a growable stuffer */
            RESULT_GUARD_POSIX(s2n_stuffer_growable_alloc(message, total_size));
            RESULT_GUARD_POSIX(s2n_stuffer_write_bytes(message, conn->post_handshake.header_in, TLS_HANDSHAKE_HEADER_LENGTH));
            RESULT_GUARD_POSIX(s2n_stuffer_skip_read(message, TLS_HANDSHAKE_HEADER_LENGTH));
        }
    }

    /* Try to copy the message body into the message stuffer.
     * The message stuffer may already contain some of the message body if
     * we have already read fragments from previous records.
     */
    if (s2n_stuffer_data_available(message) < message_len) {
        uint32_t remaining = message_len - s2n_stuffer_data_available(message);
        uint32_t to_read = MIN(remaining, s2n_stuffer_data_available(in));
        RESULT_GUARD_POSIX(s2n_stuffer_copy(in, message, to_read));
    }
    RESULT_ENSURE(s2n_stuffer_data_available(message) == message_len, S2N_ERR_IO_BLOCKED);

    /* Now that the full message body is available, process it. */
    RESULT_GUARD(s2n_post_handshake_process(conn, message, message_type));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_post_handshake_recv(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    while (s2n_stuffer_data_available(&conn->in)) {
        RESULT_GUARD(s2n_post_handshake_message_recv(conn));
        RESULT_GUARD_POSIX(s2n_stuffer_wipe(&conn->post_handshake.in));
    }
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_post_handshake_write_records(struct s2n_connection *conn, s2n_blocked_status *blocked)
{
    struct s2n_stuffer *message = &conn->handshake.io;

    /* Flush any existing records before we write a new handshake record.
     * We do not support buffering multiple handshake records.
     */
    if (s2n_stuffer_data_available(message)) {
        RESULT_GUARD_POSIX(s2n_flush(conn, blocked));
    }

    RESULT_GUARD(s2n_handshake_message_send(conn, TLS_HANDSHAKE, blocked));
    RESULT_GUARD_POSIX(s2n_stuffer_wipe(message));
    return S2N_RESULT_OK;
}

int s2n_post_handshake_send(struct s2n_connection *conn, s2n_blocked_status *blocked)
{
    POSIX_ENSURE_REF(conn);

    /* Currently, we only support TLS1.3 post-handshake messages. */
    if (conn->actual_protocol_version < S2N_TLS13) {
        return S2N_SUCCESS;
    }

    POSIX_GUARD_RESULT(s2n_post_handshake_write_records(conn, blocked));

    POSIX_GUARD(s2n_key_update_send(conn, blocked));
    POSIX_GUARD_RESULT(s2n_tls13_server_nst_send(conn, blocked));

    POSIX_GUARD(s2n_stuffer_resize(&conn->handshake.io, 0));
    return S2N_SUCCESS;
}
