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

#include "crypto/s2n_tls13_keys.h"
#include "tls/extensions/s2n_extension_type.h"
#include "tls/s2n_handshake.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls13_handshake.h"
#include "tls/s2n_tls13_secrets.h"
#include "utils/s2n_array.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

#define S2N_HASH_ALG_COUNT S2N_HASH_SENTINEL

S2N_RESULT s2n_psk_init(struct s2n_psk *psk, s2n_psk_type type)
{
    RESULT_ENSURE_MUT(psk);

    RESULT_CHECKED_MEMSET(psk, 0, sizeof(struct s2n_psk));
    psk->hmac_alg = S2N_HMAC_SHA256;
    psk->type = type;

    return S2N_RESULT_OK;
}

struct s2n_psk *s2n_external_psk_new()
{
    DEFER_CLEANUP(struct s2n_blob mem = { 0 }, s2n_free);
    PTR_GUARD_POSIX(s2n_alloc(&mem, sizeof(struct s2n_psk)));

    struct s2n_psk *psk = (struct s2n_psk *) (void *) mem.data;
    PTR_GUARD_RESULT(s2n_psk_init(psk, S2N_PSK_TYPE_EXTERNAL));

    ZERO_TO_DISABLE_DEFER_CLEANUP(mem);
    return psk;
}

int s2n_psk_set_identity(struct s2n_psk *psk, const uint8_t *identity, uint16_t identity_size)
{
    POSIX_ENSURE_REF(psk);
    POSIX_ENSURE_REF(identity);
    POSIX_ENSURE(identity_size != 0, S2N_ERR_INVALID_ARGUMENT);

    POSIX_GUARD(s2n_realloc(&psk->identity, identity_size));
    POSIX_CHECKED_MEMCPY(psk->identity.data, identity, identity_size);

    return S2N_SUCCESS;
}

int s2n_psk_set_secret(struct s2n_psk *psk, const uint8_t *secret, uint16_t secret_size)
{
    POSIX_ENSURE_REF(psk);
    POSIX_ENSURE_REF(secret);
    POSIX_ENSURE(secret_size != 0, S2N_ERR_INVALID_ARGUMENT);

    POSIX_GUARD(s2n_realloc(&psk->secret, secret_size));
    POSIX_CHECKED_MEMCPY(psk->secret.data, secret, secret_size);

    return S2N_SUCCESS;
}

S2N_RESULT s2n_psk_clone(struct s2n_psk *new_psk, struct s2n_psk *original_psk)
{
    if (original_psk == NULL) {
        return S2N_RESULT_OK;
    }
    RESULT_ENSURE_REF(new_psk);

    struct s2n_psk psk_copy = *new_psk;

    /* Copy all fields from the old_config EXCEPT the blobs, which we need to reallocate. */
    *new_psk = *original_psk;
    new_psk->identity = psk_copy.identity;
    new_psk->secret = psk_copy.secret;
    new_psk->early_secret = psk_copy.early_secret;
    new_psk->early_data_config = psk_copy.early_data_config;

    /* Clone / realloc blobs */
    RESULT_GUARD_POSIX(s2n_psk_set_identity(new_psk, original_psk->identity.data, original_psk->identity.size));
    RESULT_GUARD_POSIX(s2n_psk_set_secret(new_psk, original_psk->secret.data, original_psk->secret.size));
    RESULT_GUARD_POSIX(s2n_realloc(&new_psk->early_secret, original_psk->early_secret.size));
    RESULT_CHECKED_MEMCPY(new_psk->early_secret.data, original_psk->early_secret.data, original_psk->early_secret.size);
    RESULT_GUARD(s2n_early_data_config_clone(new_psk, &original_psk->early_data_config));

    return S2N_RESULT_OK;
}

S2N_CLEANUP_RESULT s2n_psk_wipe(struct s2n_psk *psk)
{
    if (psk == NULL) {
        return S2N_RESULT_OK;
    }

    RESULT_GUARD_POSIX(s2n_free(&psk->early_secret));
    RESULT_GUARD_POSIX(s2n_free(&psk->identity));
    RESULT_GUARD_POSIX(s2n_free(&psk->secret));
    RESULT_GUARD(s2n_early_data_config_free(&psk->early_data_config));

    return S2N_RESULT_OK;
}

