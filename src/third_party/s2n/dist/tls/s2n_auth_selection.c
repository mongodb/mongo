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

#include "tls/s2n_auth_selection.h"

#include "crypto/s2n_certificate.h"
#include "crypto/s2n_signature.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_kex.h"
#include "tls/s2n_signature_algorithms.h"
#include "utils/s2n_safety.h"

/* This module should contain any logic related to choosing a valid combination of
 * signature algorithm, authentication method, and certificate to use for authentication.
 *
 * We choose our auth methods by:
 * 1. Finding a cipher suite with an auth method that we have valid certs for. In TLS1.3,
 *    this is a no-op -- cipher suites do not specify an auth method.
 * 2. Choosing a signature algorithm that matches both the auth method (if set) and the
 *    available certs.
 * 3. Selecting the cert that matches the chosen signature algorithm.
 *
 * This is a break from the original s2n pre-TLS1.3 flow, when we could choose certs and
 * ciphers at the same time. Our cipher suites differentiate between "RSA" and "ECDSA",
 * but not between "RSA" and "RSA-PSS". To make that decision, we need to wait until
 * we've chosen a signature algorithm. This allows us to use RSA-PSS with existing
 * TLS1.2 cipher suites.
 */

int s2n_get_auth_method_for_cert_type(s2n_pkey_type cert_type, s2n_authentication_method *auth_method)
{
    switch (cert_type) {
        case S2N_PKEY_TYPE_RSA:
        case S2N_PKEY_TYPE_RSA_PSS:
            *auth_method = S2N_AUTHENTICATION_RSA;
            return S2N_SUCCESS;
        case S2N_PKEY_TYPE_ECDSA:
            *auth_method = S2N_AUTHENTICATION_ECDSA;
            return S2N_SUCCESS;
        case S2N_PKEY_TYPE_MLDSA:
        case S2N_PKEY_TYPE_UNKNOWN:
        case S2N_PKEY_TYPE_SENTINEL:
            POSIX_BAIL(S2N_ERR_CERT_TYPE_UNSUPPORTED);
    }
    POSIX_BAIL(S2N_ERR_CERT_TYPE_UNSUPPORTED);
}

static int s2n_is_sig_alg_valid_for_cipher_suite(s2n_signature_algorithm sig_alg, struct s2n_cipher_suite *cipher_suite)
{
    POSIX_ENSURE_REF(cipher_suite);

    s2n_pkey_type cert_type_for_sig_alg = S2N_PKEY_TYPE_UNKNOWN;
    POSIX_GUARD_RESULT(s2n_signature_algorithm_get_pkey_type(sig_alg, &cert_type_for_sig_alg));

    /* Non-ephemeral key exchange methods require encryption, and RSA-PSS certificates
     * do not support encryption.
     *
     * Therefore, if a cipher suite uses a non-ephemeral kex, then any signature
     * algorithm that requires RSA-PSS certificates is not valid.
     */
    const struct s2n_kex *kex = cipher_suite->key_exchange_alg;
    POSIX_ENSURE_REF(kex);
    if (!kex->is_ephemeral) {
        POSIX_ENSURE_NE(cert_type_for_sig_alg, S2N_PKEY_TYPE_RSA_PSS);
    }

    /* If a cipher suite includes an auth method, then the signature algorithm
     * must match that auth method.
     */
    if (cipher_suite->auth_method != S2N_AUTHENTICATION_METHOD_SENTINEL) {
        s2n_authentication_method auth_method_for_sig_alg;
        POSIX_GUARD(s2n_get_auth_method_for_cert_type(cert_type_for_sig_alg, &auth_method_for_sig_alg));
        POSIX_ENSURE_EQ(cipher_suite->auth_method, auth_method_for_sig_alg);
    }

    return S2N_SUCCESS;
}

static int s2n_certs_exist_for_sig_scheme(struct s2n_connection *conn, const struct s2n_signature_scheme *sig_scheme)
{
    POSIX_ENSURE_REF(sig_scheme);

    s2n_pkey_type cert_type = S2N_PKEY_TYPE_UNKNOWN;
    POSIX_GUARD_RESULT(s2n_signature_algorithm_get_pkey_type(sig_scheme->sig_alg, &cert_type));
    /* A valid cert must exist for the authentication method. */
    struct s2n_cert_chain_and_key *cert = s2n_get_compatible_cert_chain_and_key(conn, cert_type);
    POSIX_ENSURE_REF(cert);

    /* In TLS 1.3, signature scheme may further restrict the certs */
    if (conn->actual_protocol_version >= S2N_TLS13) {
        if (cert_type == S2N_PKEY_TYPE_RSA || cert_type == S2N_PKEY_TYPE_RSA_PSS) {
            /* a RSA cert is valid for any corresponding RSA signature scheme. For
             * example an rsae cert can be used for both rsa_pss_rsae_sha256
             * and rsa_pss_rsae_sha384 */
            return S2N_SUCCESS;
        } else if (cert_type == S2N_PKEY_TYPE_ECDSA) {
            /* TLS 1.3 ECDSA signatures schemes e.g. ecdsa_secp384r1_sha384 also specify
             * a curve. We must make sure that the certificate has the correct curve */
            POSIX_ENSURE_REF(cert->private_key);
            POSIX_ENSURE_REF(cert->cert_chain);
            POSIX_ENSURE_REF(cert->cert_chain->head);
            POSIX_ENSURE_EQ(cert->cert_chain->head->pkey_type, S2N_PKEY_TYPE_ECDSA);
            POSIX_ENSURE_EQ(cert->cert_chain->head->info.public_key_nid, sig_scheme->signature_curve->libcrypto_nid);
        } else if (cert_type == S2N_PKEY_TYPE_MLDSA) {
            /* ML-DSA signatures e.g. mldsa65 include a parameter set (65) which must
             * match the cert */
            POSIX_ENSURE_REF(cert->private_key);
            POSIX_ENSURE_REF(cert->cert_chain);
            POSIX_ENSURE_REF(cert->cert_chain->head);
            POSIX_ENSURE_EQ(cert->cert_chain->head->pkey_type, S2N_PKEY_TYPE_MLDSA);
            POSIX_ENSURE_EQ(cert->cert_chain->head->info.signature_nid, sig_scheme->libcrypto_nid);
        } else {
            /* We expect any future signature schemes to also have these restrictions
             * so we concretely fail here until they are properly handled */
            POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
        }
    }

    return S2N_SUCCESS;
}

