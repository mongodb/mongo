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

#include "tls/s2n_kex.h"

#include "crypto/s2n_pq.h"
#include "tls/s2n_cipher_preferences.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_client_key_exchange.h"
#include "tls/s2n_kem.h"
#include "tls/s2n_security_policies.h"
#include "tls/s2n_server_key_exchange.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_safety.h"

static S2N_RESULT s2n_check_tls13(const struct s2n_cipher_suite *cipher_suite,
        struct s2n_connection *conn, bool *is_supported)
{
    RESULT_ENSURE_REF(is_supported);
    *is_supported = (s2n_connection_get_protocol_version(conn) >= S2N_TLS13);
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_check_rsa_key(const struct s2n_cipher_suite *cipher_suite, struct s2n_connection *conn, bool *is_supported)
{
    RESULT_ENSURE_REF(cipher_suite);
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(is_supported);

    *is_supported = s2n_get_compatible_cert_chain_and_key(conn, S2N_PKEY_TYPE_RSA) != NULL;

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_check_dhe(const struct s2n_cipher_suite *cipher_suite, struct s2n_connection *conn, bool *is_supported)
{
    RESULT_ENSURE_REF(cipher_suite);
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->config);
    RESULT_ENSURE_REF(is_supported);

    *is_supported = conn->config->dhparams != NULL;

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_check_ecdhe(const struct s2n_cipher_suite *cipher_suite, struct s2n_connection *conn, bool *is_supported)
{
    RESULT_ENSURE_REF(cipher_suite);
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(is_supported);

    *is_supported = conn->kex_params.server_ecc_evp_params.negotiated_curve != NULL;

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_check_kem(const struct s2n_cipher_suite *cipher_suite, struct s2n_connection *conn, bool *is_supported)
{
    RESULT_ENSURE_REF(cipher_suite);
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(is_supported);

    /* If any of the necessary conditions are not met, we will return early and indicate KEM is not supported. */
    *is_supported = false;

    const struct s2n_kem_preferences *kem_preferences = NULL;
    RESULT_GUARD_POSIX(s2n_connection_get_kem_preferences(conn, &kem_preferences));
    RESULT_ENSURE_REF(kem_preferences);

    if (!s2n_pq_is_enabled() || kem_preferences->kem_count == 0) {
        return S2N_RESULT_OK;
    }

    const struct s2n_iana_to_kem *supported_params = NULL;
    if (s2n_cipher_suite_to_kem(cipher_suite->iana_value, &supported_params) != S2N_SUCCESS) {
        return S2N_RESULT_OK;
    }

    RESULT_ENSURE_REF(supported_params);
    if (supported_params->kem_count == 0) {
        return S2N_RESULT_OK;
    }

    struct s2n_blob *client_kem_pref_list = &(conn->kex_params.client_pq_kem_extension);
    const struct s2n_kem *chosen_kem = NULL;
    if (client_kem_pref_list == NULL || client_kem_pref_list->data == NULL) {
        /* If the client did not send a PQ KEM extension, then the server can pick its preferred parameter */
        if (s2n_choose_kem_without_peer_pref_list(
                    cipher_suite->iana_value, kem_preferences->kems, kem_preferences->kem_count, &chosen_kem)
                != S2N_SUCCESS) {
            return S2N_RESULT_OK;
        }
    } else {
        /* If the client did send a PQ KEM extension, then the server must find a mutually supported parameter. */
        if (s2n_choose_kem_with_peer_pref_list(
                    cipher_suite->iana_value, client_kem_pref_list, kem_preferences->kems, kem_preferences->kem_count, &chosen_kem)
                != S2N_SUCCESS) {
            return S2N_RESULT_OK;
        }
    }

    *is_supported = chosen_kem != NULL;
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_configure_kem(const struct s2n_cipher_suite *cipher_suite, struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(cipher_suite);
    RESULT_ENSURE_REF(conn);

    RESULT_ENSURE(s2n_pq_is_enabled(), S2N_ERR_UNIMPLEMENTED);

    const struct s2n_kem_preferences *kem_preferences = NULL;
    RESULT_GUARD_POSIX(s2n_connection_get_kem_preferences(conn, &kem_preferences));
    RESULT_ENSURE_REF(kem_preferences);

    struct s2n_blob *proposed_kems = &(conn->kex_params.client_pq_kem_extension);
    const struct s2n_kem *chosen_kem = NULL;
    if (proposed_kems == NULL || proposed_kems->data == NULL) {
        /* If the client did not send a PQ KEM extension, then the server can pick its preferred parameter */
        RESULT_GUARD_POSIX(s2n_choose_kem_without_peer_pref_list(cipher_suite->iana_value, kem_preferences->kems,
                kem_preferences->kem_count, &chosen_kem));
    } else {
        /* If the client did send a PQ KEM extension, then the server must find a mutually supported parameter. */
        RESULT_GUARD_POSIX(s2n_choose_kem_with_peer_pref_list(cipher_suite->iana_value, proposed_kems, kem_preferences->kems,
                kem_preferences->kem_count, &chosen_kem));
    }

    conn->kex_params.kem_params.kem = chosen_kem;
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_check_hybrid_ecdhe_kem(const struct s2n_cipher_suite *cipher_suite, struct s2n_connection *conn, bool *is_supported)
{
    RESULT_ENSURE_REF(cipher_suite);
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(is_supported);

    bool ecdhe_supported = false;
    bool kem_supported = false;
    RESULT_GUARD(s2n_check_ecdhe(cipher_suite, conn, &ecdhe_supported));
    RESULT_GUARD(s2n_check_kem(cipher_suite, conn, &kem_supported));

    *is_supported = ecdhe_supported && kem_supported;

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_kex_configure_noop(const struct s2n_cipher_suite *cipher_suite,
        struct s2n_connection *conn)
{
    return S2N_RESULT_OK;
}

static int s2n_kex_server_key_recv_read_data_unimplemented(struct s2n_connection *conn,
        struct s2n_blob *data_to_verify, struct s2n_kex_raw_server_data *kex_data)
{
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
}

static int s2n_kex_server_key_recv_parse_data_unimplemented(struct s2n_connection *conn,
        struct s2n_kex_raw_server_data *kex_data)
{
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
}

static int s2n_kex_io_unimplemented(struct s2n_connection *conn, struct s2n_blob *data_to_sign)
{
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
}

static int s2n_kex_prf_unimplemented(struct s2n_connection *conn, struct s2n_blob *premaster_secret)
{
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
}

const struct s2n_kex s2n_kem = {
    .is_ephemeral = true,
    .connection_supported = &s2n_check_kem,
    .configure_connection = &s2n_configure_kem,
    .server_key_recv_read_data = &s2n_kem_server_key_recv_read_data,
    .server_key_recv_parse_data = &s2n_kem_server_key_recv_parse_data,
    .server_key_send = &s2n_kem_server_key_send,
    .client_key_recv = &s2n_kem_client_key_recv,
    .client_key_send = &s2n_kem_client_key_send,
    .prf = &s2n_kex_prf_unimplemented,
};

const struct s2n_kex s2n_rsa = {
    .is_ephemeral = false,
    .connection_supported = &s2n_check_rsa_key,
    .configure_connection = &s2n_kex_configure_noop,
    .server_key_recv_read_data = &s2n_kex_server_key_recv_read_data_unimplemented,
    .server_key_recv_parse_data = &s2n_kex_server_key_recv_parse_data_unimplemented,
    .server_key_send = &s2n_kex_io_unimplemented,
    .client_key_recv = &s2n_rsa_client_key_recv,
    .client_key_send = &s2n_rsa_client_key_send,
    .prf = &s2n_prf_calculate_master_secret,
};

const struct s2n_kex s2n_dhe = {
    .is_ephemeral = true,
    .connection_supported = &s2n_check_dhe,
    .configure_connection = &s2n_kex_configure_noop,
    .server_key_recv_read_data = &s2n_dhe_server_key_recv_read_data,
    .server_key_recv_parse_data = &s2n_dhe_server_key_recv_parse_data,
    .server_key_send = &s2n_dhe_server_key_send,
    .client_key_recv = &s2n_dhe_client_key_recv,
    .client_key_send = &s2n_dhe_client_key_send,
    .prf = &s2n_prf_calculate_master_secret,
};

const struct s2n_kex s2n_ecdhe = {
    .is_ephemeral = true,
    .connection_supported = &s2n_check_ecdhe,
    .configure_connection = &s2n_kex_configure_noop,
    .server_key_recv_read_data = &s2n_ecdhe_server_key_recv_read_data,
    .server_key_recv_parse_data = &s2n_ecdhe_server_key_recv_parse_data,
    .server_key_send = &s2n_ecdhe_server_key_send,
    .client_key_recv = &s2n_ecdhe_client_key_recv,
    .client_key_send = &s2n_ecdhe_client_key_send,
    .prf = &s2n_prf_calculate_master_secret,
};

const struct s2n_kex s2n_hybrid_ecdhe_kem = {
    .is_ephemeral = true,
    .hybrid = { &s2n_ecdhe, &s2n_kem },
    .connection_supported = &s2n_check_hybrid_ecdhe_kem,
    .configure_connection = &s2n_configure_kem,
    .server_key_recv_read_data = &s2n_hybrid_server_key_recv_read_data,
    .server_key_recv_parse_data = &s2n_hybrid_server_key_recv_parse_data,
    .server_key_send = &s2n_hybrid_server_key_send,
    .client_key_recv = &s2n_hybrid_client_key_recv,
    .client_key_send = &s2n_hybrid_client_key_send,
    .prf = &s2n_hybrid_prf_master_secret,
};

/* TLS1.3 key exchange is implemented differently from previous versions and does
 * not currently require most of the functionality offered by s2n_kex.
 * This structure primarily acts as a placeholder, so its methods are either
 * noops or unimplemented.
 */
const struct s2n_kex s2n_tls13_kex = {
    .is_ephemeral = true,
    .connection_supported = &s2n_check_tls13,
    .configure_connection = &s2n_kex_configure_noop,
    .server_key_recv_read_data = &s2n_kex_server_key_recv_read_data_unimplemented,
    .server_key_recv_parse_data = &s2n_kex_server_key_recv_parse_data_unimplemented,
    .server_key_send = &s2n_kex_io_unimplemented,
    .client_key_recv = &s2n_kex_io_unimplemented,
    .client_key_send = &s2n_kex_io_unimplemented,
    .prf = &s2n_kex_prf_unimplemented,
};

S2N_RESULT s2n_kex_supported(const struct s2n_cipher_suite *cipher_suite, struct s2n_connection *conn, bool *is_supported)
{
    RESULT_ENSURE_REF(cipher_suite);
    RESULT_ENSURE_REF(cipher_suite->key_exchange_alg);
    RESULT_ENSURE_REF(cipher_suite->key_exchange_alg->connection_supported);
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(is_supported);

    RESULT_GUARD(cipher_suite->key_exchange_alg->connection_supported(cipher_suite, conn, is_supported));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_configure_kex(const struct s2n_cipher_suite *cipher_suite, struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(cipher_suite);
    RESULT_ENSURE_REF(cipher_suite->key_exchange_alg);
    RESULT_ENSURE_REF(cipher_suite->key_exchange_alg->configure_connection);
    RESULT_ENSURE_REF(conn);

    RESULT_GUARD(cipher_suite->key_exchange_alg->configure_connection(cipher_suite, conn));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_kex_is_ephemeral(const struct s2n_kex *kex, bool *is_ephemeral)
{
    RESULT_ENSURE_REF(kex);
    RESULT_ENSURE_REF(is_ephemeral);

    *is_ephemeral = kex->is_ephemeral;

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_kex_server_key_recv_parse_data(const struct s2n_kex *kex, struct s2n_connection *conn, struct s2n_kex_raw_server_data *raw_server_data)
{
    RESULT_ENSURE_REF(kex);
    RESULT_ENSURE_REF(kex->server_key_recv_parse_data);
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(raw_server_data);

    RESULT_GUARD_POSIX(kex->server_key_recv_parse_data(conn, raw_server_data));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_kex_server_key_recv_read_data(const struct s2n_kex *kex, struct s2n_connection *conn, struct s2n_blob *data_to_verify,
        struct s2n_kex_raw_server_data *raw_server_data)
{
    RESULT_ENSURE_REF(kex);
    RESULT_ENSURE_REF(kex->server_key_recv_read_data);
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(data_to_verify);

    RESULT_GUARD_POSIX(kex->server_key_recv_read_data(conn, data_to_verify, raw_server_data));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_kex_server_key_send(const struct s2n_kex *kex, struct s2n_connection *conn, struct s2n_blob *data_to_sign)
{
    RESULT_ENSURE_REF(kex);
    RESULT_ENSURE_REF(kex->server_key_send);
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(data_to_sign);

    RESULT_GUARD_POSIX(kex->server_key_send(conn, data_to_sign));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_kex_client_key_recv(const struct s2n_kex *kex, struct s2n_connection *conn, struct s2n_blob *shared_key)
{
    RESULT_ENSURE_REF(kex);
    RESULT_ENSURE_REF(kex->client_key_recv);
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(shared_key);

    RESULT_GUARD_POSIX(kex->client_key_recv(conn, shared_key));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_kex_client_key_send(const struct s2n_kex *kex, struct s2n_connection *conn, struct s2n_blob *shared_key)
{
    RESULT_ENSURE_REF(kex);
    RESULT_ENSURE_REF(kex->client_key_send);
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(shared_key);

    RESULT_GUARD_POSIX(kex->client_key_send(conn, shared_key));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_kex_tls_prf(const struct s2n_kex *kex, struct s2n_connection *conn, struct s2n_blob *premaster_secret)
{
    RESULT_ENSURE_REF(kex);
    RESULT_ENSURE_REF(kex->prf);
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(premaster_secret);

    RESULT_GUARD_POSIX(kex->prf(conn, premaster_secret));

    return S2N_RESULT_OK;
}

bool s2n_kex_includes(const struct s2n_kex *kex, const struct s2n_kex *query)
{
    if (kex == query) {
        return true;
    }

    if (kex == NULL || query == NULL) {
        return false;
    }

    return query == kex->hybrid[0] || query == kex->hybrid[1];
}
