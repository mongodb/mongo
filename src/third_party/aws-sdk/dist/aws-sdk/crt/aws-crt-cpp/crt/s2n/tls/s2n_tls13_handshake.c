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

#include "tls/s2n_tls13_handshake.h"

#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_key_log.h"
#include "tls/s2n_security_policies.h"

static int s2n_zero_sequence_number(struct s2n_connection *conn, s2n_mode mode)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);
    struct s2n_blob sequence_number = { 0 };
    POSIX_GUARD_RESULT(s2n_connection_get_sequence_number(conn, mode, &sequence_number));
    POSIX_GUARD(s2n_blob_zero(&sequence_number));
    return S2N_SUCCESS;
}

int s2n_tls13_mac_verify(struct s2n_tls13_keys *keys, struct s2n_blob *finished_verify, struct s2n_blob *wire_verify)
{
    POSIX_ENSURE_REF(wire_verify->data);
    POSIX_ENSURE_EQ(wire_verify->size, keys->size);

    S2N_ERROR_IF(!s2n_constant_time_equals(finished_verify->data, wire_verify->data, keys->size), S2N_ERR_BAD_MESSAGE);

    return S2N_SUCCESS;
}

int s2n_tls13_keys_from_conn(struct s2n_tls13_keys *keys, struct s2n_connection *conn)
{
    POSIX_GUARD(s2n_tls13_keys_init(keys, conn->secure->cipher_suite->prf_alg));
    return S2N_SUCCESS;
}

int s2n_tls13_compute_ecc_shared_secret(struct s2n_connection *conn, struct s2n_blob *shared_secret)
{
    POSIX_ENSURE_REF(conn);

    const struct s2n_ecc_preferences *ecc_preferences = NULL;
    POSIX_GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_preferences));
    POSIX_ENSURE_REF(ecc_preferences);

    struct s2n_ecc_evp_params *server_key = &conn->kex_params.server_ecc_evp_params;
    POSIX_ENSURE_REF(server_key);
    POSIX_ENSURE_REF(server_key->negotiated_curve);

    struct s2n_ecc_evp_params *client_key = &conn->kex_params.client_ecc_evp_params;
    POSIX_ENSURE_REF(client_key);
    POSIX_ENSURE_REF(client_key->negotiated_curve);

    POSIX_ENSURE_EQ(server_key->negotiated_curve, client_key->negotiated_curve);

    if (conn->mode == S2N_CLIENT) {
        POSIX_GUARD(s2n_ecc_evp_compute_shared_secret_from_params(client_key, server_key, shared_secret));
    } else {
        POSIX_GUARD(s2n_ecc_evp_compute_shared_secret_from_params(server_key, client_key, shared_secret));
    }

    return S2N_SUCCESS;
}

/* Computes the ECDHE+PQKEM hybrid shared secret as defined in
 * https://tools.ietf.org/html/draft-stebila-tls-hybrid-design */
int s2n_tls13_compute_pq_hybrid_shared_secret(struct s2n_connection *conn, struct s2n_blob *shared_secret)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(shared_secret);

    /* conn->kex_params.server_ecc_evp_params should be set only during a classic/non-hybrid handshake */
    POSIX_ENSURE_EQ(NULL, conn->kex_params.server_ecc_evp_params.negotiated_curve);
    POSIX_ENSURE_EQ(NULL, conn->kex_params.server_ecc_evp_params.evp_pkey);

    struct s2n_kem_group_params *server_kem_group_params = &conn->kex_params.server_kem_group_params;
    POSIX_ENSURE_REF(server_kem_group_params);
    struct s2n_ecc_evp_params *server_ecc_params = &server_kem_group_params->ecc_params;
    POSIX_ENSURE_REF(server_ecc_params);

    struct s2n_kem_group_params *client_kem_group_params = &conn->kex_params.client_kem_group_params;
    POSIX_ENSURE_REF(client_kem_group_params);
    struct s2n_ecc_evp_params *client_ecc_params = &client_kem_group_params->ecc_params;
    POSIX_ENSURE_REF(client_ecc_params);

    DEFER_CLEANUP(struct s2n_blob ecdhe_shared_secret = { 0 }, s2n_free_or_wipe);

    /* Compute the ECDHE shared secret, and retrieve the PQ shared secret. */
    if (conn->mode == S2N_CLIENT) {
        POSIX_GUARD(s2n_ecc_evp_compute_shared_secret_from_params(client_ecc_params, server_ecc_params, &ecdhe_shared_secret));
    } else {
        POSIX_GUARD(s2n_ecc_evp_compute_shared_secret_from_params(server_ecc_params, client_ecc_params, &ecdhe_shared_secret));
    }

    struct s2n_blob *pq_shared_secret = &client_kem_group_params->kem_params.shared_secret;
    POSIX_ENSURE_REF(pq_shared_secret);
    POSIX_ENSURE_REF(pq_shared_secret->data);

    const struct s2n_kem_group *negotiated_kem_group = conn->kex_params.server_kem_group_params.kem_group;
    POSIX_ENSURE_REF(negotiated_kem_group);
    POSIX_ENSURE_REF(negotiated_kem_group->kem);

    POSIX_ENSURE_EQ(pq_shared_secret->size, negotiated_kem_group->kem->shared_secret_key_length);

    /* Construct the concatenated/hybrid shared secret */
    uint32_t hybrid_shared_secret_size = ecdhe_shared_secret.size + negotiated_kem_group->kem->shared_secret_key_length;
    POSIX_GUARD(s2n_alloc(shared_secret, hybrid_shared_secret_size));
    struct s2n_stuffer stuffer_combiner = { 0 };
    POSIX_GUARD(s2n_stuffer_init(&stuffer_combiner, shared_secret));

    if (negotiated_kem_group->send_kem_first) {
        POSIX_GUARD(s2n_stuffer_write(&stuffer_combiner, pq_shared_secret));
        POSIX_GUARD(s2n_stuffer_write(&stuffer_combiner, &ecdhe_shared_secret));
    } else {
        POSIX_GUARD(s2n_stuffer_write(&stuffer_combiner, &ecdhe_shared_secret));
        POSIX_GUARD(s2n_stuffer_write(&stuffer_combiner, pq_shared_secret));
    }

    return S2N_SUCCESS;
}

