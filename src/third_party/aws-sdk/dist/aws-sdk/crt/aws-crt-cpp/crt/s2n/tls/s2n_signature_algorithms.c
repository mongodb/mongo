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

#include "tls/s2n_signature_algorithms.h"

#include "crypto/s2n_fips.h"
#include "crypto/s2n_rsa_pss.h"
#include "crypto/s2n_rsa_signing.h"
#include "error/s2n_errno.h"
#include "tls/s2n_auth_selection.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_kex.h"
#include "tls/s2n_security_policies.h"
#include "tls/s2n_signature_scheme.h"
#include "utils/s2n_safety.h"

static S2N_RESULT s2n_signature_scheme_validate_for_send(struct s2n_connection *conn,
        const struct s2n_signature_scheme *scheme)
{
    RESULT_ENSURE_REF(conn);

    /* If no protocol has been negotiated yet, the actual_protocol_version will
     * be equivalent to the client_protocol_version and represent the highest
     * version supported.
     */
    RESULT_ENSURE_GTE(conn->actual_protocol_version, scheme->minimum_protocol_version);

    /* QUIC only supports TLS1.3 */
    if (s2n_connection_is_quic_enabled(conn) && scheme->maximum_protocol_version) {
        RESULT_ENSURE_GTE(scheme->maximum_protocol_version, S2N_TLS13);
    }

    if (!s2n_is_rsa_pss_signing_supported()) {
        RESULT_ENSURE_NE(scheme->sig_alg, S2N_SIGNATURE_RSA_PSS_RSAE);
    }

    if (!s2n_is_rsa_pss_certs_supported()) {
        RESULT_ENSURE_NE(scheme->sig_alg, S2N_SIGNATURE_RSA_PSS_PSS);
    }

    return S2N_RESULT_OK;
}

static bool s2n_signature_scheme_is_valid_for_send(struct s2n_connection *conn,
        const struct s2n_signature_scheme *scheme)
{
    return s2n_result_is_ok(s2n_signature_scheme_validate_for_send(conn, scheme));
}

static S2N_RESULT s2n_signature_scheme_validate_for_recv(struct s2n_connection *conn,
        const struct s2n_signature_scheme *scheme)
{
    RESULT_ENSURE_REF(scheme);
    RESULT_ENSURE_REF(conn);

    RESULT_GUARD(s2n_signature_scheme_validate_for_send(conn, scheme));

    if (scheme->maximum_protocol_version != S2N_UNKNOWN_PROTOCOL_VERSION) {
        RESULT_ENSURE_LTE(conn->actual_protocol_version, scheme->maximum_protocol_version);
    }

    RESULT_ENSURE_NE(conn->actual_protocol_version, S2N_UNKNOWN_PROTOCOL_VERSION);
    if (conn->actual_protocol_version >= S2N_TLS13) {
        RESULT_ENSURE_NE(scheme->hash_alg, S2N_HASH_SHA1);
        RESULT_ENSURE_NE(scheme->sig_alg, S2N_SIGNATURE_RSA);
    } else {
        RESULT_ENSURE_NE(scheme->sig_alg, S2N_SIGNATURE_RSA_PSS_PSS);
    }

    return S2N_RESULT_OK;
}

static bool s2n_signature_scheme_is_valid_for_recv(struct s2n_connection *conn,
        const struct s2n_signature_scheme *scheme)
{
    return s2n_result_is_ok(s2n_signature_scheme_validate_for_recv(conn, scheme));
}

