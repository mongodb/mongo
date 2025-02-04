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
#include <time.h>

#include "api/s2n.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_alerts.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_record.h"
#include "tls/s2n_resume.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls13_handshake.h"
#include "utils/s2n_random.h"
#include "utils/s2n_safety.h"

/*
 * The maximum size of the NewSessionTicket message, not taking into account the
 * ticket itself.
 *
 * To get the actual maximum size required for the NewSessionTicket message, we'll need
 * to add the size of the ticket, which is much less predictable.
 *
 * This constant is enforced via unit tests.
 */
#define S2N_TLS13_MAX_FIXED_NEW_SESSION_TICKET_SIZE 112

int s2n_server_nst_recv(struct s2n_connection *conn)
{
    POSIX_GUARD(s2n_stuffer_read_uint32(&conn->handshake.io, &conn->ticket_lifetime_hint));

    uint16_t session_ticket_len = 0;
    POSIX_GUARD(s2n_stuffer_read_uint16(&conn->handshake.io, &session_ticket_len));

    if (session_ticket_len > 0) {
        POSIX_GUARD(s2n_realloc(&conn->client_ticket, session_ticket_len));

        POSIX_GUARD(s2n_stuffer_read(&conn->handshake.io, &conn->client_ticket));

        if (conn->config->session_ticket_cb != NULL) {
            size_t session_len = s2n_connection_get_session_length(conn);

            /* Alloc some memory for the serialized session ticket */
            DEFER_CLEANUP(struct s2n_blob mem = { 0 }, s2n_free);
            POSIX_GUARD(s2n_alloc(&mem,
                    S2N_STATE_FORMAT_LEN + S2N_SESSION_TICKET_SIZE_LEN + conn->client_ticket.size + S2N_TLS12_STATE_SIZE_IN_BYTES));

            POSIX_GUARD(s2n_connection_get_session(conn, mem.data, session_len));
            uint32_t session_lifetime = s2n_connection_get_session_ticket_lifetime_hint(conn);

            struct s2n_session_ticket ticket = { .ticket_data = mem, .session_lifetime = session_lifetime };

            POSIX_ENSURE(conn->config->session_ticket_cb(conn, conn->config->session_ticket_ctx, &ticket) >= S2N_SUCCESS,
                    S2N_ERR_CANCELLED);
        }
    }

    return S2N_SUCCESS;
}

int s2n_server_nst_send(struct s2n_connection *conn)
{
    uint16_t session_ticket_len = S2N_TLS12_TICKET_SIZE_IN_BYTES;
    uint8_t data[S2N_TLS12_TICKET_SIZE_IN_BYTES] = { 0 };
    struct s2n_blob entry = { 0 };
    POSIX_GUARD(s2n_blob_init(&entry, data, sizeof(data)));
    struct s2n_stuffer to = { 0 };
    uint32_t lifetime_hint_in_secs =
            (conn->config->encrypt_decrypt_key_lifetime_in_nanos + conn->config->decrypt_key_lifetime_in_nanos) / ONE_SEC_IN_NANOS;

    /* Send a zero-length ticket in the NewSessionTicket message if the server changes 
     * its mind mid-handshake or if there are no valid encrypt keys currently available. 
     *
     *= https://www.rfc-editor.org/rfc/rfc5077#section-3.3
     *# If the server determines that it does not want to include a
     *# ticket after it has included the SessionTicket extension in the
     *# ServerHello, then it sends a zero-length ticket in the
     *# NewSessionTicket handshake message.
     **/
    POSIX_GUARD(s2n_stuffer_init(&to, &entry));
    if (!conn->config->use_tickets || s2n_result_is_error(s2n_resume_encrypt_session_ticket(conn, &to))) {
        POSIX_GUARD(s2n_stuffer_write_uint32(&conn->handshake.io, 0));
        POSIX_GUARD(s2n_stuffer_write_uint16(&conn->handshake.io, 0));

        return S2N_SUCCESS;
    }

    if (!s2n_server_sending_nst(conn)) {
        POSIX_BAIL(S2N_ERR_SENDING_NST);
    }

    POSIX_GUARD(s2n_stuffer_write_uint32(&conn->handshake.io, lifetime_hint_in_secs));
    POSIX_GUARD(s2n_stuffer_write_uint16(&conn->handshake.io, session_ticket_len));

    POSIX_GUARD(s2n_stuffer_write(&conn->handshake.io, &to.blob));

    /* For parity with TLS1.3, track the single ticket sent.
     * This simplifies s2n_connection_get_tickets_sent.
     */
    conn->tickets_sent++;
    return S2N_SUCCESS;
}

