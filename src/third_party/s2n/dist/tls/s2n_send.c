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

#include <errno.h>
#include <sys/param.h>

#include "api/s2n.h"
#include "crypto/s2n_cipher.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_alerts.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_handshake.h"
#include "tls/s2n_internal.h"
#include "tls/s2n_ktls.h"
#include "tls/s2n_post_handshake.h"
#include "tls/s2n_record.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_io.h"
#include "utils/s2n_safety.h"

/*
 * Determine whether there is currently sufficient space in the send buffer to construct
 * another record, or if we need to flush now.
 *
 * We only buffer multiple records when sending application data, NOT when
 * sending handshake messages or alerts. If the next record is a post-handshake message
 * or an alert, then the send buffer will be flushed regardless of the result of this method.
 * Therefore we don't need to consider the size of any potential KeyUpdate messages,
 * NewSessionTicket messages, or Alerts.
 */
bool s2n_should_flush(struct s2n_connection *conn, ssize_t total_message_size)
{
    /* Always flush if not buffering multiple records. */
    if (!conn->multirecord_send) {
        return true;
    }

    /* Flush if all data has been sent. */
    ssize_t remaining_payload_size = total_message_size - conn->current_user_data_consumed;
    if (remaining_payload_size <= 0) {
        return true;
    }

    uint16_t max_payload_size = 0;
    if (!s2n_result_is_ok(s2n_record_max_write_payload_size(conn, &max_payload_size))) {
        /* When in doubt, flush */
        return true;
    }
    max_payload_size = MIN(max_payload_size, remaining_payload_size);

    uint16_t max_write_size = 0;
    if (!s2n_result_is_ok(s2n_record_max_write_size(conn, max_payload_size, &max_write_size))) {
        /* When in doubt, flush */
        return true;
    }

    /* Flush if the stuffer can't store the max possible record size without growing.
     *
     * However, the stuffer is allocated when the record is sent, so if the stuffer
     * hasn't been allocated, assume it will have enough space.
     */
    uint32_t available_space = s2n_stuffer_space_remaining(&conn->out);
    if (available_space < max_write_size && !s2n_stuffer_is_freed(&conn->out)) {
        return true;
    }

    return false;
}

int s2n_flush(struct s2n_connection *conn, s2n_blocked_status *blocked)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(blocked);
    *blocked = S2N_BLOCKED_ON_WRITE;

    /* Write any data that's already pending */
    while (s2n_stuffer_data_available(&conn->out)) {
        errno = 0;
        int w = s2n_connection_send_stuffer(&conn->out, conn, s2n_stuffer_data_available(&conn->out));
        POSIX_GUARD_RESULT(s2n_io_check_write_result(w));
        conn->wire_bytes_out += w;
    }
    POSIX_GUARD(s2n_stuffer_rewrite(&conn->out));

    if (conn->reader_warning_out) {
        POSIX_GUARD_RESULT(s2n_alerts_write_warning(conn));
        conn->reader_warning_out = 0;
        POSIX_GUARD(s2n_flush(conn, blocked));
    }

    *blocked = S2N_NOT_BLOCKED;
    return 0;
}

S2N_RESULT s2n_sendv_with_offset_total_size(const struct iovec *bufs, ssize_t count,
        ssize_t offs, ssize_t *total_size_out)
{
    RESULT_ENSURE_REF(total_size_out);
    if (count > 0) {
        RESULT_ENSURE_REF(bufs);
    }

    size_t total_size = 0;
    for (ssize_t i = 0; i < count; i++) {
        size_t iov_len = bufs[i].iov_len;
        /* Account for any offset */
        if (offs > 0) {
            size_t offs_consumed = MIN((size_t) offs, iov_len);
            iov_len -= offs_consumed;
            offs -= offs_consumed;
        }
        RESULT_ENSURE(S2N_ADD_IS_OVERFLOW_SAFE(total_size, iov_len, SIZE_MAX),
                S2N_ERR_INVALID_ARGUMENT);
        total_size += iov_len;
    }

    /* We must have fully accounted for the offset, or else the offset is larger
     * than the available data and our inputs are invalid.
     */
    RESULT_ENSURE(offs == 0, S2N_ERR_INVALID_ARGUMENT);

    RESULT_ENSURE(total_size <= SSIZE_MAX, S2N_ERR_INVALID_ARGUMENT);
    *total_size_out = total_size;
    return S2N_RESULT_OK;
}

