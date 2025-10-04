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

#include "tls/s2n_tls13_certificate_verify.h"

#include <stdint.h>

#include "crypto/s2n_hash.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_async_pkey.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_tls13_handshake.h"
#include "utils/s2n_safety.h"

/**
  * Specified in https://tools.ietf.org/html/rfc8446#section-4.4.3
  *
  * Servers MUST send this message when authenticating via a certificate.  
  * Clients MUST send this message whenever authenticating via a certificate. 
  * When sent, this message MUST appear immediately after the Certificate 
  * message and immediately prior to the Finished message.
 **/

/* 64 'space' characters (0x20) */
const uint8_t S2N_CERT_VERIFY_PREFIX[] = { 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 };
/* 'TLS 1.3, server CertificateVerify' with 0x00 separator */
const uint8_t S2N_SERVER_CERT_VERIFY_CONTEXT[] = { 0x54, 0x4c, 0x53, 0x20, 0x31, 0x2e, 0x33,
    0x2c, 0x20, 0x73, 0x65, 0x72, 0x76, 0x65, 0x72, 0x20, 0x43, 0x65, 0x72, 0x74, 0x69,
    0x66, 0x69, 0x63, 0x61, 0x74, 0x65, 0x56, 0x65, 0x72, 0x69, 0x66, 0x79, 0x00 };
/* 'TLS 1.3, client CertificateVerify' with 0x00 separator */
const uint8_t S2N_CLIENT_CERT_VERIFY_CONTEXT[] = { 0x54, 0x4c, 0x53, 0x20, 0x31, 0x2e, 0x33,
    0x2c, 0x20, 0x63, 0x6c, 0x69, 0x65, 0x6e, 0x74, 0x20, 0x43, 0x65, 0x72, 0x74, 0x69,
    0x66, 0x69, 0x63, 0x61, 0x74, 0x65, 0x56, 0x65, 0x72, 0x69, 0x66, 0x79, 0x00 };

static int s2n_tls13_write_cert_verify_signature(struct s2n_connection *conn,
        const struct s2n_signature_scheme *chosen_sig_scheme);
static int s2n_tls13_write_signature(struct s2n_connection *conn, struct s2n_blob *signature);
static int s2n_tls13_generate_unsigned_cert_verify_content(struct s2n_connection *conn,
        struct s2n_stuffer *unsigned_content, s2n_mode mode);
static int s2n_tls13_cert_read_and_verify_signature(struct s2n_connection *conn,
        const struct s2n_signature_scheme *chosen_sig_scheme);
static uint8_t s2n_tls13_cert_verify_header_length(s2n_mode mode);

int s2n_tls13_cert_verify_send(struct s2n_connection *conn)
{
    S2N_ASYNC_PKEY_GUARD(conn);

    if (conn->mode == S2N_SERVER) {
        /* Write digital signature */
        POSIX_GUARD(s2n_tls13_write_cert_verify_signature(conn, conn->handshake_params.server_cert_sig_scheme));
    } else {
        /* Write digital signature */
        POSIX_GUARD(s2n_tls13_write_cert_verify_signature(conn, conn->handshake_params.client_cert_sig_scheme));
    }

    return 0;
}

int s2n_tls13_write_cert_verify_signature(struct s2n_connection *conn,
        const struct s2n_signature_scheme *chosen_sig_scheme)
{
    POSIX_ENSURE_REF(conn->handshake_params.our_chain_and_key);

    /* Write the SignatureScheme out */
    struct s2n_stuffer *out = &conn->handshake.io;
    POSIX_GUARD(s2n_stuffer_write_uint16(out, chosen_sig_scheme->iana_value));

    DEFER_CLEANUP(struct s2n_hash_state message_hash = { 0 }, s2n_hash_free);
    POSIX_GUARD(s2n_hash_new(&message_hash));
    POSIX_GUARD(s2n_hash_init(&message_hash, chosen_sig_scheme->hash_alg));

    DEFER_CLEANUP(struct s2n_stuffer unsigned_content = { 0 }, s2n_stuffer_free);
    POSIX_GUARD(s2n_tls13_generate_unsigned_cert_verify_content(conn, &unsigned_content, conn->mode));

    POSIX_GUARD(s2n_hash_update(&message_hash, unsigned_content.blob.data,
            s2n_stuffer_data_available(&unsigned_content)));

    S2N_ASYNC_PKEY_SIGN(conn, chosen_sig_scheme->sig_alg, &message_hash, s2n_tls13_write_signature);
}

int s2n_tls13_write_signature(struct s2n_connection *conn, struct s2n_blob *signature)
{
    struct s2n_stuffer *out = &conn->handshake.io;

    POSIX_GUARD(s2n_stuffer_write_uint16(out, signature->size));
    POSIX_GUARD(s2n_stuffer_write_bytes(out, signature->data, signature->size));

    return 0;
}

int s2n_tls13_generate_unsigned_cert_verify_content(struct s2n_connection *conn,
        struct s2n_stuffer *unsigned_content, s2n_mode mode)
{
    s2n_tls13_connection_keys(tls13_ctx, conn);

