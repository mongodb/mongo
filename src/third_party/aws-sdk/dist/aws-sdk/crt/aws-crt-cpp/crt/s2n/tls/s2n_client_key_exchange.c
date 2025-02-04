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

#include "api/s2n.h"
#include "crypto/s2n_dhe.h"
#include "crypto/s2n_pkey.h"
#include "crypto/s2n_rsa.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_async_pkey.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_handshake.h"
#include "tls/s2n_kem.h"
#include "tls/s2n_kex.h"
#include "tls/s2n_key_log.h"
#include "tls/s2n_resume.h"
#include "utils/s2n_random.h"
#include "utils/s2n_safety.h"

#define get_client_hello_protocol_version(conn) (conn->client_hello_version == S2N_SSLv2 ? conn->client_protocol_version : conn->client_hello_version)

typedef S2N_RESULT s2n_kex_client_key_method(const struct s2n_kex *kex, struct s2n_connection *conn, struct s2n_blob *shared_key);
typedef void *s2n_stuffer_action(struct s2n_stuffer *stuffer, uint32_t data_len);

static int s2n_rsa_client_key_recv_complete(struct s2n_connection *conn, bool rsa_failed, struct s2n_blob *shared_key);

static int s2n_hybrid_client_action(struct s2n_connection *conn, struct s2n_blob *combined_shared_key,
        s2n_kex_client_key_method kex_method, uint32_t *cursor, s2n_stuffer_action stuffer_action)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);
    POSIX_ENSURE_REF(kex_method);
    POSIX_ENSURE_REF(stuffer_action);

    struct s2n_stuffer *io = &conn->handshake.io;
    const struct s2n_kex *hybrid_kex_0 = conn->secure->cipher_suite->key_exchange_alg->hybrid[0];
    const struct s2n_kex *hybrid_kex_1 = conn->secure->cipher_suite->key_exchange_alg->hybrid[1];

    /* Keep a copy to the start of the entire hybrid client key exchange message for the hybrid PRF */
    struct s2n_blob *client_key_exchange_message = &conn->kex_params.client_key_exchange_message;
    client_key_exchange_message->data = stuffer_action(io, 0);
    POSIX_ENSURE_REF(client_key_exchange_message->data);
    const uint32_t start_cursor = *cursor;

    DEFER_CLEANUP(struct s2n_blob shared_key_0 = { 0 }, s2n_free);
    POSIX_GUARD_RESULT(kex_method(hybrid_kex_0, conn, &shared_key_0));

    struct s2n_blob *shared_key_1 = &(conn->kex_params.kem_params.shared_secret);
    POSIX_GUARD_RESULT(kex_method(hybrid_kex_1, conn, shared_key_1));

    const uint32_t end_cursor = *cursor;
    POSIX_ENSURE_GTE(end_cursor, start_cursor);
    client_key_exchange_message->size = end_cursor - start_cursor;

    POSIX_GUARD(s2n_alloc(combined_shared_key, shared_key_0.size + shared_key_1->size));
    struct s2n_stuffer stuffer_combiner = { 0 };
    POSIX_GUARD(s2n_stuffer_init(&stuffer_combiner, combined_shared_key));
    POSIX_GUARD(s2n_stuffer_write(&stuffer_combiner, &shared_key_0));
    POSIX_GUARD(s2n_stuffer_write(&stuffer_combiner, shared_key_1));

    POSIX_GUARD(s2n_kem_free(&conn->kex_params.kem_params));

    return 0;
}

static int s2n_calculate_keys(struct s2n_connection *conn, struct s2n_blob *shared_key)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);
    POSIX_ENSURE_REF(conn->secure->cipher_suite);

    /* Turn the pre-master secret into a master secret */
    POSIX_GUARD_RESULT(s2n_kex_tls_prf(conn->secure->cipher_suite->key_exchange_alg, conn, shared_key));

    /* Expand the keys */
    POSIX_GUARD(s2n_prf_key_expansion(conn));
    /* Save the master secret in the cache.
     * Failing to cache the session should not affect the current handshake.
     */
    if (s2n_allowed_to_cache_connection(conn)) {
        s2n_result_ignore(s2n_store_to_cache(conn));
    }
    /* log the secret, if needed */
    s2n_result_ignore(s2n_key_log_tls12_secret(conn));
    return 0;
}

