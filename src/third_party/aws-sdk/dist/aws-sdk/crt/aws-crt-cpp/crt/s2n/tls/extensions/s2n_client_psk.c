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

#include "tls/extensions/s2n_client_psk.h"

#include <stdint.h>
#include <sys/param.h>

#include "crypto/s2n_hash.h"
#include "tls/s2n_psk.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_bitmap.h"
#include "utils/s2n_safety.h"

#define SIZE_OF_BINDER_SIZE      sizeof(uint8_t)
#define SIZE_OF_BINDER_LIST_SIZE sizeof(uint16_t)

/* To avoid a DoS attack triggered by decrypting too many session tickets,
 * set a limit on the number of tickets we will attempt to decrypt before giving up.
 * We may want to make this configurable someday, but just set a reasonable maximum for now. */
#define MAX_REJECTED_TICKETS 3

static int s2n_client_psk_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_client_psk_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);
static int s2n_client_psk_is_missing(struct s2n_connection *conn);

const s2n_extension_type s2n_client_psk_extension = {
    .iana_value = TLS_EXTENSION_PRE_SHARED_KEY,
    .minimum_version = S2N_TLS13,
    .is_response = false,
    .send = s2n_client_psk_send,
    .recv = s2n_client_psk_recv,
    .should_send = s2n_client_psk_should_send,
    .if_missing = s2n_client_psk_is_missing,
};

int s2n_client_psk_is_missing(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    /* If the PSK extension is missing, we must not have received
     * a request for early data.
     *
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
     *# When a PSK is used and early data is allowed for that PSK, the client
     *# can send Application Data in its first flight of messages.  If the
     *# client opts to do so, it MUST supply both the "pre_shared_key" and
     *# "early_data" extensions.
     */
    POSIX_ENSURE(conn->early_data_state != S2N_EARLY_DATA_REQUESTED, S2N_ERR_UNSUPPORTED_EXTENSION);
    return S2N_SUCCESS;
}