static int s2n_tls13_pq_hybrid_supported(struct s2n_connection *conn)
{
    return conn->kex_params.server_kem_group_params.kem_group != NULL;
}

int s2n_tls13_compute_shared_secret(struct s2n_connection *conn, struct s2n_blob *shared_secret)
{
    POSIX_ENSURE_REF(conn);

    if (s2n_tls13_pq_hybrid_supported(conn)) {
        POSIX_GUARD(s2n_tls13_compute_pq_hybrid_shared_secret(conn, shared_secret));
    } else {
        POSIX_GUARD(s2n_tls13_compute_ecc_shared_secret(conn, shared_secret));
    }

    POSIX_GUARD_RESULT(s2n_connection_wipe_all_keyshares(conn));

    /* It would make more sense to wipe the PSK secrets in s2n_tls13_handle_early_secret,
     * but at that point we don't know whether or not the server will request a HRR request
     * and we'll have to use the secrets again.
     *
     * Instead, wipe them here when we wipe all the other connection secrets. */
    POSIX_GUARD_RESULT(s2n_psk_parameters_wipe_secrets(&conn->psk_params));

    return S2N_SUCCESS;
}

int s2n_update_application_traffic_keys(struct s2n_connection *conn, s2n_mode mode, keyupdate_status status)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);
    POSIX_ENSURE_GTE(conn->actual_protocol_version, S2N_TLS13);

    /* get tls13 key context */
    s2n_tls13_connection_keys(keys, conn);

    struct s2n_session_key *old_key = NULL;
    struct s2n_blob old_app_secret = { 0 };
    struct s2n_blob app_iv = { 0 };

    if (mode == S2N_CLIENT) {
        old_key = &conn->secure->client_key;
        POSIX_GUARD(s2n_blob_init(&old_app_secret, conn->secrets.version.tls13.client_app_secret, keys.size));
        POSIX_GUARD(s2n_blob_init(&app_iv, conn->secure->client_implicit_iv, S2N_TLS13_FIXED_IV_LEN));
    } else {
        old_key = &conn->secure->server_key;
        POSIX_GUARD(s2n_blob_init(&old_app_secret, conn->secrets.version.tls13.server_app_secret, keys.size));
        POSIX_GUARD(s2n_blob_init(&app_iv, conn->secure->server_implicit_iv, S2N_TLS13_FIXED_IV_LEN));
    }

    /* Produce new application secret */
    s2n_stack_blob(app_secret_update, keys.size, S2N_TLS13_SECRET_MAX_LEN);

    /* Derives next generation of traffic secret */
    POSIX_GUARD(s2n_tls13_update_application_traffic_secret(&keys, &old_app_secret, &app_secret_update));

    s2n_tls13_key_blob(app_key, conn->secure->cipher_suite->record_alg->cipher->key_material_size);

    /* Derives next generation of traffic key */
    uint8_t *count = NULL;
    POSIX_GUARD(s2n_tls13_derive_traffic_keys(&keys, &app_secret_update, &app_key, &app_iv));
    if (status == RECEIVING) {
        POSIX_GUARD_RESULT(conn->secure->cipher_suite->record_alg->cipher->set_decryption_key(old_key, &app_key));
        count = &conn->recv_key_updated;
    } else {
        POSIX_GUARD_RESULT(conn->secure->cipher_suite->record_alg->cipher->set_encryption_key(old_key, &app_key));
        count = &conn->send_key_updated;
    }

    /* Increment the count.
     * Don't treat overflows as errors-- we only do best-effort reporting.
     */
    *count = MIN(UINT8_MAX, *count + 1);

    /* According to https://tools.ietf.org/html/rfc8446#section-5.3:
     * Each sequence number is set to zero at the beginning of a connection and
     * whenever the key is changed; the first record transmitted under a particular traffic key
     * MUST use sequence number 0.
     */
    POSIX_GUARD(s2n_zero_sequence_number(conn, mode));

    /* Save updated secret */
    struct s2n_stuffer old_secret_stuffer = { 0 };
    POSIX_GUARD(s2n_stuffer_init(&old_secret_stuffer, &old_app_secret));
    POSIX_GUARD(s2n_stuffer_write_bytes(&old_secret_stuffer, app_secret_update.data, keys.size));

    return S2N_SUCCESS;
}