ssize_t s2n_sendv_with_offset_impl(struct s2n_connection *conn, const struct iovec *bufs,
        ssize_t count, ssize_t offs, s2n_blocked_status *blocked)
{
    ssize_t user_data_sent = 0, total_size = 0;

    POSIX_ENSURE(s2n_connection_check_io_status(conn, S2N_IO_WRITABLE), S2N_ERR_CLOSED);
    POSIX_ENSURE(!s2n_connection_is_quic_enabled(conn), S2N_ERR_UNSUPPORTED_WITH_QUIC);

    /* Flush any pending I/O */
    POSIX_GUARD(s2n_flush(conn, blocked));

    if (conn->ktls_send_enabled) {
        return s2n_ktls_sendv_with_offset(conn, bufs, count, offs, blocked);
    }

    /* Acknowledge consumed and flushed user data as sent */
    user_data_sent = conn->current_user_data_consumed;

    *blocked = S2N_BLOCKED_ON_WRITE;

    uint16_t max_payload_size = 0;
    POSIX_GUARD_RESULT(s2n_record_max_write_payload_size(conn, &max_payload_size));

    /* TLS 1.0 and SSLv3 are vulnerable to the so-called Beast attack. Work
     * around this by splitting messages into one byte records, and then
     * the remainder can follow as usual.
     */
    int cbcHackUsed = 0;

    struct s2n_crypto_parameters *writer = conn->server;
    if (conn->mode == S2N_CLIENT) {
        writer = conn->client;
    }

    POSIX_GUARD_RESULT(s2n_sendv_with_offset_total_size(bufs, count, offs, &total_size));
    /* Defensive check against an invalid retry */
    POSIX_ENSURE(conn->current_user_data_consumed <= total_size, S2N_ERR_SEND_SIZE);
    POSIX_GUARD_RESULT(s2n_early_data_validate_send(conn, total_size));

    if (conn->dynamic_record_timeout_threshold > 0) {
        uint64_t elapsed = 0;
        POSIX_GUARD_RESULT(s2n_timer_elapsed(conn->config, &conn->write_timer, &elapsed));
        /* Reset record size back to a single segment after threshold seconds of inactivity */
        if (elapsed - conn->last_write_elapsed > (uint64_t) conn->dynamic_record_timeout_threshold * 1000000000) {
            conn->active_application_bytes_consumed = 0;
        }
        conn->last_write_elapsed = elapsed;
    }

    /* Now write the data we were asked to send this round */
    while (total_size - conn->current_user_data_consumed) {
        ssize_t to_write = MIN(total_size - conn->current_user_data_consumed, max_payload_size);

        /* If dynamic record size is enabled,
         * use small TLS records that fit into a single TCP segment for the threshold bytes of data
         */
        if (conn->active_application_bytes_consumed < (uint64_t) conn->dynamic_record_resize_threshold) {
            uint16_t min_payload_size = 0;
            POSIX_GUARD_RESULT(s2n_record_min_write_payload_size(conn, &min_payload_size));
            to_write = MIN(min_payload_size, to_write);
        }

        /* Don't split messages in server mode for interoperability with naive clients.
         * Some clients may have expectations based on the amount of content in the first record.
         */
        if (conn->actual_protocol_version < S2N_TLS11
                && writer->cipher_suite->record_alg->cipher->type == S2N_CBC && conn->mode != S2N_SERVER) {
            if (to_write > 1 && cbcHackUsed == 0) {
                to_write = 1;
                cbcHackUsed = 1;
            }
        }

        POSIX_GUARD(s2n_post_handshake_send(conn, blocked));

        /* Write and encrypt the record */
        int written_to_record = s2n_record_writev(conn, TLS_APPLICATION_DATA, bufs, count,
                conn->current_user_data_consumed + offs, to_write);
        POSIX_GUARD(written_to_record);
        conn->current_user_data_consumed += written_to_record;
        conn->active_application_bytes_consumed += written_to_record;

        /* Send it, unless we're waiting for more records */
        if (s2n_should_flush(conn, total_size)) {
            if (s2n_flush(conn, blocked) < 0) {
                if (s2n_errno == S2N_ERR_IO_BLOCKED && user_data_sent > 0) {
                    /* We successfully sent >0 user bytes on the wire, but not the full requested payload
                     * because we became blocked on I/O. Acknowledge the data sent. */

                    conn->current_user_data_consumed -= user_data_sent;
                    return user_data_sent;
                } else {
                    S2N_ERROR_PRESERVE_ERRNO();
                }
            }

            /* Acknowledge consumed and flushed user data as sent */
            user_data_sent = conn->current_user_data_consumed;
        }
    }

    /* If everything has been written, then there's no user data pending */
    conn->current_user_data_consumed = 0;

    *blocked = S2N_NOT_BLOCKED;
    return total_size;
}

ssize_t s2n_sendv_with_offset(struct s2n_connection *conn, const struct iovec *bufs, ssize_t count,
        ssize_t offs, s2n_blocked_status *blocked)
{
    POSIX_ENSURE(!conn->send_in_use, S2N_ERR_REENTRANCY);
    conn->send_in_use = true;

    ssize_t result = s2n_sendv_with_offset_impl(conn, bufs, count, offs, blocked);
    POSIX_GUARD_RESULT(s2n_early_data_record_bytes(conn, result));

    POSIX_GUARD_RESULT(s2n_connection_dynamic_free_out_buffer(conn));

    conn->send_in_use = false;
    return result;
}

ssize_t s2n_sendv(struct s2n_connection *conn, const struct iovec *bufs, ssize_t count, s2n_blocked_status *blocked)
{
    return s2n_sendv_with_offset(conn, bufs, count, 0, blocked);
}

ssize_t s2n_send(struct s2n_connection *conn, const void *buf, ssize_t size, s2n_blocked_status *blocked)
{
    struct iovec iov;
    iov.iov_base = (void *) (uintptr_t) buf;
    iov.iov_len = size;
    return s2n_sendv_with_offset(conn, &iov, 1, 0, blocked);
}