bool s2n_client_psk_should_send(struct s2n_connection *conn)
{
    if (!conn || !conn->secure) {
        return false;
    }

    /* If this is NOT the second ClientHello after a retry, then all PSKs are viable.
     * Send the extension if any PSKs are configured.
     */
    if (!s2n_is_hello_retry_handshake(conn)) {
        return conn->psk_params.psk_list.len > 0;
    }

    /* If this is the second ClientHello after a retry, then only PSKs that match the cipher suite
     * are viable. Only send the extension if at least one configured PSK matches the cipher suite.
     */
    for (size_t i = 0; i < conn->psk_params.psk_list.len; i++) {
        struct s2n_psk *psk = NULL;
        if (s2n_result_is_ok(s2n_array_get(&conn->psk_params.psk_list, i, (void **) &psk))
                && psk != NULL
                && conn->secure->cipher_suite->prf_alg == psk->hmac_alg) {
            return true;
        }
    }
    return false;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.11.1
 *# The "obfuscated_ticket_age"
 *# field of each PskIdentity contains an obfuscated version of the
 *# ticket age formed by taking the age in milliseconds and adding the
 *# "ticket_age_add" value that was included with the ticket (see
 *# Section 4.6.1), modulo 2^32.
*/
static S2N_RESULT s2n_generate_obfuscated_ticket_age(struct s2n_psk *psk, uint64_t current_time, uint32_t *output)
{
    RESULT_ENSURE_REF(psk);
    RESULT_ENSURE_MUT(output);

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.11
     *# For identities
     *# established externally, an obfuscated_ticket_age of 0 SHOULD be
     *# used,
     **/
    if (psk->type == S2N_PSK_TYPE_EXTERNAL) {
        *output = 0;
        return S2N_RESULT_OK;
    }

    RESULT_ENSURE(current_time >= psk->ticket_issue_time, S2N_ERR_SAFETY);

    /* Calculate ticket age */
    uint64_t ticket_age_in_nanos = current_time - psk->ticket_issue_time;

    /* Convert ticket age to milliseconds */
    uint64_t ticket_age_in_millis = ticket_age_in_nanos / ONE_MILLISEC_IN_NANOS;
    RESULT_ENSURE(ticket_age_in_millis <= UINT32_MAX, S2N_ERR_SAFETY);

    /* Add the ticket_age_add value to the ticket age in milliseconds. The resulting uint32_t value
     * may wrap, resulting in the modulo 2^32 operation. */
    *output = ticket_age_in_millis + psk->ticket_age_add;

    return S2N_RESULT_OK;
}

static int s2n_client_psk_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);

    struct s2n_psk_parameters *psk_params = &conn->psk_params;
    struct s2n_array *psk_list = &psk_params->psk_list;

    struct s2n_stuffer_reservation identity_list_size;
    POSIX_GUARD(s2n_stuffer_reserve_uint16(out, &identity_list_size));

    uint16_t binder_list_size = SIZE_OF_BINDER_LIST_SIZE;

    for (size_t i = 0; i < psk_list->len; i++) {
        struct s2n_psk *psk = NULL;
        POSIX_GUARD_RESULT(s2n_array_get(psk_list, i, (void **) &psk));
        POSIX_ENSURE_REF(psk);

        /**
         *= https://www.rfc-editor.org/rfc/rfc8446#section-4.1.4
         *# In addition, in its updated ClientHello, the client SHOULD NOT offer
         *# any pre-shared keys associated with a hash other than that of the
         *# selected cipher suite.
         */
        if (s2n_is_hello_retry_handshake(conn) && conn->secure->cipher_suite->prf_alg != psk->hmac_alg) {
            continue;
        }

        /* Write the identity */
        POSIX_GUARD(s2n_stuffer_write_uint16(out, psk->identity.size));
        POSIX_GUARD(s2n_stuffer_write(out, &psk->identity));

        /* Write obfuscated ticket age */
        uint32_t obfuscated_ticket_age = 0;
        uint64_t current_time = 0;
        POSIX_GUARD_RESULT(s2n_config_wall_clock(conn->config, &current_time));
        POSIX_GUARD_RESULT(s2n_generate_obfuscated_ticket_age(psk, current_time, &obfuscated_ticket_age));
        POSIX_GUARD(s2n_stuffer_write_uint32(out, obfuscated_ticket_age));

        /* Calculate binder size */
        uint8_t hash_size = 0;
        POSIX_GUARD(s2n_hmac_digest_size(psk->hmac_alg, &hash_size));
        binder_list_size += hash_size + SIZE_OF_BINDER_SIZE;
    }

    POSIX_GUARD(s2n_stuffer_write_vector_size(&identity_list_size));

    /* Calculating the binders requires a complete ClientHello, and at this point
     * the extension size, extension list size, and message size are all blank.
     *
     * We'll write placeholder data to ensure the extension and extension list sizes
     * are calculated correctly, then rewrite the binders with real data later. */
    psk_params->binder_list_size = binder_list_size;
    POSIX_GUARD(s2n_stuffer_skip_write(out, binder_list_size));

    return S2N_SUCCESS;
}

/* Find the first of the server's PSK identities that matches the client's identities.
 * This method compares all server identities to all client identities.
 *
 * While both the client's identities and whether a match was found are public, we should make an attempt
 * to keep the server's identities a secret. We will make comparisons to the server's identities constant
 * time (to hide partial matches) and not end the search early when a match is found (to hide the ordering).
 *
 * Keeping these comparisons constant time is not high priority. There's no known attack using these timings,
 * and an attacker could probably guess the server's known identities just by observing the public identities
 * sent by clients.
 */