    uint8_t hash_digest_length = tls13_ctx.size;
    uint8_t digest_out[S2N_MAX_DIGEST_LEN];

    /* Get current handshake hash */
    POSIX_ENSURE_REF(conn->handshake.hashes);
    struct s2n_hash_state *hash_state = &conn->handshake.hashes->hash_workspace;
    POSIX_GUARD_RESULT(s2n_handshake_copy_hash_state(conn, tls13_ctx.hash_algorithm, hash_state));
    POSIX_GUARD(s2n_hash_digest(hash_state, digest_out, hash_digest_length));

    /* Concatenate the content to be signed/verified */
    POSIX_GUARD(s2n_stuffer_alloc(unsigned_content, hash_digest_length + s2n_tls13_cert_verify_header_length(mode)));
    POSIX_GUARD(s2n_stuffer_write_bytes(unsigned_content, S2N_CERT_VERIFY_PREFIX, sizeof(S2N_CERT_VERIFY_PREFIX)));

    if (mode == S2N_CLIENT) {
        POSIX_GUARD(s2n_stuffer_write_bytes(unsigned_content, S2N_CLIENT_CERT_VERIFY_CONTEXT,
                sizeof(S2N_CLIENT_CERT_VERIFY_CONTEXT)));
    } else {
        POSIX_GUARD(s2n_stuffer_write_bytes(unsigned_content, S2N_SERVER_CERT_VERIFY_CONTEXT,
                sizeof(S2N_SERVER_CERT_VERIFY_CONTEXT)));
    }

    POSIX_GUARD(s2n_stuffer_write_bytes(unsigned_content, digest_out, hash_digest_length));

    return 0;
}

uint8_t s2n_tls13_cert_verify_header_length(s2n_mode mode)
{
    if (mode == S2N_CLIENT) {
        return sizeof(S2N_CERT_VERIFY_PREFIX) + sizeof(S2N_CLIENT_CERT_VERIFY_CONTEXT);
    }
    return sizeof(S2N_CERT_VERIFY_PREFIX) + sizeof(S2N_SERVER_CERT_VERIFY_CONTEXT);
}

int s2n_tls13_cert_verify_recv(struct s2n_connection *conn)
{
    POSIX_GUARD_RESULT(s2n_signature_algorithm_recv(conn, &conn->handshake.io));
    /* Read the rest of the signature and verify */
    if (conn->mode == S2N_SERVER) {
        POSIX_GUARD(s2n_tls13_cert_read_and_verify_signature(conn,
                conn->handshake_params.client_cert_sig_scheme));
    } else {
        POSIX_GUARD(s2n_tls13_cert_read_and_verify_signature(conn,
                conn->handshake_params.server_cert_sig_scheme));
    }

    return 0;
}

int s2n_tls13_cert_read_and_verify_signature(struct s2n_connection *conn,
        const struct s2n_signature_scheme *chosen_sig_scheme)
{
    struct s2n_stuffer *in = &conn->handshake.io;
    DEFER_CLEANUP(struct s2n_blob signed_content = { 0 }, s2n_free);
    DEFER_CLEANUP(struct s2n_stuffer unsigned_content = { 0 }, s2n_stuffer_free);
    DEFER_CLEANUP(struct s2n_hash_state message_hash = { 0 }, s2n_hash_free);
    POSIX_GUARD(s2n_hash_new(&message_hash));

    /* Get signature size */
    uint16_t signature_size = 0;
    POSIX_GUARD(s2n_stuffer_read_uint16(in, &signature_size));
    S2N_ERROR_IF(signature_size > s2n_stuffer_data_available(in), S2N_ERR_BAD_MESSAGE);

    /* Get wire signature */
    POSIX_GUARD(s2n_alloc(&signed_content, signature_size));
    signed_content.size = signature_size;
    POSIX_GUARD(s2n_stuffer_read_bytes(in, signed_content.data, signature_size));

    /* Verify signature. We send the opposite mode as we are trying to verify what was sent to us */
    if (conn->mode == S2N_CLIENT) {
        POSIX_GUARD(s2n_tls13_generate_unsigned_cert_verify_content(conn, &unsigned_content, S2N_SERVER));
    } else {
        POSIX_GUARD(s2n_tls13_generate_unsigned_cert_verify_content(conn, &unsigned_content, S2N_CLIENT));
    }

    POSIX_GUARD(s2n_hash_init(&message_hash, chosen_sig_scheme->hash_alg));
    POSIX_GUARD(s2n_hash_update(&message_hash, unsigned_content.blob.data,
            s2n_stuffer_data_available(&unsigned_content)));

    if (conn->mode == S2N_CLIENT) {
        POSIX_GUARD(s2n_pkey_verify(&conn->handshake_params.server_public_key, chosen_sig_scheme->sig_alg,
                &message_hash, &signed_content));
    } else {
        POSIX_GUARD(s2n_pkey_verify(&conn->handshake_params.client_public_key, chosen_sig_scheme->sig_alg,
                &message_hash, &signed_content));
    }

    return 0;
}
