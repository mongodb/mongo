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

#include "tls/s2n_key_update.h"

#include "crypto/s2n_sequence.h"
#include "error/s2n_errno.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_record.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls13_handshake.h"
#include "utils/s2n_atomic.h"
#include "utils/s2n_safety.h"

static s2n_peer_key_update key_update_request_val = S2N_KEY_UPDATE_NOT_REQUESTED;

int s2n_key_update_write(struct s2n_blob *out);
int s2n_check_record_limit(struct s2n_connection *conn, struct s2n_blob *sequence_number);

S2N_RESULT s2n_set_key_update_request_for_testing(s2n_peer_key_update request)
{
    RESULT_ENSURE(s2n_in_unit_test(), S2N_ERR_NOT_IN_UNIT_TEST);
    key_update_request_val = request;
    return S2N_RESULT_OK;
}

int s2n_key_update_recv(struct s2n_connection *conn, struct s2n_stuffer *request)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE(conn->actual_protocol_version >= S2N_TLS13, S2N_ERR_BAD_MESSAGE);
    POSIX_ENSURE(!s2n_connection_is_quic_enabled(conn), S2N_ERR_BAD_MESSAGE);
    POSIX_ENSURE(!conn->ktls_recv_enabled, S2N_ERR_KTLS_KEYUPDATE);

    uint8_t key_update_request = 0;
    POSIX_GUARD(s2n_stuffer_read_uint8(request, &key_update_request));
    if (key_update_request == S2N_KEY_UPDATE_REQUESTED) {
        POSIX_ENSURE(!conn->ktls_send_enabled, S2N_ERR_KTLS_KEYUPDATE);
        s2n_atomic_flag_set(&conn->key_update_pending);
    } else {
        POSIX_ENSURE(key_update_request == S2N_KEY_UPDATE_NOT_REQUESTED, S2N_ERR_BAD_MESSAGE);
    }

    /* Update peer's key since a key_update was received */
    if (conn->mode == S2N_CLIENT) {
        POSIX_GUARD(s2n_update_application_traffic_keys(conn, S2N_SERVER, RECEIVING));
    } else {
        POSIX_GUARD(s2n_update_application_traffic_keys(conn, S2N_CLIENT, RECEIVING));
    }

    return S2N_SUCCESS;
}

int s2n_key_update_send(struct s2n_connection *conn, s2n_blocked_status *blocked)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);
    POSIX_ENSURE_GTE(conn->actual_protocol_version, S2N_TLS13);

    struct s2n_blob sequence_number = { 0 };
    POSIX_GUARD_RESULT(s2n_connection_get_sequence_number(conn, conn->mode, &sequence_number));
    POSIX_GUARD(s2n_check_record_limit(conn, &sequence_number));

    if (s2n_atomic_flag_test(&conn->key_update_pending)) {
        POSIX_ENSURE(!conn->ktls_send_enabled, S2N_ERR_KTLS_KEY_LIMIT);

        /* Flush any buffered records to ensure an empty output buffer.
         *
         * This is important when buffering multiple records because we don't:
         * 1) Respect max fragment length for handshake messages
         * 2) Check if there is sufficient space in the output buffer for
         *    post-handshake messages.
         */
        POSIX_GUARD(s2n_flush(conn, blocked));

        uint8_t key_update_data[S2N_KEY_UPDATE_MESSAGE_SIZE];
        struct s2n_blob key_update_blob = { 0 };
        POSIX_GUARD(s2n_blob_init(&key_update_blob, key_update_data, sizeof(key_update_data)));

        /* Write key update message */
        POSIX_GUARD(s2n_key_update_write(&key_update_blob));

        /* Encrypt the message */
        POSIX_GUARD_RESULT(s2n_record_write(conn, TLS_HANDSHAKE, &key_update_blob));

        /* Update encryption key */
        POSIX_GUARD(s2n_update_application_traffic_keys(conn, conn->mode, SENDING));

        s2n_atomic_flag_clear(&conn->key_update_pending);
        POSIX_GUARD(s2n_flush(conn, blocked));
    }

    return S2N_SUCCESS;
}

int s2n_key_update_write(struct s2n_blob *out)
{
    POSIX_ENSURE_REF(out);

    struct s2n_stuffer key_update_stuffer = { 0 };
    POSIX_GUARD(s2n_stuffer_init(&key_update_stuffer, out));
    POSIX_GUARD(s2n_stuffer_write_uint8(&key_update_stuffer, TLS_KEY_UPDATE));
    POSIX_GUARD(s2n_stuffer_write_uint24(&key_update_stuffer, S2N_KEY_UPDATE_LENGTH));

    /* s2n currently does not require peers to update their encryption keys. */
    POSIX_GUARD(s2n_stuffer_write_uint8(&key_update_stuffer, key_update_request_val));

    return S2N_SUCCESS;
}

int s2n_check_record_limit(struct s2n_connection *conn, struct s2n_blob *sequence_number)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(sequence_number);
    POSIX_ENSURE_REF(conn->secure);
    POSIX_ENSURE_REF(conn->secure->cipher_suite);
    POSIX_ENSURE_REF(conn->secure->cipher_suite->record_alg);

    /*
     * This is the sequence number that will be used for the next record,
     * because we incremented the sequence number after sending the last record.
     */
    uint64_t next_seq_num = 0;
    POSIX_GUARD(s2n_sequence_number_to_uint64(sequence_number, &next_seq_num));

    /*
     * If the next record is the last record we can send, then the next record needs
     * to contain a KeyUpdate message.
     *
     * This should always trigger on "==", but we use ">=" just in case.
     */
    if (next_seq_num >= conn->secure->cipher_suite->record_alg->encryption_limit) {
        s2n_atomic_flag_set(&conn->key_update_pending);
    }

    return S2N_SUCCESS;
}

int s2n_connection_request_key_update(struct s2n_connection *conn, s2n_peer_key_update peer_request)
{
    POSIX_ENSURE_REF(conn);
    /* s2n-tls does not currently support requesting key updates from peers */
    POSIX_ENSURE(peer_request == S2N_KEY_UPDATE_NOT_REQUESTED, S2N_ERR_INVALID_ARGUMENT);
    s2n_atomic_flag_set(&conn->key_update_pending);
    return S2N_SUCCESS;
}