static int s2n_certs_exist_for_auth_method(struct s2n_connection *conn, s2n_authentication_method auth_method)
{
    if (auth_method == S2N_AUTHENTICATION_METHOD_SENTINEL) {
        return S2N_SUCCESS;
    }

    s2n_authentication_method auth_method_for_cert_type;
    for (int i = 0; i < S2N_CERT_TYPE_COUNT; i++) {
        POSIX_GUARD(s2n_get_auth_method_for_cert_type(i, &auth_method_for_cert_type));

        if (auth_method != auth_method_for_cert_type) {
            continue;
        }

        if (s2n_get_compatible_cert_chain_and_key(conn, i) != NULL) {
            return S2N_SUCCESS;
        }
    }
    POSIX_BAIL(S2N_ERR_CERT_TYPE_UNSUPPORTED);
}

/* TLS1.3 ciphers are always valid, as they don't include an auth method.
 *
 * A pre-TLS1.3 cipher suite is valid if:
 * - At least one compatible cert is configured
 *
 * This method is called by the server when choosing a cipher suite.
 */
int s2n_is_cipher_suite_valid_for_auth(struct s2n_connection *conn, struct s2n_cipher_suite *cipher_suite)
{
    POSIX_ENSURE_REF(cipher_suite);

    POSIX_GUARD(s2n_certs_exist_for_auth_method(conn, cipher_suite->auth_method));

    return S2N_SUCCESS;
}

/* A signature algorithm is valid if:
 * - At least one compatible cert is configured.
 * - The signature algorithm is allowed by the cipher suite's auth method
 *   (if running as a pre-TLS1.3 server).
 *
 * This method is called by the both server and client when choosing a signature algorithm.
 */
int s2n_is_sig_scheme_valid_for_auth(struct s2n_connection *conn, const struct s2n_signature_scheme *sig_scheme)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);
    POSIX_ENSURE_REF(sig_scheme);

    struct s2n_cipher_suite *cipher_suite = conn->secure->cipher_suite;
    POSIX_ENSURE_REF(cipher_suite);

    POSIX_GUARD(s2n_certs_exist_for_sig_scheme(conn, sig_scheme));

    /* For the client side, signature algorithm does not need to match the cipher suite. */
    if (conn->mode == S2N_SERVER) {
        POSIX_GUARD(s2n_is_sig_alg_valid_for_cipher_suite(sig_scheme->sig_alg, cipher_suite));
    }
    return S2N_SUCCESS;
}

/* A cert is valid if:
 * - The configured cipher suite's auth method (if present) supports the cert.
 *
 * We could also verify that at least one of our supported sig algs
 * supports the cert, but that seems unnecessary. If we don't have a valid
 * sig alg, we'll fail on CertVerify.
 *
 * This method is called by the client when receiving the server's cert.
 */
int s2n_is_cert_type_valid_for_auth(struct s2n_connection *conn, s2n_pkey_type cert_type)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);
    POSIX_ENSURE_REF(conn->secure->cipher_suite);
    s2n_authentication_method conn_auth_method = conn->secure->cipher_suite->auth_method;

    /* TLS1.3 cipher suites do not specify an auth type */
    if (conn_auth_method == S2N_AUTHENTICATION_METHOD_SENTINEL) {
        return S2N_SUCCESS;
    }

    s2n_authentication_method cert_auth_method = 0;
    POSIX_GUARD(s2n_get_auth_method_for_cert_type(cert_type, &cert_auth_method));
    POSIX_ENSURE(cert_auth_method == conn_auth_method, S2N_ERR_CERT_TYPE_UNSUPPORTED);
    return S2N_SUCCESS;
}

/* Choose the cert associated with our configured signature algorithm.
 *
 * This method is called by the server after configuring its cipher suite and sig algs.
 */
int s2n_select_certs_for_server_auth(struct s2n_connection *conn, struct s2n_cert_chain_and_key **chosen_certs)
{
    POSIX_ENSURE_REF(conn);
    const struct s2n_signature_scheme *sig_scheme = conn->handshake_params.server_cert_sig_scheme;
    POSIX_ENSURE_REF(sig_scheme);
    s2n_signature_algorithm sig_alg = sig_scheme->sig_alg;

    s2n_pkey_type cert_type = S2N_PKEY_TYPE_UNKNOWN;
    if (sig_scheme == &s2n_null_sig_scheme) {
        /* Only RSA auth (+ RSA kex) supports no signature scheme */
        cert_type = S2N_PKEY_TYPE_RSA;
    } else {
        POSIX_GUARD_RESULT(s2n_signature_algorithm_get_pkey_type(sig_alg, &cert_type));
    }

    *chosen_certs = s2n_get_compatible_cert_chain_and_key(conn, cert_type);
    S2N_ERROR_IF(*chosen_certs == NULL, S2N_ERR_CERT_TYPE_UNSUPPORTED);

    return S2N_SUCCESS;
}