S2N_RESULT s2n_tls13_server_nst_send(struct s2n_connection *conn, s2n_blocked_status *blocked)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_GTE(conn->actual_protocol_version, S2N_TLS13);

    /* Usually tickets are sent immediately after the handshake.
     * If possible, reuse the handshake IO stuffer before it's wiped.
     *
     * Note: handshake.io isn't explicitly dedicated to only reading or only writing,
     * so we have to be careful using it outside of s2n_negotiate.
     * If we use it for writing here, we CAN'T use it for reading any post-handshake messages.
     */
    struct s2n_stuffer *nst_stuffer = &conn->handshake.io;

    if (conn->mode != S2N_SERVER || !conn->config->use_tickets) {
        return S2N_RESULT_OK;
    }

    /* Legacy behavior is that the s2n server sends a NST even if the client did not indicate support
     * for resumption or does not support the psk_dhe_ke mode. This is potentially wasteful so we 
     * choose to not extend this behavior to QUIC.
     */
    if (conn->quic_enabled && conn->psk_params.psk_ke_mode != S2N_PSK_DHE_KE) {
        return S2N_RESULT_OK;
    }

    /* No-op if all tickets already sent.
     * Clean up the stuffer used for the ticket to conserve memory. */
    if (conn->tickets_to_send == conn->tickets_sent) {
        RESULT_GUARD_POSIX(s2n_stuffer_resize(nst_stuffer, 0));
        return S2N_RESULT_OK;
    }

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.6.1
     *# Note that in principle it is possible to continue issuing new tickets
     *# which indefinitely extend the lifetime of the keying material
     *# originally derived from an initial non-PSK handshake (which was most
     *# likely tied to the peer's certificate). It is RECOMMENDED that
     *# implementations place limits on the total lifetime of such keying
     *# material; these limits should take into account the lifetime of the
     *# peer's certificate, the likelihood of intervening revocation, and the
     *# time since the peer's online CertificateVerify signature.
     */
    if (s2n_result_is_error(s2n_psk_validate_keying_material(conn))) {
        conn->tickets_to_send = conn->tickets_sent;
        return S2N_RESULT_OK;
    }

    RESULT_ENSURE(conn->tickets_sent <= conn->tickets_to_send, S2N_ERR_INTEGER_OVERFLOW);

    size_t session_state_size = 0;
    RESULT_GUARD(s2n_connection_get_session_state_size(conn, &session_state_size));
    const size_t maximum_nst_size = session_state_size + S2N_TLS13_MAX_FIXED_NEW_SESSION_TICKET_SIZE;
    if (s2n_stuffer_space_remaining(nst_stuffer) < maximum_nst_size) {
        RESULT_GUARD_POSIX(s2n_stuffer_resize(nst_stuffer, maximum_nst_size));
    }

    while (conn->tickets_to_send - conn->tickets_sent > 0) {
        if (s2n_result_is_error(s2n_tls13_server_nst_write(conn, nst_stuffer))) {
            return S2N_RESULT_OK;
        }

        RESULT_GUARD(s2n_post_handshake_write_records(conn, blocked));
    }

    return S2N_RESULT_OK;
}

/** 
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.6.1
 *# Indicates the lifetime in seconds as a 32-bit
 *# unsigned integer in network byte order from the time of ticket
 *# issuance. 
 **/
static S2N_RESULT s2n_generate_ticket_lifetime(struct s2n_connection *conn, uint32_t *ticket_lifetime)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_MUT(ticket_lifetime);

    uint32_t key_lifetime_in_secs =
            (conn->config->encrypt_decrypt_key_lifetime_in_nanos + conn->config->decrypt_key_lifetime_in_nanos) / ONE_SEC_IN_NANOS;
    uint32_t session_lifetime_in_secs = conn->config->session_state_lifetime_in_nanos / ONE_SEC_IN_NANOS;
    uint32_t key_and_session_min_lifetime = MIN(key_lifetime_in_secs, session_lifetime_in_secs);
    /** 
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.6.1
     *# Servers MUST NOT use any value greater than
     *# 604800 seconds (7 days).
     **/
    *ticket_lifetime = MIN(key_and_session_min_lifetime, ONE_WEEK_IN_SEC);

    return S2N_RESULT_OK;
}