static S2N_RESULT s2n_signature_algorithms_get_legacy_default(struct s2n_connection *conn,
        s2n_mode signer, const struct s2n_signature_scheme **default_sig_scheme)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(default_sig_scheme);

    s2n_authentication_method auth_method = 0;
    if (signer == S2N_CLIENT) {
        RESULT_GUARD_POSIX(s2n_get_auth_method_for_cert_type(
                conn->handshake_params.client_cert_pkey_type, &auth_method));
    } else {
        RESULT_ENSURE_REF(conn->secure);
        RESULT_ENSURE_REF(conn->secure->cipher_suite);
        auth_method = conn->secure->cipher_suite->auth_method;
    }

    if (auth_method == S2N_AUTHENTICATION_ECDSA) {
        *default_sig_scheme = &s2n_ecdsa_sha1;
    } else {
        *default_sig_scheme = &s2n_rsa_pkcs1_md5_sha1;
    }
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_signature_algorithm_recv(struct s2n_connection *conn, struct s2n_stuffer *in)
{
    RESULT_ENSURE_REF(conn);

    const struct s2n_signature_scheme **chosen_sig_scheme = NULL;
    s2n_mode peer_mode = S2N_PEER_MODE(conn->mode);
    if (peer_mode == S2N_CLIENT) {
        chosen_sig_scheme = &conn->handshake_params.client_cert_sig_scheme;
    } else {
        chosen_sig_scheme = &conn->handshake_params.server_cert_sig_scheme;
    }

    /* Before TLS1.2, signature algorithms were fixed instead of negotiated */
    if (conn->actual_protocol_version < S2N_TLS12) {
        return s2n_signature_algorithms_get_legacy_default(conn, peer_mode, chosen_sig_scheme);
    }

    uint16_t iana_value = 0;
    RESULT_ENSURE(s2n_stuffer_read_uint16(in, &iana_value) == S2N_SUCCESS,
            S2N_ERR_BAD_MESSAGE);

    const struct s2n_signature_preferences *signature_preferences = NULL;
    RESULT_GUARD_POSIX(s2n_connection_get_signature_preferences(conn, &signature_preferences));
    RESULT_ENSURE_REF(signature_preferences);

    for (size_t i = 0; i < signature_preferences->count; i++) {
        const struct s2n_signature_scheme *candidate = signature_preferences->signature_schemes[i];

        if (candidate->iana_value != iana_value) {
            continue;
        }

        if (!s2n_signature_scheme_is_valid_for_recv(conn, candidate)) {
            continue;
        }

        *chosen_sig_scheme = candidate;
        return S2N_RESULT_OK;
    }

    RESULT_BAIL(S2N_ERR_INVALID_SIGNATURE_SCHEME);
}

static S2N_RESULT s2n_signature_algorithms_validate_supported_by_peer(
        struct s2n_connection *conn, uint16_t iana)
{
    RESULT_ENSURE_REF(conn);

    const struct s2n_sig_scheme_list *peer_list = &conn->handshake_params.peer_sig_scheme_list;
    for (size_t i = 0; i < peer_list->len; i++) {
        if (peer_list->iana_list[i] == iana) {
            return S2N_RESULT_OK;
        }
    }

    RESULT_BAIL(S2N_ERR_NO_VALID_SIGNATURE_SCHEME);
}

static bool s2n_signature_algorithm_is_supported_by_peer(
        struct s2n_connection *conn, uint16_t iana)
{
    return s2n_result_is_ok(s2n_signature_algorithms_validate_supported_by_peer(conn, iana));
}

