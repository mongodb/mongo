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

#include "api/s2n.h"
#include "tls/s2n_alerts.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_atomic.h"
#include "utils/s2n_safety.h"

static bool s2n_shutdown_expect_close_notify(struct s2n_connection *conn)
{
    /* No close_notify expected if we already received an error instead */
    if (s2n_atomic_flag_test(&conn->error_alert_received)) {
        return false;
    }

    /* No close_notify expected if we sent an error instead of a close_notify */
    if (conn->writer_alert_out || conn->reader_alert_out) {
        return false;
    }

    /* The purpose of the peer responding to our close_notify
     * with its own close_notify is to prevent application data truncation.
     * However, application data is not a concern during the handshake.
     *
     * Additionally, decrypting alerts sent during the handshake can be error prone
     * due to different encryption keys and may lead to unnecessary error reporting
     * and unnecessary blinding.
     */
    if (!s2n_handshake_is_complete(conn)) {
        return false;
    }

    /* QUIC does not use TLS alerts */
    if (conn->quic_enabled) {
        return false;
    }

    /* Blinded errors indicate a fatal error handling inputs.
     * We should not attempt to handle further inputs.
     */
    if (conn->delay) {
        return false;
    }

    return true;
}

int s2n_shutdown_send(struct s2n_connection *conn, s2n_blocked_status *blocked)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(blocked);
    *blocked = S2N_NOT_BLOCKED;

    /* Treat this call as a no-op if already wiped.
     * This should probably be an error, but wasn't in the past so is left as-is
     * for backwards compatibility.
     */
    if (conn->send == NULL && conn->recv == NULL) {
        return S2N_SUCCESS;
    }

    /* Flush any outstanding data */
    s2n_atomic_flag_set(&conn->write_closed);
    POSIX_GUARD(s2n_flush(conn, blocked));

    /* For a connection closed due to receiving an alert, we don't send anything. */
    if (s2n_atomic_flag_test(&conn->error_alert_received)) {
        return S2N_SUCCESS;
    }

    /* If we've already sent an alert, don't send another. */
    if (conn->alert_sent) {
        return S2N_SUCCESS;
    }

    /* Enforce blinding.
     * If an application is using self-service blinding, ensure that they have
     * waited the required time before triggering any alerts.
     */
    uint64_t elapsed = 0;
    POSIX_GUARD_RESULT(s2n_timer_elapsed(conn->config, &conn->write_timer, &elapsed));
    S2N_ERROR_IF(elapsed < conn->delay, S2N_ERR_SHUTDOWN_PAUSED);

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-6.1
     *# Each party MUST send a "close_notify" alert before closing its write
     *# side of the connection, unless it has already sent some error alert.
     */
    POSIX_GUARD_RESULT(s2n_alerts_write_error_or_close_notify(conn));
    POSIX_GUARD(s2n_flush(conn, blocked));
    return S2N_SUCCESS;
}

int s2n_shutdown(struct s2n_connection *conn, s2n_blocked_status *blocked)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(blocked);
    *blocked = S2N_NOT_BLOCKED;

    /* If necessary, send an alert to indicate shutdown. */
    POSIX_GUARD(s2n_shutdown_send(conn, blocked));

    /* If we don't expect a close_notify from our peer,
     * just ensure that the connection is marked closed.
     */
    if (!s2n_shutdown_expect_close_notify(conn)) {
        POSIX_GUARD_RESULT(s2n_connection_set_closed(conn));
        *blocked = S2N_NOT_BLOCKED;
        return S2N_SUCCESS;
    }

    /* Wait for the peer's close_notify. */
    uint8_t record_type = 0;
    int isSSLv2 = false;
    *blocked = S2N_BLOCKED_ON_READ;
    while (!s2n_atomic_flag_test(&conn->close_notify_received)) {
        POSIX_GUARD(s2n_read_full_record(conn, &record_type, &isSSLv2));
        POSIX_ENSURE(!isSSLv2, S2N_ERR_BAD_MESSAGE);
        if (record_type == TLS_ALERT) {
            POSIX_GUARD(s2n_process_alert_fragment(conn));
        }
        POSIX_GUARD_RESULT(s2n_record_wipe(conn));
    }

    *blocked = S2N_NOT_BLOCKED;
    return S2N_SUCCESS;
}