/** 
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.6.1
 *# A per-ticket value that is unique across all tickets
 *# issued on this connection.
 **/
static S2N_RESULT s2n_generate_ticket_nonce(uint16_t value, struct s2n_blob *output)
{
    RESULT_ENSURE_MUT(output);

    struct s2n_stuffer stuffer = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init(&stuffer, output));
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint16(&stuffer, value));

    return S2N_RESULT_OK;
}

/** 
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.6.1
 *# A securely generated, random 32-bit value that is
 *# used to obscure the age of the ticket that the client includes in
 *# the "pre_shared_key" extension.
 **/
static S2N_RESULT s2n_generate_ticket_age_add(struct s2n_blob *random_data, uint32_t *ticket_age_add)
{
    RESULT_ENSURE_REF(random_data);
    RESULT_ENSURE_REF(ticket_age_add);

    struct s2n_stuffer stuffer = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init(&stuffer, random_data));
    RESULT_GUARD_POSIX(s2n_stuffer_skip_write(&stuffer, random_data->size));
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint32(&stuffer, ticket_age_add));

    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.6.1
 *# The PSK associated with the ticket is computed as:
 *#
 *#    HKDF-Expand-Label(resumption_master_secret,
 *#                     "resumption", ticket_nonce, Hash.length)
 **/
static int s2n_generate_session_secret(struct s2n_connection *conn, struct s2n_blob *nonce, struct s2n_blob *output)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(nonce);
    POSIX_ENSURE_REF(output);

    s2n_tls13_connection_keys(secrets, conn);
    struct s2n_blob master_secret = { 0 };
    POSIX_GUARD(s2n_blob_init(&master_secret, conn->secrets.version.tls13.resumption_master_secret, secrets.size));
    POSIX_GUARD(s2n_realloc(output, secrets.size));
    POSIX_GUARD_RESULT(s2n_tls13_derive_session_ticket_secret(&secrets, &master_secret, nonce, output));

    return S2N_SUCCESS;
}

S2N_RESULT s2n_tls13_server_nst_write(struct s2n_connection *conn, struct s2n_stuffer *output)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(output);

    struct s2n_ticket_fields *ticket_fields = &conn->tls13_ticket_fields;

    /* Write message type because session resumption in TLS13 is a post-handshake message */
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint8(output, TLS_SERVER_NEW_SESSION_TICKET));

    struct s2n_stuffer_reservation message_size = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_reserve_uint24(output, &message_size));

    uint32_t ticket_lifetime_in_secs = 0;
    RESULT_GUARD(s2n_generate_ticket_lifetime(conn, &ticket_lifetime_in_secs));
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint32(output, ticket_lifetime_in_secs));

    /* Get random data to use as ticket_age_add value */
    uint8_t data[sizeof(uint32_t)] = { 0 };
    struct s2n_blob random_data = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&random_data, data, sizeof(data)));
    /** 
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.6.1
     *#  The server MUST generate a fresh value
     *#  for each ticket it sends.
     **/
    RESULT_GUARD(s2n_get_private_random_data(&random_data));
    RESULT_GUARD(s2n_generate_ticket_age_add(&random_data, &ticket_fields->ticket_age_add));
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint32(output, ticket_fields->ticket_age_add));

    /* Write ticket nonce */
    uint8_t nonce_data[sizeof(uint16_t)] = { 0 };
    struct s2n_blob nonce = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&nonce, nonce_data, sizeof(nonce_data)));
    RESULT_GUARD(s2n_generate_ticket_nonce(conn->tickets_sent, &nonce));
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint8(output, nonce.size));
    RESULT_GUARD_POSIX(s2n_stuffer_write_bytes(output, nonce.data, nonce.size));

    /* Derive individual session ticket secret */
    RESULT_GUARD_POSIX(s2n_generate_session_secret(conn, &nonce, &ticket_fields->session_secret));

    /* Write ticket */
    struct s2n_stuffer_reservation ticket_size = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_reserve_uint16(output, &ticket_size));
    RESULT_GUARD(s2n_resume_encrypt_session_ticket(conn, output));
    RESULT_GUARD_POSIX(s2n_stuffer_write_vector_size(&ticket_size));

    RESULT_GUARD_POSIX(s2n_extension_list_send(S2N_EXTENSION_LIST_NST, conn, output));

    RESULT_GUARD_POSIX(s2n_stuffer_write_vector_size(&message_size));

    RESULT_ENSURE(conn->tickets_sent < UINT16_MAX, S2N_ERR_INTEGER_OVERFLOW);
    conn->tickets_sent++;

    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.6.1
 *#     struct {
 *#         uint32 ticket_lifetime;
 *#         uint32 ticket_age_add;
 *#         opaque ticket_nonce<0..255>;
 *#         opaque ticket<1..2^16-1>;
 *#         Extension extensions<0..2^16-2>;
 *#      } NewSessionTicket;