int s2n_rsa_client_key_recv(struct s2n_connection *conn, struct s2n_blob *shared_key)
{
    /* Set shared_key before async guard to pass the proper shared_key to the caller upon async completion */
    POSIX_ENSURE_REF(shared_key);
    shared_key->data = conn->secrets.version.tls12.rsa_premaster_secret;
    shared_key->size = S2N_TLS_SECRET_LEN;

    S2N_ASYNC_PKEY_GUARD(conn);

    struct s2n_stuffer *in = &conn->handshake.io;
    uint8_t client_hello_protocol_version[S2N_TLS_PROTOCOL_VERSION_LEN];
    uint16_t length = 0;

    if (conn->actual_protocol_version == S2N_SSLv3) {
        length = s2n_stuffer_data_available(in);
    } else {
        POSIX_GUARD(s2n_stuffer_read_uint16(in, &length));
    }

    S2N_ERROR_IF(length > s2n_stuffer_data_available(in), S2N_ERR_BAD_MESSAGE);

    /* Keep a copy of the client hello version in wire format, which should be
     * either the protocol version supported by client if the supported version is <= TLS1.2,
     * or TLS1.2 (the legacy version) if client supported version is TLS1.3
     */
    uint8_t legacy_client_hello_protocol_version = get_client_hello_protocol_version(conn);
    client_hello_protocol_version[0] = legacy_client_hello_protocol_version / 10;
    client_hello_protocol_version[1] = legacy_client_hello_protocol_version % 10;

    /* Decrypt the pre-master secret */
    struct s2n_blob encrypted = { 0 };
    POSIX_GUARD(s2n_blob_init(&encrypted, s2n_stuffer_raw_read(in, length), length));
    POSIX_ENSURE_REF(encrypted.data);
    POSIX_ENSURE_GT(encrypted.size, 0);

    /* First: use a random pre-master secret */
    POSIX_GUARD_RESULT(s2n_get_private_random_data(shared_key));
    conn->secrets.version.tls12.rsa_premaster_secret[0] = client_hello_protocol_version[0];
    conn->secrets.version.tls12.rsa_premaster_secret[1] = client_hello_protocol_version[1];

    S2N_ASYNC_PKEY_DECRYPT(conn, &encrypted, shared_key, s2n_rsa_client_key_recv_complete);
}

int s2n_rsa_client_key_recv_complete(struct s2n_connection *conn, bool rsa_failed, struct s2n_blob *decrypted)
{
    S2N_ERROR_IF(decrypted->size != S2N_TLS_SECRET_LEN, S2N_ERR_SIZE_MISMATCH);

    /* Avoid copying the same buffer for the case where async pkey is not used */
    if (conn->secrets.version.tls12.rsa_premaster_secret != decrypted->data) {
        /* Copy (maybe) decrypted data into shared key */
        POSIX_CHECKED_MEMCPY(conn->secrets.version.tls12.rsa_premaster_secret, decrypted->data, S2N_TLS_SECRET_LEN);
    }

    /* Get client hello protocol version for comparison with decrypted data */
    uint8_t legacy_client_hello_protocol_version = get_client_hello_protocol_version(conn);
    uint8_t client_hello_protocol_version[S2N_TLS_PROTOCOL_VERSION_LEN];
    client_hello_protocol_version[0] = legacy_client_hello_protocol_version / 10;
    client_hello_protocol_version[1] = legacy_client_hello_protocol_version % 10;

    conn->handshake.rsa_failed = rsa_failed;

    /* Set rsa_failed to true, if it isn't already, if the protocol version isn't what we expect */
    conn->handshake.rsa_failed |= !s2n_constant_time_equals(client_hello_protocol_version,
            conn->secrets.version.tls12.rsa_premaster_secret, S2N_TLS_PROTOCOL_VERSION_LEN);

    /* Required to protect against Bleichenbacher attack.
     * See https://www.rfc-editor.org/rfc/rfc5246#section-7.4.7.1
     * We choose the first option: always setting the version in the rsa_premaster_secret
     * from our local view of the client_hello value.
     */
    conn->secrets.version.tls12.rsa_premaster_secret[0] = client_hello_protocol_version[0];
    conn->secrets.version.tls12.rsa_premaster_secret[1] = client_hello_protocol_version[1];

    return 0;
}

