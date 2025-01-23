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

#include <stdint.h>

#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_safety.h"

/* From RFC5246 7.1: https://tools.ietf.org/html/rfc5246#section-7.1 */
#define CHANGE_CIPHER_SPEC_TYPE 1

int s2n_basic_ccs_recv(struct s2n_connection *conn)
{
    uint8_t type = 0;

    POSIX_GUARD(s2n_stuffer_read_uint8(&conn->handshake.io, &type));
    S2N_ERROR_IF(type != CHANGE_CIPHER_SPEC_TYPE, S2N_ERR_BAD_MESSAGE);

    return 0;
}

int s2n_client_ccs_recv(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);

    POSIX_GUARD(s2n_basic_ccs_recv(conn));

    /* Zero the sequence number */
    struct s2n_blob seq = { 0 };
    POSIX_GUARD(s2n_blob_init(&seq, conn->secure->client_sequence_number, sizeof(conn->secure->client_sequence_number)));
    POSIX_GUARD(s2n_blob_zero(&seq));

    /* Update the client to use the cipher-suite */
    conn->client = conn->secure;

    /* Flush any partial alert messages that were pending.
     * If we don't do this, an attacker can inject a 1-byte alert message into the handshake
     * and cause later, valid alerts to be processed incorrectly. */
    POSIX_GUARD(s2n_stuffer_wipe(&conn->alert_in));

    return 0;
}

int s2n_server_ccs_recv(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);

    POSIX_GUARD(s2n_basic_ccs_recv(conn));

    /* Zero the sequence number */
    struct s2n_blob seq = { 0 };
    POSIX_GUARD(s2n_blob_init(&seq, conn->secure->server_sequence_number, sizeof(conn->secure->server_sequence_number)));
    POSIX_GUARD(s2n_blob_zero(&seq));

    /* Compute the finished message */
    POSIX_GUARD(s2n_prf_server_finished(conn));

    /* Update the secure state to active, and point the client at the active state */
    conn->server = conn->secure;

    /* Flush any partial alert messages that were pending.
     * If we don't do this, an attacker can inject a 1-byte alert message into the handshake
     * and cause later, valid alerts to be processed incorrectly. */
    POSIX_GUARD(s2n_stuffer_wipe(&conn->alert_in));

    return 0;
}

int s2n_ccs_send(struct s2n_connection *conn)
{
    POSIX_GUARD(s2n_stuffer_write_uint8(&conn->handshake.io, CHANGE_CIPHER_SPEC_TYPE));

    return 0;
}
