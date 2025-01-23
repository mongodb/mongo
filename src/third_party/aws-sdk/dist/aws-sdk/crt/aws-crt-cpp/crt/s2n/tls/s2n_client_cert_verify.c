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
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_async_pkey.h"
#include "tls/s2n_config.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_signature_algorithms.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_safety.h"

static int s2n_client_cert_verify_send_complete(struct s2n_connection *conn, struct s2n_blob *signature);

int s2n_client_cert_verify_recv(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    struct s2n_handshake_hashes *hashes = conn->handshake.hashes;
    POSIX_ENSURE_REF(hashes);

    struct s2n_stuffer *in = &conn->handshake.io;

    POSIX_GUARD_RESULT(s2n_signature_algorithm_recv(conn, in));
    const struct s2n_signature_scheme *chosen_sig_scheme = conn->handshake_params.client_cert_sig_scheme;
    POSIX_ENSURE_REF(chosen_sig_scheme);

    uint16_t signature_size = 0;
    struct s2n_blob signature = { 0 };
    POSIX_GUARD(s2n_stuffer_read_uint16(in, &signature_size));
    signature.size = signature_size;
    signature.data = s2n_stuffer_raw_read(in, signature.size);
    POSIX_ENSURE_REF(signature.data);

    /* Use a copy of the hash state since the verify digest computation may modify the running hash state we need later. */
    struct s2n_hash_state *hash_state = &hashes->hash_workspace;
    POSIX_GUARD_RESULT(s2n_handshake_copy_hash_state(conn, chosen_sig_scheme->hash_alg, hash_state));

    /* Verify the signature */
    POSIX_GUARD(s2n_pkey_verify(&conn->handshake_params.client_public_key, chosen_sig_scheme->sig_alg, hash_state, &signature));

    /* Client certificate has been verified. Minimize required handshake hash algs */
    POSIX_GUARD(s2n_conn_update_required_handshake_hashes(conn));

    return S2N_SUCCESS;
}

int s2n_client_cert_verify_send(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    struct s2n_handshake_hashes *hashes = conn->handshake.hashes;
    POSIX_ENSURE_REF(hashes);

    S2N_ASYNC_PKEY_GUARD(conn);
    struct s2n_stuffer *out = &conn->handshake.io;

    if (conn->actual_protocol_version >= S2N_TLS12) {
        POSIX_GUARD(s2n_stuffer_write_uint16(out, conn->handshake_params.client_cert_sig_scheme->iana_value));
    }
    const struct s2n_signature_scheme *chosen_sig_scheme = conn->handshake_params.client_cert_sig_scheme;
    POSIX_ENSURE_REF(chosen_sig_scheme);

    /* Use a copy of the hash state since the verify digest computation may modify the running hash state we need later. */
    struct s2n_hash_state *hash_state = &hashes->hash_workspace;
    POSIX_GUARD_RESULT(s2n_handshake_copy_hash_state(conn, chosen_sig_scheme->hash_alg, hash_state));

    S2N_ASYNC_PKEY_SIGN(conn, chosen_sig_scheme->sig_alg, hash_state, s2n_client_cert_verify_send_complete);
}

static int s2n_client_cert_verify_send_complete(struct s2n_connection *conn, struct s2n_blob *signature)
{
    struct s2n_stuffer *out = &conn->handshake.io;

    POSIX_GUARD(s2n_stuffer_write_uint16(out, signature->size));
    POSIX_GUARD(s2n_stuffer_write(out, signature));

    /* Client certificate has been verified. Minimize required handshake hash algs */
    POSIX_GUARD(s2n_conn_update_required_handshake_hashes(conn));

    return S2N_SUCCESS;
}