static S2N_RESULT s2n_select_external_psk(struct s2n_connection *conn, struct s2n_offered_psk_list *client_identity_list)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(client_identity_list);

    struct s2n_array *server_psks = &conn->psk_params.psk_list;
    conn->psk_params.chosen_psk = NULL;

    for (size_t i = 0; i < server_psks->len; i++) {
        struct s2n_psk *server_psk = NULL;
        RESULT_GUARD(s2n_array_get(server_psks, i, (void **) &server_psk));
        RESULT_ENSURE_REF(server_psk);

        struct s2n_offered_psk client_psk = { 0 };
        uint16_t wire_index = 0;

        RESULT_GUARD_POSIX(s2n_offered_psk_list_reread(client_identity_list));
        while (s2n_offered_psk_list_has_next(client_identity_list)) {
            RESULT_GUARD_POSIX(s2n_offered_psk_list_next(client_identity_list, &client_psk));
            uint16_t compare_size = MIN(client_psk.identity.size, server_psk->identity.size);
            if (s2n_constant_time_equals(client_psk.identity.data, server_psk->identity.data, compare_size)
                    & (client_psk.identity.size == server_psk->identity.size)
                    & (conn->psk_params.chosen_psk == NULL)) {
                conn->psk_params.chosen_psk = server_psk;
                conn->psk_params.chosen_psk_wire_index = wire_index;
            }
            wire_index++;
        };
    }
    RESULT_ENSURE_REF(conn->psk_params.chosen_psk);
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_select_resumption_psk(struct s2n_connection *conn, struct s2n_offered_psk_list *client_identity_list)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(client_identity_list);

    struct s2n_offered_psk client_psk = { 0 };
    conn->psk_params.chosen_psk = NULL;

    uint8_t rejected_count = 0;
    while (s2n_offered_psk_list_has_next(client_identity_list) && (rejected_count < MAX_REJECTED_TICKETS)) {
        RESULT_GUARD_POSIX(s2n_offered_psk_list_next(client_identity_list, &client_psk));
        /* Select the first resumption PSK that can be decrypted */
        if (s2n_offered_psk_list_choose_psk(client_identity_list, &client_psk) == S2N_SUCCESS) {
            return S2N_RESULT_OK;
        }
        rejected_count++;
    }

    RESULT_BAIL(S2N_ERR_INVALID_SESSION_TICKET);
}

static S2N_RESULT s2n_client_psk_recv_identity_list(struct s2n_connection *conn, struct s2n_stuffer *wire_identities_in)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->config);
    RESULT_ENSURE_REF(wire_identities_in);

    struct s2n_offered_psk_list identity_list = {
        .conn = conn,
        .wire_data = *wire_identities_in,
    };

    if (conn->config->psk_selection_cb) {
        RESULT_GUARD_POSIX(conn->config->psk_selection_cb(conn, conn->config->psk_selection_ctx, &identity_list));
    } else if (conn->psk_params.type == S2N_PSK_TYPE_EXTERNAL) {
        RESULT_GUARD(s2n_select_external_psk(conn, &identity_list));
    } else if (conn->psk_params.type == S2N_PSK_TYPE_RESUMPTION) {
        RESULT_GUARD(s2n_select_resumption_psk(conn, &identity_list));
    }

    RESULT_ENSURE_REF(conn->psk_params.chosen_psk);
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_client_psk_recv_binder_list(struct s2n_connection *conn, struct s2n_blob *partial_client_hello,
        struct s2n_stuffer *wire_binders_in)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(wire_binders_in);

    uint16_t wire_index = 0;
    while (s2n_stuffer_data_available(wire_binders_in) > 0) {
        uint8_t wire_binder_size = 0;
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(wire_binders_in, &wire_binder_size));

        uint8_t *wire_binder_data = NULL;
        RESULT_ENSURE_REF(wire_binder_data = s2n_stuffer_raw_read(wire_binders_in, wire_binder_size));

        struct s2n_blob wire_binder = { 0 };
        RESULT_GUARD_POSIX(s2n_blob_init(&wire_binder, wire_binder_data, wire_binder_size));

        if (wire_index == conn->psk_params.chosen_psk_wire_index) {
            RESULT_GUARD_POSIX(s2n_psk_verify_binder(conn, conn->psk_params.chosen_psk,
                    partial_client_hello, &wire_binder));
            return S2N_RESULT_OK;
        }
        wire_index++;
    }
    RESULT_BAIL(S2N_ERR_BAD_MESSAGE);
}

static S2N_RESULT s2n_client_psk_recv_identities(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    RESULT_ENSURE_REF(conn);

    uint16_t identity_list_size = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(extension, &identity_list_size));

    uint8_t *identity_list_data = NULL;
    RESULT_ENSURE_REF(identity_list_data = s2n_stuffer_raw_read(extension, identity_list_size));

    struct s2n_blob identity_list_blob = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&identity_list_blob, identity_list_data, identity_list_size));

    struct s2n_stuffer identity_list = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init(&identity_list, &identity_list_blob));
    RESULT_GUARD_POSIX(s2n_stuffer_skip_write(&identity_list, identity_list_blob.size));

    return s2n_client_psk_recv_identity_list(conn, &identity_list);
}