int s2n_psk_free(struct s2n_psk **psk)
{
    if (psk == NULL) {
        return S2N_SUCCESS;
    }
    POSIX_GUARD_RESULT(s2n_psk_wipe(*psk));
    return s2n_free_object((uint8_t **) psk, sizeof(struct s2n_psk));
}

S2N_RESULT s2n_psk_parameters_init(struct s2n_psk_parameters *params)
{
    RESULT_ENSURE_REF(params);
    RESULT_CHECKED_MEMSET(params, 0, sizeof(struct s2n_psk_parameters));
    RESULT_GUARD(s2n_array_init(&params->psk_list, sizeof(struct s2n_psk)));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_psk_offered_psk_size(struct s2n_psk *psk, uint32_t *size)
{
    *size = sizeof(uint16_t)   /* identity size */
            + sizeof(uint32_t) /* obfuscated ticket age */
            + sizeof(uint8_t); /* binder size */

    RESULT_GUARD_POSIX(s2n_add_overflow(*size, psk->identity.size, size));

    uint8_t binder_size = 0;
    RESULT_GUARD_POSIX(s2n_hmac_digest_size(psk->hmac_alg, &binder_size));
    RESULT_GUARD_POSIX(s2n_add_overflow(*size, binder_size, size));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_psk_parameters_offered_psks_size(struct s2n_psk_parameters *params, uint32_t *size)
{
    RESULT_ENSURE_REF(params);
    RESULT_ENSURE_REF(size);

    *size = sizeof(uint16_t) /* identity list size */
            + sizeof(uint16_t) /* binder list size */;

    for (uint32_t i = 0; i < params->psk_list.len; i++) {
        struct s2n_psk *psk = NULL;
        RESULT_GUARD(s2n_array_get(&params->psk_list, i, (void **) &psk));
        RESULT_ENSURE_REF(psk);

        uint32_t psk_size = 0;
        RESULT_GUARD(s2n_psk_offered_psk_size(psk, &psk_size));
        RESULT_GUARD_POSIX(s2n_add_overflow(*size, psk_size, size));
    }
    return S2N_RESULT_OK;
}

S2N_CLEANUP_RESULT s2n_psk_parameters_wipe(struct s2n_psk_parameters *params)
{
    RESULT_ENSURE_REF(params);

    for (size_t i = 0; i < params->psk_list.len; i++) {
        struct s2n_psk *psk = NULL;
        RESULT_GUARD(s2n_array_get(&params->psk_list, i, (void **) &psk));
        RESULT_GUARD(s2n_psk_wipe(psk));
    }
    RESULT_GUARD_POSIX(s2n_free(&params->psk_list.mem));
    RESULT_GUARD(s2n_psk_parameters_init(params));

    return S2N_RESULT_OK;
}

S2N_CLEANUP_RESULT s2n_psk_parameters_wipe_secrets(struct s2n_psk_parameters *params)
{
    RESULT_ENSURE_REF(params);

    for (size_t i = 0; i < params->psk_list.len; i++) {
        struct s2n_psk *psk = NULL;
        RESULT_GUARD(s2n_array_get(&params->psk_list, i, (void **) &psk));
        RESULT_ENSURE_REF(psk);
        RESULT_GUARD_POSIX(s2n_free(&psk->early_secret));
        RESULT_GUARD_POSIX(s2n_free(&psk->secret));
    }

    return S2N_RESULT_OK;
}

bool s2n_offered_psk_list_has_next(struct s2n_offered_psk_list *psk_list)
{
    return psk_list != NULL && s2n_stuffer_data_available(&psk_list->wire_data) > 0;
}

S2N_RESULT s2n_offered_psk_list_read_next(struct s2n_offered_psk_list *psk_list, struct s2n_offered_psk *psk)
{
    RESULT_ENSURE_REF(psk_list);
    RESULT_ENSURE_REF(psk_list->conn);
    RESULT_ENSURE_MUT(psk);

    uint16_t identity_size = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(&psk_list->wire_data, &identity_size));
    RESULT_ENSURE_GT(identity_size, 0);

    uint8_t *identity_data = NULL;
    identity_data = s2n_stuffer_raw_read(&psk_list->wire_data, identity_size);
    RESULT_ENSURE_REF(identity_data);

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.11
     *# For identities established externally, an obfuscated_ticket_age of 0 SHOULD be
     *# used, and servers MUST ignore the value.
     */
    if (psk_list->conn->psk_params.type == S2N_PSK_TYPE_EXTERNAL) {
        RESULT_GUARD_POSIX(s2n_stuffer_skip_read(&psk_list->wire_data, sizeof(uint32_t)));
    } else {
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint32(&psk_list->wire_data, &psk->obfuscated_ticket_age));
    }

    RESULT_GUARD_POSIX(s2n_blob_init(&psk->identity, identity_data, identity_size));
    psk->wire_index = psk_list->wire_index;

    RESULT_ENSURE(psk_list->wire_index < UINT16_MAX, S2N_ERR_INTEGER_OVERFLOW);
    psk_list->wire_index++;
    return S2N_RESULT_OK;
}