**/
S2N_RESULT s2n_tls13_server_nst_recv(struct s2n_connection *conn, struct s2n_stuffer *input)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(input);
    RESULT_ENSURE_REF(conn->config);

    RESULT_ENSURE(conn->actual_protocol_version >= S2N_TLS13, S2N_ERR_BAD_MESSAGE);
    RESULT_ENSURE(conn->mode == S2N_CLIENT, S2N_ERR_BAD_MESSAGE);

    if (!conn->config->use_tickets) {
        return S2N_RESULT_OK;
    }
    struct s2n_ticket_fields *ticket_fields = &conn->tls13_ticket_fields;

    /* Handle `ticket_lifetime` field */
    uint32_t ticket_lifetime = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint32(input, &ticket_lifetime));
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.6.1
     *# Servers MUST NOT use any value greater than
     *# 604800 seconds (7 days).
     */
    RESULT_ENSURE(ticket_lifetime <= ONE_WEEK_IN_SEC, S2N_ERR_BAD_MESSAGE);
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.6.1
     *# The value of zero indicates that the
     *# ticket should be discarded immediately.
     */
    if (ticket_lifetime == 0) {
        return S2N_RESULT_OK;
    }
    conn->ticket_lifetime_hint = ticket_lifetime;

    /* Handle `ticket_age_add` field */
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint32(input, &ticket_fields->ticket_age_add));

    /* Handle `ticket_nonce` field */
    uint8_t ticket_nonce_len = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(input, &ticket_nonce_len));
    uint8_t nonce_data[UINT8_MAX] = { 0 };
    struct s2n_blob nonce = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&nonce, nonce_data, ticket_nonce_len));
    RESULT_GUARD_POSIX(s2n_stuffer_read_bytes(input, nonce.data, ticket_nonce_len));
    RESULT_GUARD_POSIX(s2n_generate_session_secret(conn, &nonce, &ticket_fields->session_secret));

    /* Handle `ticket` field */
    uint16_t session_ticket_len = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(input, &session_ticket_len));
    RESULT_ENSURE(session_ticket_len > 0, S2N_ERR_SAFETY);
    RESULT_GUARD_POSIX(s2n_realloc(&conn->client_ticket, session_ticket_len));
    RESULT_GUARD_POSIX(s2n_stuffer_read(input, &conn->client_ticket));

    /* Handle `extensions` field */
    RESULT_GUARD_POSIX(s2n_extension_list_recv(S2N_EXTENSION_LIST_NST, conn, input));

    if (conn->config->session_ticket_cb != NULL) {
        /* Retrieve serialized session data */
        const uint16_t session_state_size = s2n_connection_get_session_length(conn);
        DEFER_CLEANUP(struct s2n_blob session_state = { 0 }, s2n_free);
        RESULT_GUARD_POSIX(s2n_realloc(&session_state, session_state_size));
        RESULT_GUARD_POSIX(s2n_connection_get_session(conn, session_state.data, session_state.size));

        struct s2n_session_ticket ticket = {
            .ticket_data = session_state,
            .session_lifetime = ticket_lifetime
        };
        RESULT_ENSURE(conn->config->session_ticket_cb(conn, conn->config->session_ticket_ctx, &ticket) >= S2N_SUCCESS,
                S2N_ERR_CANCELLED);
    }

    return S2N_RESULT_OK;
}