static S2N_RESULT s2n_client_psk_recv_binders(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    RESULT_ENSURE_REF(conn);

    uint16_t binder_list_size = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(extension, &binder_list_size));

    uint8_t *binder_list_data = NULL;
    RESULT_ENSURE_REF(binder_list_data = s2n_stuffer_raw_read(extension, binder_list_size));

    struct s2n_blob binder_list_blob = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&binder_list_blob, binder_list_data, binder_list_size));

    struct s2n_stuffer binder_list = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init(&binder_list, &binder_list_blob));
    RESULT_GUARD_POSIX(s2n_stuffer_skip_write(&binder_list, binder_list_blob.size));

    /* Record the ClientHello message up to but not including the binder list.
     * This is required to calculate the binder for the chosen PSK. */
    struct s2n_blob partial_client_hello = { 0 };
    const struct s2n_stuffer *client_hello = &conn->handshake.io;
    uint32_t binders_size = binder_list_blob.size + SIZE_OF_BINDER_LIST_SIZE;
    RESULT_ENSURE_GTE(client_hello->write_cursor, binders_size);
    uint16_t partial_client_hello_size = client_hello->write_cursor - binders_size;
    RESULT_GUARD_POSIX(s2n_blob_slice(&client_hello->blob, &partial_client_hello, 0, partial_client_hello_size));

    return s2n_client_psk_recv_binder_list(conn, &partial_client_hello, &binder_list);
}

int s2n_client_psk_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.11
     *# The "pre_shared_key" extension MUST be the last extension in the
     *# ClientHello (this facilitates implementation as described below).
     *# Servers MUST check that it is the last extension and otherwise fail
     *# the handshake with an "illegal_parameter" alert.
     */
    s2n_extension_type_id psk_ext_id = 0;
    POSIX_GUARD(s2n_extension_supported_iana_value_to_id(TLS_EXTENSION_PRE_SHARED_KEY, &psk_ext_id));
    POSIX_ENSURE_NE(conn->client_hello.extensions.count, 0);
    uint16_t last_wire_index = conn->client_hello.extensions.count - 1;
    uint16_t extension_wire_index = conn->client_hello.extensions.parsed_extensions[psk_ext_id].wire_index;
    POSIX_ENSURE(extension_wire_index == last_wire_index, S2N_ERR_UNSUPPORTED_EXTENSION);

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.9
     *# If clients offer "pre_shared_key" without a "psk_key_exchange_modes" extension,
     *# servers MUST abort the handshake.
     *
     * We can safely do this check here because s2n_client_psk is
     * required to be the last extension sent in the list.
     */
    s2n_extension_type_id psk_ke_mode_ext_id = 0;
    POSIX_GUARD(s2n_extension_supported_iana_value_to_id(TLS_EXTENSION_PSK_KEY_EXCHANGE_MODES, &psk_ke_mode_ext_id));
    POSIX_ENSURE(S2N_CBIT_TEST(conn->extension_requests_received, psk_ke_mode_ext_id), S2N_ERR_MISSING_EXTENSION);

    if (conn->psk_params.psk_ke_mode == S2N_PSK_DHE_KE) {
        s2n_extension_type_id key_share_ext_id = 0;
        POSIX_GUARD(s2n_extension_supported_iana_value_to_id(TLS_EXTENSION_KEY_SHARE, &key_share_ext_id));
        /* A key_share extension must have been received in order to use a pre-shared key
         * in (EC)DHE key exchange mode.
         */
        POSIX_ENSURE(S2N_CBIT_TEST(conn->extension_requests_received, key_share_ext_id), S2N_ERR_MISSING_EXTENSION);
    } else {
        /* s2n currently only supports pre-shared keys in (EC)DHE key exchange mode. If we receive keys with any other
         * exchange mode we fall back to a full handshake.
         */
        return S2N_SUCCESS;
    }

    if (s2n_result_is_error(s2n_client_psk_recv_identities(conn, extension))) {
        /**
         *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.11
         *# If no acceptable PSKs are found, the server SHOULD perform a non-PSK
         *# handshake if possible.
         */
        conn->psk_params.chosen_psk = NULL;
    }

    if (conn->psk_params.chosen_psk) {
        /**
         *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.11
         *# Prior to accepting PSK key establishment, the server MUST validate
         *# the corresponding binder value (see Section 4.2.11.2 below).  If this
         *# value is not present or does not validate, the server MUST abort the
         *# handshake.
         */
        POSIX_GUARD_RESULT(s2n_client_psk_recv_binders(conn, extension));
    }

    /* At this point, we have either chosen a PSK or fallen back to a full handshake. */
    return S2N_SUCCESS;
}