int s2n_dhe_client_key_recv(struct s2n_connection *conn, struct s2n_blob *shared_key)
{
    struct s2n_stuffer *in = &conn->handshake.io;

    /* Get the shared key */
    POSIX_GUARD(s2n_dh_compute_shared_secret_as_server(&conn->kex_params.server_dh_params, in, shared_key));
    /* We don't need the server params any more */
    POSIX_GUARD(s2n_dh_params_free(&conn->kex_params.server_dh_params));
    return 0;
}

int s2n_ecdhe_client_key_recv(struct s2n_connection *conn, struct s2n_blob *shared_key)
{
    struct s2n_stuffer *in = &conn->handshake.io;

    /* Get the shared key */
    POSIX_GUARD(s2n_ecc_evp_compute_shared_secret_as_server(&conn->kex_params.server_ecc_evp_params, in, shared_key));
    /* We don't need the server params any more */
    POSIX_GUARD(s2n_ecc_evp_params_free(&conn->kex_params.server_ecc_evp_params));
    return 0;
}

int s2n_kem_client_key_recv(struct s2n_connection *conn, struct s2n_blob *shared_key)
{
    /* s2n_kem_recv_ciphertext() writes the KEM shared secret directly to
     * conn->kex_params.kem_params. However, the calling function
     * likely expects *shared_key to point to the shared secret. We 
     * can't reassign *shared_key to point to kem_params.shared_secret,
     * because that would require us to take struct s2n_blob **shared_key
     * as the argument, but we can't (easily) change the function signature
     * because it has to be consistent with what is defined in s2n_kex.
     *
     * So, we assert that the caller already has *shared_key pointing
     * to kem_params.shared_secret. */
    POSIX_ENSURE_REF(shared_key);
    S2N_ERROR_IF(shared_key != &(conn->kex_params.kem_params.shared_secret), S2N_ERR_SAFETY);
    conn->kex_params.kem_params.len_prefixed = true; /* PQ TLS 1.2 is always length prefixed. */

    POSIX_GUARD(s2n_kem_recv_ciphertext(&(conn->handshake.io), &(conn->kex_params.kem_params)));

    return 0;
}

int s2n_hybrid_client_key_recv(struct s2n_connection *conn, struct s2n_blob *combined_shared_key)
{
    return s2n_hybrid_client_action(conn, combined_shared_key, &s2n_kex_client_key_recv, &conn->handshake.io.read_cursor,
            &s2n_stuffer_raw_read);
}

int s2n_client_key_recv(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);
    POSIX_ENSURE_REF(conn->secure->cipher_suite);

    const struct s2n_kex *key_exchange = conn->secure->cipher_suite->key_exchange_alg;
    DEFER_CLEANUP(struct s2n_blob shared_key = { 0 }, s2n_free_or_wipe);
    POSIX_GUARD_RESULT(s2n_kex_client_key_recv(key_exchange, conn, &shared_key));

    POSIX_GUARD(s2n_calculate_keys(conn, &shared_key));
    return 0;
}

int s2n_dhe_client_key_send(struct s2n_connection *conn, struct s2n_blob *shared_key)
{
    struct s2n_stuffer *out = &conn->handshake.io;
    POSIX_GUARD(s2n_dh_compute_shared_secret_as_client(&conn->kex_params.server_dh_params, out, shared_key));

    /* We don't need the server params any more */
    POSIX_GUARD(s2n_dh_params_free(&conn->kex_params.server_dh_params));
    return 0;
}

int s2n_ecdhe_client_key_send(struct s2n_connection *conn, struct s2n_blob *shared_key)
{
    struct s2n_stuffer *out = &conn->handshake.io;
    POSIX_GUARD(s2n_ecc_evp_compute_shared_secret_as_client(&conn->kex_params.server_ecc_evp_params, out, shared_key));

    /* We don't need the server params any more */
    POSIX_GUARD(s2n_ecc_evp_params_free(&conn->kex_params.server_ecc_evp_params));
    return 0;
}