S2N_RESULT s2n_signature_algorithm_select(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->secure);
    struct s2n_cipher_suite *cipher_suite = conn->secure->cipher_suite;
    RESULT_ENSURE_REF(cipher_suite);

    const struct s2n_signature_scheme **chosen_sig_scheme = NULL;
    if (conn->mode == S2N_CLIENT) {
        chosen_sig_scheme = &conn->handshake_params.client_cert_sig_scheme;
    } else {
        chosen_sig_scheme = &conn->handshake_params.server_cert_sig_scheme;
    }

    /* Before TLS1.2, signature algorithms were fixed instead of negotiated */
    if (conn->actual_protocol_version < S2N_TLS12) {
        RESULT_GUARD(s2n_signature_algorithms_get_legacy_default(conn, conn->mode, chosen_sig_scheme));
        return S2N_RESULT_OK;
    }

    const struct s2n_signature_preferences *signature_preferences = NULL;
    RESULT_GUARD_POSIX(s2n_connection_get_signature_preferences(conn, &signature_preferences));
    RESULT_ENSURE_REF(signature_preferences);

    const struct s2n_signature_scheme *fallback_candidate = NULL;

    /* We use local preference order, not peer preference order, so we iterate
     * over the local preferences instead of over the options offered by the peer.
     */
    for (size_t i = 0; i < signature_preferences->count; i++) {
        const struct s2n_signature_scheme *candidate = signature_preferences->signature_schemes[i];

        /* Validates that a signature is valid to choose,
         * including that it's allowed by the current protocol version.
         */
        if (!s2n_signature_scheme_is_valid_for_recv(conn, candidate)) {
            continue;
        }

        if (s2n_is_sig_scheme_valid_for_auth(conn, candidate) != S2N_SUCCESS) {
            continue;
        }

        /* s2n-tls first attempts to choose a signature algorithm offered by the peer.
         * However, if that is not possible, we will attempt to continue the handshake
         * anyway with an algorithm not offered by the peer. This fallback behavior
         * is allowed by the RFC for TLS1.3 servers and partially allowed for TLS1.2
         * servers that don't receive the signature_algorithms extension, but is
         * otherwise an intentional deviation from the RFC.
         *
         * TLS1.3 servers:
         *= https://www.rfc-editor.org/rfc/rfc8446#section-4.4.3
         *# If the CertificateVerify message is sent by a server, the signature
         *# algorithm MUST be one offered in the client's "signature_algorithms"
         *# extension unless no valid certificate chain can be produced without
         *# unsupported algorithms
         *
         * TLS1.3 clients:
         *= https://www.rfc-editor.org/rfc/rfc8446#section-4.4.3
         *= type=exception
         *= reason=Compatibility with hypothetical faulty peers
         *# If sent by a client, the signature algorithm used in the signature
         *# MUST be one of those present in the supported_signature_algorithms
         *# field of the "signature_algorithms" extension in the
         *# CertificateRequest message.
         *
         * TLS1.2 servers:
         *= https://www.rfc-editor.org/rfc/rfc5246#section-7.4.3
         *= type=exception
         *= reason=Compatibility with known faulty peers
         *# If the client has offered the "signature_algorithms" extension, the
         *# signature algorithm and hash algorithm MUST be a pair listed in that
         *# extension.
         *
         * TLS1.2 clients:
         *= https://www.rfc-editor.org/rfc/rfc5246#section-7.4.8
         *= type=exception
         *= reason=Compatibility with hypothetical faulty peers
         *# The hash and signature algorithms used in the signature MUST be
         *# one of those present in the supported_signature_algorithms field
         *# of the CertificateRequest message.
         */
        bool is_peer_supported = s2n_signature_algorithm_is_supported_by_peer(
                conn, candidate->iana_value);
        if (is_peer_supported) {
            *chosen_sig_scheme = candidate;
            return S2N_RESULT_OK;
        }

        /**
         *= https://www.rfc-editor.org/rfc/rfc5246#section-7.4.1.4.1
         *# If the client does not send the signature_algorithms extension, the
         *# server MUST do the following:
         *#
         *# -  If the negotiated key exchange algorithm is one of (RSA, DHE_RSA,
         *#    DH_RSA, RSA_PSK, ECDH_RSA, ECDHE_RSA), behave as if client had
         *#    sent the value {sha1,rsa}.
         *#
         *# -  If the negotiated key exchange algorithm is one of (DHE_DSS,
         *#    DH_DSS), behave as if the client had sent the value {sha1,dsa}.
         *#
         *# -  If the negotiated key exchange algorithm is one of (ECDH_ECDSA,
         *#    ECDHE_ECDSA), behave as if the client had sent value {sha1,ecdsa}.
         *
         * The default scheme for DSA is not used because s2n-tls does not support DSA certificates.
         *
         * These defaults are only relevant for TLS1.2, since TLS1.3 does not allow SHA1.
         */
        bool is_default = (candidate == &s2n_ecdsa_sha1 || candidate == &s2n_rsa_pkcs1_sha1);

        /* If we ultimately cannot choose any algorithm offered by the peer,
         * we will attempt negotiation with an algorithm not offered by the peer.
         *
         * The TLS1.2 RFC specifies default algorithms for use when no signature_algorithms
         * extension is sent-- see the definition of is_default above.
         *
         * s2n-tls has encountered clients in the wild that support the TLS1.2
         * default algorithms but do not include them in their signature_algorithms
         * extension, likely due to a misreading of the RFC. So s2n-tls attempts
         * to use the TLS1.2 defaults even when the client sends the signature_algorithms
         * extension, and always treats them as the most preferred fallback option.
         *
         * If the TLS1.2 defaults are not possible-- for example, because TLS1.3
         * or the security policy forbids SHA1-- we fallback to our own most
         * preferred algorithm. In most cases a correctly implemented peer will reject
         * this fallback, but the only alternative is to kill the connection here.
         */
        if (is_default) {
            fallback_candidate = candidate;
        } else if (fallback_candidate == NULL) {
            fallback_candidate = candidate;
        }
    }

    if (fallback_candidate) {
        *chosen_sig_scheme = fallback_candidate;
    } else {
        RESULT_BAIL(S2N_ERR_NO_VALID_SIGNATURE_SCHEME);
    }
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_signature_algorithms_supported_list_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    const struct s2n_signature_preferences *signature_preferences = NULL;
    RESULT_GUARD_POSIX(s2n_connection_get_signature_preferences(conn, &signature_preferences));
    RESULT_ENSURE_REF(signature_preferences);

    struct s2n_stuffer_reservation size = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_reserve_uint16(out, &size));

    for (size_t i = 0; i < signature_preferences->count; i++) {
        const struct s2n_signature_scheme *const scheme = signature_preferences->signature_schemes[i];
        RESULT_ENSURE_REF(scheme);
        if (s2n_signature_scheme_is_valid_for_send(conn, scheme)) {
            RESULT_GUARD_POSIX(s2n_stuffer_write_uint16(out, scheme->iana_value));
        }
    }
    RESULT_GUARD_POSIX(s2n_stuffer_write_vector_size(&size));

    return S2N_RESULT_OK;
}