int s2n_offered_psk_list_next(struct s2n_offered_psk_list *psk_list, struct s2n_offered_psk *psk)
{
    POSIX_ENSURE_REF(psk_list);
    POSIX_ENSURE_REF(psk);
    *psk = (struct s2n_offered_psk){ 0 };
    POSIX_ENSURE(s2n_offered_psk_list_has_next(psk_list), S2N_ERR_STUFFER_OUT_OF_DATA);
    POSIX_ENSURE(s2n_result_is_ok(s2n_offered_psk_list_read_next(psk_list, psk)), S2N_ERR_BAD_MESSAGE);
    return S2N_SUCCESS;
}

int s2n_offered_psk_list_reread(struct s2n_offered_psk_list *psk_list)
{
    POSIX_ENSURE_REF(psk_list);
    psk_list->wire_index = 0;
    return s2n_stuffer_reread(&psk_list->wire_data);
}

/* Match a PSK identity received from the client against the server's known PSK identities.
 * This method compares a single client identity to all server identities.
 *
 * While both the client's offered identities and whether a match was found are public, we should make an attempt
 * to keep the server's known identities a secret. We will make comparisons to the server's identities constant
 * time (to hide partial matches) and not end the search early when a match is found (to hide the ordering).
 *
 * Keeping these comparisons constant time is not high priority. There's no known attack using these timings,
 * and an attacker could probably guess the server's known identities just by observing the public identities
 * sent by clients.
 */
static S2N_RESULT s2n_match_psk_identity(struct s2n_array *known_psks, const struct s2n_blob *wire_identity,
        struct s2n_psk **match)
{
    RESULT_ENSURE_REF(match);
    RESULT_ENSURE_REF(wire_identity);
    RESULT_ENSURE_REF(known_psks);
    *match = NULL;
    for (size_t i = 0; i < known_psks->len; i++) {
        struct s2n_psk *psk = NULL;
        RESULT_GUARD(s2n_array_get(known_psks, i, (void **) &psk));
        RESULT_ENSURE_REF(psk);
        RESULT_ENSURE_REF(psk->identity.data);
        RESULT_ENSURE_REF(wire_identity->data);
        uint32_t compare_size = MIN(wire_identity->size, psk->identity.size);
        if (s2n_constant_time_equals(psk->identity.data, wire_identity->data, compare_size)
                & (psk->identity.size == wire_identity->size) & (!*match)) {
            *match = psk;
        }
    }
    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
 *# For PSKs provisioned via NewSessionTicket, a server MUST validate
 *# that the ticket age for the selected PSK identity (computed by
 *# subtracting ticket_age_add from PskIdentity.obfuscated_ticket_age
 *# modulo 2^32) is within a small tolerance of the time since the ticket
 *# was issued (see Section 8).
 **/
static S2N_RESULT s2n_validate_ticket_lifetime(struct s2n_connection *conn, uint32_t obfuscated_ticket_age, uint32_t ticket_age_add)
{
    RESULT_ENSURE_REF(conn);

    if (conn->psk_params.type == S2N_PSK_TYPE_EXTERNAL) {
        return S2N_RESULT_OK;
    }

    /* Subtract the ticket_age_add value from the ticket age in milliseconds. The resulting uint32_t value
     * may wrap, resulting in the modulo 2^32 operation. */
    uint32_t ticket_age_in_millis = obfuscated_ticket_age - ticket_age_add;
    uint32_t session_lifetime_in_millis = conn->config->session_state_lifetime_in_nanos / ONE_MILLISEC_IN_NANOS;
    RESULT_ENSURE(ticket_age_in_millis < session_lifetime_in_millis, S2N_ERR_INVALID_SESSION_TICKET);

    return S2N_RESULT_OK;
}

int s2n_offered_psk_list_choose_psk(struct s2n_offered_psk_list *psk_list, struct s2n_offered_psk *psk)
{
    POSIX_ENSURE_REF(psk_list);
    POSIX_ENSURE_REF(psk_list->conn);

    struct s2n_psk_parameters *psk_params = &psk_list->conn->psk_params;
    struct s2n_stuffer ticket_stuffer = { 0 };

    if (!psk) {
        psk_params->chosen_psk = NULL;
        return S2N_SUCCESS;
    }

    if (psk_params->type == S2N_PSK_TYPE_RESUMPTION && psk_list->conn->config->use_tickets) {
        POSIX_GUARD(s2n_stuffer_init(&ticket_stuffer, &psk->identity));
        POSIX_GUARD(s2n_stuffer_skip_write(&ticket_stuffer, psk->identity.size));

        /* s2n_resume_decrypt_session_ticket appends a new PSK with the decrypted values. */
        POSIX_GUARD_RESULT(s2n_resume_decrypt_session_ticket(psk_list->conn, &ticket_stuffer));
    }

    struct s2n_psk *chosen_psk = NULL;
    POSIX_GUARD_RESULT(s2n_match_psk_identity(&psk_params->psk_list, &psk->identity, &chosen_psk));
    POSIX_ENSURE_REF(chosen_psk);
    POSIX_GUARD_RESULT(s2n_validate_ticket_lifetime(psk_list->conn, psk->obfuscated_ticket_age, chosen_psk->ticket_age_add));
    psk_params->chosen_psk = chosen_psk;
    psk_params->chosen_psk_wire_index = psk->wire_index;

    return S2N_SUCCESS;
}

struct s2n_offered_psk *s2n_offered_psk_new()
{
    DEFER_CLEANUP(struct s2n_blob mem = { 0 }, s2n_free);
    PTR_GUARD_POSIX(s2n_alloc(&mem, sizeof(struct s2n_offered_psk)));
    PTR_GUARD_POSIX(s2n_blob_zero(&mem));

