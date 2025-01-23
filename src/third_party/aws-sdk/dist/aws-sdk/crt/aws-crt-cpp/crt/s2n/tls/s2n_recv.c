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

/* Use usleep */
#define _XOPEN_SOURCE 500
#include <errno.h>
#include <unistd.h>

#include "api/s2n.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_alerts.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_handshake.h"
#include "tls/s2n_ktls.h"
#include "tls/s2n_post_handshake.h"
#include "tls/s2n_record.h"
#include "tls/s2n_resume.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_io.h"
#include "utils/s2n_safety.h"
#include "utils/s2n_socket.h"

S2N_RESULT s2n_recv_in_init(struct s2n_connection *conn, uint32_t written, uint32_t total)
{
    RESULT_ENSURE_REF(conn);

    /* If we're going to initialize conn->in to point to more memory than
     * is actually readable, make sure that the additional memory exists.
     */
    RESULT_ENSURE_LTE(written, total);
    uint32_t remaining = total - written;
    RESULT_ENSURE_LTE(remaining, s2n_stuffer_space_remaining(&conn->buffer_in));

    uint8_t *data = s2n_stuffer_raw_read(&conn->buffer_in, written);
    RESULT_ENSURE_REF(data);
    RESULT_GUARD_POSIX(s2n_stuffer_free(&conn->in));
    RESULT_GUARD_POSIX(s2n_blob_init(&conn->in.blob, data, total));
    RESULT_GUARD_POSIX(s2n_stuffer_skip_write(&conn->in, written));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_read_in_bytes(struct s2n_connection *conn, struct s2n_stuffer *output, uint32_t length)
{
    while (s2n_stuffer_data_available(output) < length) {
        uint32_t remaining = length - s2n_stuffer_data_available(output);
        if (conn->recv_buffering) {
            remaining = MAX(remaining, s2n_stuffer_space_remaining(output));
        }
        errno = 0;
        int r = s2n_connection_recv_stuffer(output, conn, remaining);
        if (r == 0) {
            s2n_atomic_flag_set(&conn->read_closed);
        }
        RESULT_GUARD(s2n_io_check_read_result(r));
        conn->wire_bytes_in += r;
    }

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_recv_buffer_in(struct s2n_connection *conn, size_t min_size)
{
    RESULT_GUARD_POSIX(s2n_stuffer_resize_if_empty(&conn->buffer_in, S2N_LARGE_FRAGMENT_LENGTH));
    uint32_t buffer_in_available = s2n_stuffer_data_available(&conn->buffer_in);
    if (buffer_in_available < min_size) {
        uint32_t remaining = min_size - buffer_in_available;
        if (s2n_stuffer_space_remaining(&conn->buffer_in) < remaining) {
            RESULT_GUARD_POSIX(s2n_stuffer_shift(&conn->buffer_in));
        }
        RESULT_GUARD(s2n_read_in_bytes(conn, &conn->buffer_in, min_size));
    }
    return S2N_RESULT_OK;
}

int s2n_read_full_record(struct s2n_connection *conn, uint8_t *record_type, int *isSSLv2)
{
    *isSSLv2 = 0;

    if (conn->ktls_recv_enabled) {
        return s2n_ktls_read_full_record(conn, record_type);
    }

    /* If the record has already been decrypted, then leave it alone */
    if (conn->in_status == PLAINTEXT) {
        /* Only application data packets count as plaintext */
        *record_type = TLS_APPLICATION_DATA;
        return S2N_SUCCESS;
    }

    /* Read the record until we at least have a header */
    POSIX_GUARD(s2n_stuffer_reread(&conn->header_in));
    uint32_t header_available = s2n_stuffer_data_available(&conn->header_in);
    if (header_available < S2N_TLS_RECORD_HEADER_LENGTH) {
        uint32_t header_remaining = S2N_TLS_RECORD_HEADER_LENGTH - header_available;
        s2n_result ret = s2n_recv_buffer_in(conn, header_remaining);
        uint32_t header_read = MIN(header_remaining, s2n_stuffer_data_available(&conn->buffer_in));
        POSIX_GUARD(s2n_stuffer_copy(&conn->buffer_in, &conn->header_in, header_read));
        POSIX_GUARD_RESULT(ret);
    }

    uint16_t fragment_length = 0;

    /* If the first bit is set then this is an SSLv2 record */
    if (conn->header_in.blob.data[0] & S2N_TLS_SSLV2_HEADER_FLAG) {
        *isSSLv2 = 1;
        WITH_ERROR_BLINDING(conn, POSIX_GUARD(s2n_sslv2_record_header_parse(conn, record_type, &conn->client_protocol_version, &fragment_length)));
    } else {
        WITH_ERROR_BLINDING(conn, POSIX_GUARD(s2n_record_header_parse(conn, record_type, &fragment_length)));
    }

    /* Read enough to have the whole record */
    uint32_t fragment_available = s2n_stuffer_data_available(&conn->in);
    if (fragment_available < fragment_length || fragment_length == 0) {
        POSIX_GUARD(s2n_stuffer_rewind_read(&conn->buffer_in, fragment_available));
        s2n_result ret = s2n_recv_buffer_in(conn, fragment_length);
        uint32_t fragment_read = MIN(fragment_length, s2n_stuffer_data_available(&conn->buffer_in));
        POSIX_GUARD_RESULT(s2n_recv_in_init(conn, fragment_read, fragment_length));
        POSIX_GUARD_RESULT(ret);
    }

    if (*isSSLv2) {
        return 0;
    }

    /* Decrypt and parse the record */
    if (s2n_early_data_is_trial_decryption_allowed(conn, *record_type)) {
        POSIX_ENSURE(s2n_record_parse(conn) >= S2N_SUCCESS, S2N_ERR_EARLY_DATA_TRIAL_DECRYPT);
    } else {
        WITH_ERROR_BLINDING(conn, POSIX_GUARD(s2n_record_parse(conn)));
    }

    /* In TLS 1.3, encrypted handshake records would appear to be of record type
    * TLS_APPLICATION_DATA. The actual record content type is found after the encrypted
    * is decrypted.
    */
    if (conn->actual_protocol_version == S2N_TLS13 && *record_type == TLS_APPLICATION_DATA) {
        POSIX_GUARD(s2n_tls13_parse_record_type(&conn->in, record_type));
    }

    return 0;
}

ssize_t s2n_recv_impl(struct s2n_connection *conn, void *buf, ssize_t size_signed, s2n_blocked_status *blocked)
{
    POSIX_ENSURE_GTE(size_signed, 0);
    size_t size = size_signed;
    ssize_t bytes_read = 0;
    struct s2n_blob out = { 0 };
    POSIX_GUARD(s2n_blob_init(&out, (uint8_t *) buf, 0));

    /*
     * Set the `blocked` status to BLOCKED_ON_READ by default
     *
     * The only case in which it should be updated is on a successful read into the provided buffer.
     *
     * Unfortunately, the current `blocked` behavior has become ossified by buggy applications that ignore
     * error types and only read `blocked`. As such, it's very important to avoid changing how this value is updated
     * as it could break applications.
     */
    *blocked = S2N_BLOCKED_ON_READ;

    if (!s2n_connection_check_io_status(conn, S2N_IO_READABLE)) {
        /*
         *= https://www.rfc-editor.org/rfc/rfc8446#6.1
         *# If a transport-level close
         *# is received prior to a "close_notify", the receiver cannot know that
         *# all the data that was sent has been received.
         *
         *= https://www.rfc-editor.org/rfc/rfc8446#6.1
         *# If the application protocol using TLS provides that any data may be
         *# carried over the underlying transport after the TLS connection is
         *# closed, the TLS implementation MUST receive a "close_notify" alert
         *# before indicating end-of-data to the application layer.
         */
        POSIX_ENSURE(s2n_atomic_flag_test(&conn->close_notify_received), S2N_ERR_CLOSED);
        *blocked = S2N_NOT_BLOCKED;
        return 0;
    }

    POSIX_ENSURE(!s2n_connection_is_quic_enabled(conn), S2N_ERR_UNSUPPORTED_WITH_QUIC);
    POSIX_GUARD_RESULT(s2n_early_data_validate_recv(conn));

    while (size && s2n_connection_check_io_status(conn, S2N_IO_READABLE)) {
        int isSSLv2 = 0;
        uint8_t record_type = 0;
        int r = s2n_read_full_record(conn, &record_type, &isSSLv2);
        if (r < 0) {
            /* Don't propagate the error if we already read some bytes. */
            if (bytes_read && (s2n_errno == S2N_ERR_CLOSED || s2n_errno == S2N_ERR_IO_BLOCKED)) {
                break;
            }

            /* If we get here, it's an error condition. 
             * For stateful resumption, invalidate the session on error to prevent resumption with 
             * potentially corrupted session state. This ensures that a bad session state does not 
             * lead to repeated failures during resumption attempts.
             */
            if (s2n_errno != S2N_ERR_IO_BLOCKED && s2n_allowed_to_cache_connection(conn) && conn->session_id_len) {
                conn->config->cache_delete(conn, conn->config->cache_delete_data, conn->session_id, conn->session_id_len);
            }

            S2N_ERROR_PRESERVE_ERRNO();
        }

        S2N_ERROR_IF(isSSLv2, S2N_ERR_BAD_MESSAGE);

        if (record_type != TLS_HANDSHAKE) {
            /*
             *= https://www.rfc-editor.org/rfc/rfc8446#section-5.1
             *#    -  Handshake messages MUST NOT be interleaved with other record
             *#       types.  That is, if a handshake message is split over two or more
             *#       records, there MUST NOT be any other records between them.
             */
            POSIX_ENSURE(s2n_stuffer_is_wiped(&conn->post_handshake.in), S2N_ERR_BAD_MESSAGE);

            /* If not handling a handshake message, free the post-handshake memory.
             * Post-handshake messages are infrequent enough that we don't want to
             * keep a potentially large buffer around unnecessarily.
             */
            if (!s2n_stuffer_is_freed(&conn->post_handshake.in)) {
                POSIX_GUARD(s2n_stuffer_free(&conn->post_handshake.in));
            }
        }

        if (record_type != TLS_APPLICATION_DATA) {
            switch (record_type) {
                case TLS_ALERT:
                    POSIX_GUARD(s2n_process_alert_fragment(conn));
                    break;
                case TLS_HANDSHAKE: {
                    s2n_result result = s2n_post_handshake_recv(conn);
                    /* Ignore any errors due to insufficient input data from io.
                     * The next iteration of this loop will attempt to read more input data.
                     */
                    if (s2n_result_is_error(result) && s2n_errno != S2N_ERR_IO_BLOCKED) {
                        WITH_ERROR_BLINDING(conn, POSIX_GUARD_RESULT(result));
                    }
                    break;
                }
            }
            POSIX_GUARD_RESULT(s2n_record_wipe(conn));
            continue;
        }

        out.size = MIN(size, s2n_stuffer_data_available(&conn->in));

        POSIX_GUARD(s2n_stuffer_erase_and_read(&conn->in, &out));
        bytes_read += out.size;

        out.data += out.size;
        size -= out.size;

        /* Are we ready for more encrypted data? */
        if (s2n_stuffer_data_available(&conn->in) == 0) {
            POSIX_GUARD_RESULT(s2n_record_wipe(conn));
        }

        /* If we've read some data, return it in legacy mode */
        if (bytes_read && !conn->config->recv_multi_record) {
            break;
        }
    }

    /* Due to the history of this API, some applications depend on the blocked status to know if
     * the connection's `in` stuffer was completely cleared. This behavior needs to be preserved.
     *
     * Moving forward, applications should instead use `s2n_peek`, which accomplishes the same thing
     * without conflating being blocked on reading from the OS socket vs blocked on the application's
     * buffer size.
     */
    if (s2n_stuffer_data_available(&conn->in) == 0) {
        *blocked = S2N_NOT_BLOCKED;
    }

    return bytes_read;
}

ssize_t s2n_recv(struct s2n_connection *conn, void *buf, ssize_t size, s2n_blocked_status *blocked)
{
    POSIX_ENSURE(!conn->recv_in_use, S2N_ERR_REENTRANCY);
    conn->recv_in_use = true;

    ssize_t result = s2n_recv_impl(conn, buf, size, blocked);
    POSIX_GUARD_RESULT(s2n_early_data_record_bytes(conn, result));

    /* finish the recv call */
    POSIX_GUARD_RESULT(s2n_connection_dynamic_free_in_buffer(conn));

    conn->recv_in_use = false;
    return result;
}

uint32_t s2n_peek(struct s2n_connection *conn)
{
    if (conn == NULL) {
        return 0;
    }

    /* If we have partially buffered an encrypted record,
     * we should not report those bytes as available to read.
     */
    if (conn->in_status != PLAINTEXT) {
        return 0;
    }

    return s2n_stuffer_data_available(&conn->in);
}

uint32_t s2n_peek_buffered(struct s2n_connection *conn)
{
    if (conn == NULL) {
        return 0;
    }
    return s2n_stuffer_data_available(&conn->buffer_in);
}