int s2n_recv_supported_sig_scheme_list(struct s2n_stuffer *in, struct s2n_sig_scheme_list *sig_hash_algs)
{
    uint16_t length_of_all_pairs = 0;
    POSIX_GUARD(s2n_stuffer_read_uint16(in, &length_of_all_pairs));
    if (length_of_all_pairs > s2n_stuffer_data_available(in)) {
        /* Malformed length, ignore the extension */
        return 0;
    }

    if (length_of_all_pairs % 2) {
        /* Pairs occur in two byte lengths. Malformed length, ignore the extension and skip ahead */
        POSIX_GUARD(s2n_stuffer_skip_read(in, length_of_all_pairs));
        return 0;
    }

    int pairs_available = length_of_all_pairs / 2;

    if (pairs_available > TLS_SIGNATURE_SCHEME_LIST_MAX_LEN) {
        POSIX_BAIL(S2N_ERR_TOO_MANY_SIGNATURE_SCHEMES);
    }

    sig_hash_algs->len = 0;

    for (size_t i = 0; i < (size_t) pairs_available; i++) {
        uint16_t sig_scheme = 0;
        POSIX_GUARD(s2n_stuffer_read_uint16(in, &sig_scheme));

        sig_hash_algs->iana_list[sig_hash_algs->len] = sig_scheme;
        sig_hash_algs->len += 1;
    }

    return 0;
}

S2N_RESULT s2n_signature_algorithm_get_pkey_type(s2n_signature_algorithm sig_alg, s2n_pkey_type *pkey_type)
{
    RESULT_ENSURE_REF(pkey_type);
    *pkey_type = S2N_PKEY_TYPE_UNKNOWN;

    switch (sig_alg) {
        case S2N_SIGNATURE_RSA:
        case S2N_SIGNATURE_RSA_PSS_RSAE:
            *pkey_type = S2N_PKEY_TYPE_RSA;
            break;
        case S2N_SIGNATURE_RSA_PSS_PSS:
            *pkey_type = S2N_PKEY_TYPE_RSA_PSS;
            break;
        case S2N_SIGNATURE_ECDSA:
            *pkey_type = S2N_PKEY_TYPE_ECDSA;
            break;
        default:
            RESULT_BAIL(S2N_ERR_INVALID_SIGNATURE_ALGORITHM);
    }

    return S2N_RESULT_OK;
}