    struct s2n_offered_psk *psk = (struct s2n_offered_psk *) (void *) mem.data;

    ZERO_TO_DISABLE_DEFER_CLEANUP(mem);
    return psk;
}

int s2n_offered_psk_free(struct s2n_offered_psk **psk)
{
    if (psk == NULL) {
        return S2N_SUCCESS;
    }
    return s2n_free_object((uint8_t **) psk, sizeof(struct s2n_offered_psk));
}

int s2n_offered_psk_get_identity(struct s2n_offered_psk *psk, uint8_t **identity, uint16_t *size)
{
    POSIX_ENSURE_REF(psk);
    POSIX_ENSURE_REF(identity);
    POSIX_ENSURE_REF(size);
    *identity = psk->identity.data;
    *size = psk->identity.size;
    return S2N_SUCCESS;
}

/* The binder hash is computed by hashing the concatenation of the current transcript
 * and a partial ClientHello that does not include the binders themselves.
 */
int s2n_psk_calculate_binder_hash(struct s2n_connection *conn, s2n_hmac_algorithm hmac_alg,
        const struct s2n_blob *partial_client_hello, struct s2n_blob *output_binder_hash)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(partial_client_hello);
    POSIX_ENSURE_REF(output_binder_hash);
    struct s2n_handshake_hashes *hashes = conn->handshake.hashes;
    POSIX_ENSURE_REF(hashes);

    /* Retrieve the current transcript.
     * The current transcript will be empty unless this handshake included a HelloRetryRequest. */
    s2n_hash_algorithm hash_alg = S2N_HASH_NONE;
    struct s2n_hash_state *hash_state = &hashes->hash_workspace;
    POSIX_GUARD(s2n_hmac_hash_alg(hmac_alg, &hash_alg));
    POSIX_GUARD_RESULT(s2n_handshake_copy_hash_state(conn, hash_alg, hash_state));

    /* Add the partial client hello to the transcript. */
    POSIX_GUARD(s2n_hash_update(hash_state, partial_client_hello->data, partial_client_hello->size));

    /* Get the transcript digest */
    POSIX_GUARD(s2n_hash_digest(hash_state, output_binder_hash->data, output_binder_hash->size));

    return S2N_SUCCESS;
}

/* The binder is computed in the same way as the Finished message
 * (https://tools.ietf.org/html/rfc8446#section-4.4.4) but with the BaseKey being the binder_key
 * derived via the key schedule from the corresponding PSK which is being offered
 * (https://tools.ietf.org/html/rfc8446#section-7.1)
 */