int s2n_rsa_client_key_send(struct s2n_connection *conn, struct s2n_blob *shared_key)
{
    uint8_t client_hello_protocol_version[S2N_TLS_PROTOCOL_VERSION_LEN];
    uint8_t legacy_client_hello_protocol_version = get_client_hello_protocol_version(conn);
    client_hello_protocol_version[0] = legacy_client_hello_protocol_version / 10;
    client_hello_protocol_version[1] = legacy_client_hello_protocol_version % 10;

    shared_key->data = conn->secrets.version.tls12.rsa_premaster_secret;
    shared_key->size = S2N_TLS_SECRET_LEN;

    POSIX_GUARD_RESULT(s2n_get_private_random_data(shared_key));

    /* Over-write the first two bytes with the client hello version, per RFC2246/RFC4346/RFC5246 7.4.7.1.
     * The latest version supported by client (as seen from the the client hello version) are <= TLS1.2
     * for all clients, because TLS 1.3 clients freezes the TLS1.2 legacy version in client hello.
     */
    POSIX_CHECKED_MEMCPY(conn->secrets.version.tls12.rsa_premaster_secret, client_hello_protocol_version, S2N_TLS_PROTOCOL_VERSION_LEN);

    uint32_t encrypted_size = 0;
    POSIX_GUARD_RESULT(s2n_pkey_size(&conn->handshake_params.server_public_key, &encrypted_size));
    S2N_ERROR_IF(encrypted_size > 0xffff, S2N_ERR_SIZE_MISMATCH);

    if (conn->actual_protocol_version > S2N_SSLv3) {
        POSIX_GUARD(s2n_stuffer_write_uint16(&conn->handshake.io, encrypted_size));
    }

    struct s2n_blob encrypted = { 0 };
    encrypted.data = s2n_stuffer_raw_write(&conn->handshake.io, encrypted_size);
    encrypted.size = encrypted_size;
    POSIX_ENSURE_REF(encrypted.data);

    /* Encrypt the secret and send it on */
    POSIX_GUARD(s2n_pkey_encrypt(&conn->handshake_params.server_public_key, shared_key, &encrypted));

    /* We don't need the key any more, so free it */
    POSIX_GUARD(s2n_pkey_free(&conn->handshake_params.server_public_key));
    return 0;
}

int s2n_kem_client_key_send(struct s2n_connection *conn, struct s2n_blob *shared_key)
{
    /* s2n_kem_send_ciphertext() writes the KEM shared secret directly to
     * conn->kex_params.kem_params. However, the calling function
     * likely expects *shared_key to point to the shared secret. We
     * can't reassign *shared_key to point to kem_params.shared_secret,
     * because that would require us to take struct s2n_blob **shared_key
     * as the argument, but we can't (easily) change the function signature
     * because it has to be consistent with what is defined in s2n_kex.
     *
     * So, we assert that the caller already has *shared_key pointing
     * to kem_params.shared_secret. */
    POSIX_ENSURE_REF(shared_key);
    S2N_ERROR_IF(shared_key != &(conn->kex_params.kem_params.shared_secret), S2N_ERR_SAFETY);

    conn->kex_params.kem_params.len_prefixed = true; /* PQ TLS 1.2 is always length prefixed */

    POSIX_GUARD(s2n_kem_send_ciphertext(&(conn->handshake.io), &(conn->kex_params.kem_params)));

    return 0;
}

int s2n_hybrid_client_key_send(struct s2n_connection *conn, struct s2n_blob *combined_shared_key)
{
    return s2n_hybrid_client_action(conn, combined_shared_key, &s2n_kex_client_key_send, &conn->handshake.io.write_cursor,
            s2n_stuffer_raw_write);
}

int s2n_client_key_send(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);
    POSIX_ENSURE_REF(conn->secure->cipher_suite);

    const struct s2n_kex *key_exchange = conn->secure->cipher_suite->key_exchange_alg;
    DEFER_CLEANUP(struct s2n_blob shared_key = { 0 }, s2n_free_or_wipe);

    POSIX_GUARD_RESULT(s2n_kex_client_key_send(key_exchange, conn, &shared_key));

    POSIX_GUARD(s2n_calculate_keys(conn, &shared_key));
    return 0;
}