int s2n_psk_calculate_binder(struct s2n_psk *psk, const struct s2n_blob *binder_hash,
        struct s2n_blob *output_binder)
{
    POSIX_ENSURE_REF(psk);
    POSIX_ENSURE_REF(binder_hash);
    POSIX_ENSURE_REF(output_binder);

    DEFER_CLEANUP(struct s2n_tls13_keys psk_keys, s2n_tls13_keys_free);
    POSIX_GUARD(s2n_tls13_keys_init(&psk_keys, psk->hmac_alg));
    POSIX_ENSURE_EQ(binder_hash->size, psk_keys.size);
    POSIX_ENSURE_EQ(output_binder->size, psk_keys.size);

    /* Derive the binder key */
    POSIX_GUARD_RESULT(s2n_derive_binder_key(psk, &psk_keys.derive_secret));
    POSIX_GUARD(s2n_blob_init(&psk_keys.extract_secret, psk->early_secret.data, psk_keys.size));
    struct s2n_blob *binder_key = &psk_keys.derive_secret;

    /* Expand the binder key into the finished key */
    s2n_tls13_key_blob(finished_key, psk_keys.size);
    POSIX_GUARD(s2n_tls13_derive_finished_key(&psk_keys, binder_key, &finished_key));

    /* HMAC the binder hash with the binder finished key */
    POSIX_GUARD(s2n_hkdf_extract(&psk_keys.hmac, psk_keys.hmac_algorithm, &finished_key, binder_hash, output_binder));

    return S2N_SUCCESS;
}

int s2n_psk_verify_binder(struct s2n_connection *conn, struct s2n_psk *psk,
        const struct s2n_blob *partial_client_hello, struct s2n_blob *binder_to_verify)
{
    POSIX_ENSURE_REF(psk);
    POSIX_ENSURE_REF(binder_to_verify);

    DEFER_CLEANUP(struct s2n_tls13_keys psk_keys, s2n_tls13_keys_free);
    POSIX_GUARD(s2n_tls13_keys_init(&psk_keys, psk->hmac_alg));
    POSIX_ENSURE_EQ(binder_to_verify->size, psk_keys.size);

    /* Calculate the binder hash from the transcript */
    s2n_tls13_key_blob(binder_hash, psk_keys.size);
    POSIX_GUARD(s2n_psk_calculate_binder_hash(conn, psk->hmac_alg, partial_client_hello, &binder_hash));

    /* Calculate the expected binder from the binder hash */
    s2n_tls13_key_blob(expected_binder, psk_keys.size);
    POSIX_GUARD(s2n_psk_calculate_binder(psk, &binder_hash, &expected_binder));

    /* Verify the expected binder matches the given binder.
     * This operation must be constant time. */
    POSIX_GUARD(s2n_tls13_mac_verify(&psk_keys, &expected_binder, binder_to_verify));

    return S2N_SUCCESS;
}

static S2N_RESULT s2n_psk_write_binder(struct s2n_connection *conn, struct s2n_psk *psk,
        const struct s2n_blob *binder_hash, struct s2n_stuffer *out)
{
    RESULT_ENSURE_REF(binder_hash);

    struct s2n_blob binder = { 0 };
    uint8_t binder_data[S2N_TLS13_SECRET_MAX_LEN] = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&binder, binder_data, binder_hash->size));

    RESULT_GUARD_POSIX(s2n_psk_calculate_binder(psk, binder_hash, &binder));
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint8(out, binder.size));
    RESULT_GUARD_POSIX(s2n_stuffer_write(out, &binder));

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_psk_write_binder_list(struct s2n_connection *conn, const struct s2n_blob *partial_client_hello,
        struct s2n_stuffer *out)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(partial_client_hello);
    RESULT_ENSURE_REF(conn->secure);

    struct s2n_psk_parameters *psk_params = &conn->psk_params;
    struct s2n_array *psk_list = &psk_params->psk_list;

    /* Setup memory to hold the binder hashes. We potentially need one for
     * every hash algorithm. */
    uint8_t binder_hashes_data[S2N_HASH_ALG_COUNT][S2N_TLS13_SECRET_MAX_LEN] = { 0 };
    struct s2n_blob binder_hashes[S2N_HASH_ALG_COUNT] = { 0 };

    struct s2n_stuffer_reservation binder_list_size = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_reserve_uint16(out, &binder_list_size));

    /* Write binder for every psk */
    for (size_t i = 0; i < psk_list->len; i++) {
        struct s2n_psk *psk = NULL;
        RESULT_GUARD(s2n_array_get(psk_list, i, (void **) &psk));
        RESULT_ENSURE_REF(psk);

        /**
         *= https://www.rfc-editor.org/rfc/rfc8446#section-4.1.4
         *# In addition, in its updated ClientHello, the client SHOULD NOT offer
         *# any pre-shared keys associated with a hash other than that of the
         *# selected cipher suite.  This allows the client to avoid having to
         *# compute partial hash transcripts for multiple hashes in the second
         *# ClientHello.
         */
        if (s2n_is_hello_retry_handshake(conn) && conn->secure->cipher_suite->prf_alg != psk->hmac_alg) {
            continue;
        }

        /* Retrieve or calculate the binder hash. */
        struct s2n_blob *binder_hash = &binder_hashes[psk->hmac_alg];
        if (binder_hash->size == 0) {
            uint8_t hash_size = 0;
            RESULT_GUARD_POSIX(s2n_hmac_digest_size(psk->hmac_alg, &hash_size));
            RESULT_GUARD_POSIX(s2n_blob_init(binder_hash, binder_hashes_data[psk->hmac_alg], hash_size));
            RESULT_GUARD_POSIX(s2n_psk_calculate_binder_hash(conn, psk->hmac_alg, partial_client_hello, binder_hash));
        }

        RESULT_GUARD(s2n_psk_write_binder(conn, psk, binder_hash, out));
    }
    RESULT_GUARD_POSIX(s2n_stuffer_write_vector_size(&binder_list_size));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_finish_psk_extension(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);

    if (!conn->psk_params.binder_list_size) {
        return S2N_RESULT_OK;
    }

    struct s2n_stuffer *client_hello = &conn->handshake.io;
    struct s2n_psk_parameters *psk_params = &conn->psk_params;

    /* Fill in the correct message size. */
    RESULT_GUARD_POSIX(s2n_handshake_finish_header(client_hello));

    /* Remove the empty space allocated for the binder list.
     * It was originally added to ensure the extension / extension list / message sizes
     * were properly calculated. */
    RESULT_GUARD_POSIX(s2n_stuffer_wipe_n(client_hello, psk_params->binder_list_size));

    /* Store the partial client hello for use in calculating the binder hash. */
    struct s2n_blob partial_client_hello = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&partial_client_hello, client_hello->blob.data,
            s2n_stuffer_data_available(client_hello)));

    RESULT_GUARD(s2n_psk_write_binder_list(conn, &partial_client_hello, client_hello));

    /* Reset binder list size.
     * This is important because the psk extension can be removed during a retry.
     */
    conn->psk_params.binder_list_size = 0;

    return S2N_RESULT_OK;
}

int s2n_psk_set_hmac(struct s2n_psk *psk, s2n_psk_hmac hmac)
{
    POSIX_ENSURE_REF(psk);
    switch (hmac) {
        case S2N_PSK_HMAC_SHA256:
            psk->hmac_alg = S2N_HMAC_SHA256;
            break;
        case S2N_PSK_HMAC_SHA384:
            psk->hmac_alg = S2N_HMAC_SHA384;
            break;
        default:
            POSIX_BAIL(S2N_ERR_HMAC_INVALID_ALGORITHM);
    }
    return S2N_SUCCESS;
}

S2N_RESULT s2n_connection_set_psk_type(struct s2n_connection *conn, s2n_psk_type type)
{
    RESULT_ENSURE_REF(conn);
    if (conn->psk_params.psk_list.len != 0) {
        RESULT_ENSURE(conn->psk_params.type == type, S2N_ERR_PSK_MODE);
    }
    conn->psk_params.type = type;
    return S2N_RESULT_OK;
}

int s2n_connection_append_psk(struct s2n_connection *conn, struct s2n_psk *input_psk)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(input_psk);
    POSIX_GUARD_RESULT(s2n_connection_set_psk_type(conn, input_psk->type));

    struct s2n_array *psk_list = &conn->psk_params.psk_list;

    /* Check for duplicate identities */
    for (uint32_t j = 0; j < psk_list->len; j++) {
        struct s2n_psk *existing_psk = NULL;
        POSIX_GUARD_RESULT(s2n_array_get(psk_list, j, (void **) &existing_psk));
        POSIX_ENSURE_REF(existing_psk);

        bool duplicate = existing_psk->identity.size == input_psk->identity.size
                && memcmp(existing_psk->identity.data, input_psk->identity.data, existing_psk->identity.size) == 0;
        POSIX_ENSURE(!duplicate, S2N_ERR_DUPLICATE_PSK_IDENTITIES);
    }

    /* Verify the PSK list will fit in the ClientHello pre_shared_key extension */
    if (conn->mode == S2N_CLIENT) {
        uint32_t list_size = 0;
        POSIX_GUARD_RESULT(s2n_psk_parameters_offered_psks_size(&conn->psk_params, &list_size));

        uint32_t psk_size = 0;
        POSIX_GUARD_RESULT(s2n_psk_offered_psk_size(input_psk, &psk_size));

        POSIX_ENSURE(list_size + psk_size + S2N_EXTENSION_HEADER_LENGTH <= UINT16_MAX, S2N_ERR_OFFERED_PSKS_TOO_LONG);
    }

    DEFER_CLEANUP(struct s2n_psk new_psk = { 0 }, s2n_psk_wipe);
    POSIX_ENSURE(s2n_result_is_ok(s2n_psk_clone(&new_psk, input_psk)), S2N_ERR_INVALID_ARGUMENT);
    POSIX_GUARD_RESULT(s2n_array_insert_and_copy(psk_list, psk_list->len, &new_psk));

    ZERO_TO_DISABLE_DEFER_CLEANUP(new_psk);
    return S2N_SUCCESS;
}

int s2n_config_set_psk_mode(struct s2n_config *config, s2n_psk_mode mode)
{
    POSIX_ENSURE_REF(config);
    config->psk_mode = mode;
    return S2N_SUCCESS;
}

int s2n_connection_set_psk_mode(struct s2n_connection *conn, s2n_psk_mode mode)
{
    POSIX_ENSURE_REF(conn);
    s2n_psk_type type = 0;
    switch (mode) {
        case S2N_PSK_MODE_RESUMPTION:
            type = S2N_PSK_TYPE_RESUMPTION;
            break;
        case S2N_PSK_MODE_EXTERNAL:
            type = S2N_PSK_TYPE_EXTERNAL;
            break;
        default:
            POSIX_BAIL(S2N_ERR_INVALID_ARGUMENT);
            break;
    }
    POSIX_GUARD_RESULT(s2n_connection_set_psk_type(conn, type));
    conn->psk_mode_overridden = true;
    return S2N_SUCCESS;
}

int s2n_connection_get_negotiated_psk_identity_length(struct s2n_connection *conn, uint16_t *identity_length)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(identity_length);

    struct s2n_psk *chosen_psk = conn->psk_params.chosen_psk;

    if (chosen_psk == NULL) {
        *identity_length = 0;
    } else {
        *identity_length = chosen_psk->identity.size;
    }

    return S2N_SUCCESS;
}

int s2n_connection_get_negotiated_psk_identity(struct s2n_connection *conn, uint8_t *identity,
        uint16_t max_identity_length)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(identity);

    struct s2n_psk *chosen_psk = conn->psk_params.chosen_psk;

    if (chosen_psk == NULL) {
        return S2N_SUCCESS;
    }

    POSIX_ENSURE(chosen_psk->identity.size <= max_identity_length, S2N_ERR_INSUFFICIENT_MEM_SIZE);
    POSIX_CHECKED_MEMCPY(identity, chosen_psk->identity.data, chosen_psk->identity.size);

    return S2N_SUCCESS;
}

S2N_RESULT s2n_psk_validate_keying_material(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);

    struct s2n_psk *chosen_psk = conn->psk_params.chosen_psk;
    if (!chosen_psk || chosen_psk->type != S2N_PSK_TYPE_RESUMPTION) {
        return S2N_RESULT_OK;
    }

    /*
     * The minimum ticket lifetime is 1s, because ticket_lifetime is given
     * in seconds and 0 indicates that the ticket should be immediately discarded.
     */
    uint32_t min_lifetime = ONE_SEC_IN_NANOS;

    uint64_t current_time = 0;
    RESULT_GUARD(s2n_config_wall_clock(conn->config, &current_time));
    RESULT_ENSURE(chosen_psk->keying_material_expiration > current_time + min_lifetime, S2N_ERR_KEYING_MATERIAL_EXPIRED);

    return S2N_RESULT_OK;
}
