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
#include "crypto/s2n_certificate.h"
#include "error/s2n_errno.h"
#include "extensions/s2n_cert_authorities.h"
#include "extensions/s2n_extension_list.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_config.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_signature_algorithms.h"
#include "tls/s2n_signature_scheme.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_array.h"
#include "utils/s2n_safety.h"

/* RFC's that define below values:
 *  - https://tools.ietf.org/html/rfc5246#section-7.4.4
 *  - https://tools.ietf.org/search/rfc4492#section-5.5
 */
typedef enum {
    S2N_CERT_TYPE_RSA_SIGN = 1,
    S2N_CERT_TYPE_DSS_SIGN = 2,
    S2N_CERT_TYPE_RSA_FIXED_DH = 3,
    S2N_CERT_TYPE_DSS_FIXED_DH = 4,
    S2N_CERT_TYPE_RSA_EPHEMERAL_DH_RESERVED = 5,
    S2N_CERT_TYPE_DSS_EPHEMERAL_DH_RESERVED = 6,
    S2N_CERT_TYPE_FORTEZZA_DMS_RESERVED = 20,
    S2N_CERT_TYPE_ECDSA_SIGN = 64,
    S2N_CERT_TYPE_RSA_FIXED_ECDH = 65,
    S2N_CERT_TYPE_ECDSA_FIXED_ECDH = 66,
} s2n_cert_type;

static uint8_t s2n_cert_type_preference_list[] = {
    S2N_CERT_TYPE_RSA_SIGN,
    S2N_CERT_TYPE_ECDSA_SIGN
};

/*
 * Include DSS sign certificate type in server certificate request.
 * Only will be used if cert_req_dss_legacy_compat_enabled is set by calling
 * s2n_config_enable_cert_req_dss_legacy_compat.
 */
static uint8_t s2n_cert_type_preference_list_legacy_dss[] = {
    S2N_CERT_TYPE_RSA_SIGN,
    S2N_CERT_TYPE_DSS_SIGN,
    S2N_CERT_TYPE_ECDSA_SIGN
};

static int s2n_recv_client_cert_preferences(struct s2n_stuffer *in, s2n_cert_type *chosen_cert_type_out)
{
    uint8_t cert_types_len = 0;
    POSIX_GUARD(s2n_stuffer_read_uint8(in, &cert_types_len));

    uint8_t *their_cert_type_pref_list = s2n_stuffer_raw_read(in, cert_types_len);
    POSIX_ENSURE_REF(their_cert_type_pref_list);

    /* Iterate through our preference list from most to least preferred, and return the first match that we find. */
    for (size_t our_cert_pref_idx = 0; our_cert_pref_idx < s2n_array_len(s2n_cert_type_preference_list); our_cert_pref_idx++) {
        for (int their_cert_idx = 0; their_cert_idx < cert_types_len; their_cert_idx++) {
            if (their_cert_type_pref_list[their_cert_idx] == s2n_cert_type_preference_list[our_cert_pref_idx]) {
                *chosen_cert_type_out = s2n_cert_type_preference_list[our_cert_pref_idx];
                return 0;
            }
        }
    }

    POSIX_BAIL(S2N_ERR_CERT_TYPE_UNSUPPORTED);
}

struct s2n_certificate_authority_list {
    struct s2n_stuffer iterator;
};

struct s2n_certificate_request {
    struct s2n_certificate_authority_list list;
    struct s2n_cert_chain_and_key *chain;
};

struct s2n_certificate_authority_list *s2n_certificate_request_get_ca_list(struct s2n_certificate_request *request)
{
    return request != NULL ? &request->list : NULL;
}

int s2n_certificate_request_set_certificate(struct s2n_certificate_request *request, struct s2n_cert_chain_and_key *chain)
{
    POSIX_ENSURE(request, S2N_ERR_INVALID_ARGUMENT);
    POSIX_ENSURE(chain, S2N_ERR_INVALID_ARGUMENT);
    request->chain = chain;
    return S2N_SUCCESS;
}

bool s2n_certificate_authority_list_has_next(struct s2n_certificate_authority_list *list)
{
    return list != NULL && s2n_stuffer_data_available(&list->iterator) > 0;
}

S2N_RESULT s2n_certificate_authority_list_read_next(struct s2n_certificate_authority_list *list, uint8_t **name, uint16_t *length)
{
    RESULT_ENSURE_REF(list);
    RESULT_ENSURE_MUT(length);
    RESULT_ENSURE_MUT(name);

    RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(&list->iterator, length));
    RESULT_ENSURE_GT(*length, 0);

    *name = s2n_stuffer_raw_read(&list->iterator, *length);
    RESULT_ENSURE_REF(*name);

    return S2N_RESULT_OK;
}

int s2n_certificate_authority_list_next(struct s2n_certificate_authority_list *list, uint8_t **name, uint16_t *length)
{
    POSIX_ENSURE(list, S2N_ERR_INVALID_ARGUMENT);
    POSIX_ENSURE(name, S2N_ERR_INVALID_ARGUMENT);
    POSIX_ENSURE(length, S2N_ERR_INVALID_ARGUMENT);

    /* Ensure that if we exit due to errors we've cleared these fields */
    *name = NULL;
    *length = 0;

    POSIX_ENSURE(s2n_certificate_authority_list_has_next(list), S2N_ERR_INVALID_ARGUMENT);
    POSIX_ENSURE(s2n_result_is_ok(s2n_certificate_authority_list_read_next(list, name, length)), S2N_ERR_BAD_MESSAGE);
    return S2N_SUCCESS;
}

int s2n_certificate_authority_list_reread(struct s2n_certificate_authority_list *list)
{
    POSIX_ENSURE(list, S2N_ERR_INVALID_ARGUMENT);
    return s2n_stuffer_reread(&list->iterator);
}

static int s2n_set_cert_chain_as_client(struct s2n_connection *conn)
{
    if (s2n_config_get_num_default_certs(conn->config) > 0) {
        struct s2n_cert_chain_and_key *cert = NULL;
        if (conn->config->cert_request_cb) {
            struct s2n_certificate_request request = { 0 };
            /* Don't try to free this stuffer, it's purely for reading pre-existing blob that outlives it. */
            POSIX_GUARD(s2n_stuffer_init_written(&request.list.iterator, &conn->cert_authorities));
            POSIX_ENSURE(conn->config->cert_request_cb(conn, conn->config->cert_request_cb_ctx, &request) == S2N_SUCCESS, S2N_ERR_CANCELLED);
            cert = request.chain;
            POSIX_ENSURE(cert, S2N_ERR_NO_CERT_FOUND);
        } else {
            cert = s2n_config_get_single_default_cert(conn->config);
            POSIX_ENSURE_REF(cert);
        }
        conn->handshake_params.our_chain_and_key = cert;
        conn->handshake_params.client_cert_pkey_type = s2n_cert_chain_and_key_get_pkey_type(cert);

        POSIX_GUARD_RESULT(s2n_signature_algorithm_select(conn));
    }

    return 0;
}

int s2n_tls13_cert_req_recv(struct s2n_connection *conn)
{
    struct s2n_stuffer *in = &conn->handshake.io;

    /* read request context length */
    uint8_t request_context_length = 0;
    POSIX_GUARD(s2n_stuffer_read_uint8(in, &request_context_length));
    /* RFC 8446: This field SHALL be zero length unless used for the post-handshake authentication */
    S2N_ERROR_IF(request_context_length != 0, S2N_ERR_BAD_MESSAGE);

    POSIX_GUARD(s2n_extension_list_recv(S2N_EXTENSION_LIST_CERT_REQ, conn, in));

    POSIX_GUARD(s2n_set_cert_chain_as_client(conn));

    return S2N_SUCCESS;
}

int s2n_cert_req_recv(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->config);
    POSIX_ENSURE_EQ(conn->mode, S2N_CLIENT);

    struct s2n_stuffer *in = &conn->handshake.io;

    s2n_cert_type cert_type = 0;
    POSIX_GUARD(s2n_recv_client_cert_preferences(in, &cert_type));

    if (conn->actual_protocol_version == S2N_TLS12) {
        POSIX_GUARD(s2n_recv_supported_sig_scheme_list(in, &conn->handshake_params.peer_sig_scheme_list));
    }

    uint16_t cert_authorities_len = 0;
    POSIX_GUARD(s2n_stuffer_read_uint16(in, &cert_authorities_len));

    /* Stash the certificate authorities into the connection for later parsing
     * as part of certificate lookup. Note that in TLS 1.3 these are handled as
     * an extension in the ClientHello or CertificateRequest messages, rather
     * than a field of the CertificateRequest message.
     *
     * The extension is handled in tls/extensions/s2n_cert_authorities.c.
     */
    if (conn->config->cert_request_cb) {
        POSIX_GUARD(s2n_stuffer_extract_blob(in, &conn->cert_authorities));
        POSIX_ENSURE_EQ(conn->cert_authorities.size, cert_authorities_len);
    } else {
        POSIX_GUARD(s2n_stuffer_skip_read(in, cert_authorities_len));
    }

    /* In the future we may have more advanced logic to match a set of configured certificates against
     * The cert authorities extension and the signature algorithms advertised.
     * For now, this will just set the only certificate configured.
     */
    POSIX_GUARD(s2n_set_cert_chain_as_client(conn));

    return 0;
}

int s2n_tls13_cert_req_send(struct s2n_connection *conn)
{
    struct s2n_stuffer *out = &conn->handshake.io;

    /* Write 0 length request context https://tools.ietf.org/html/rfc8446#section-4.3.2 */
    POSIX_GUARD(s2n_stuffer_write_uint8(out, 0));

    POSIX_GUARD(s2n_extension_list_send(S2N_EXTENSION_LIST_CERT_REQ, conn, out));

    return S2N_SUCCESS;
}

int s2n_cert_req_send(struct s2n_connection *conn)
{
    struct s2n_stuffer *out = &conn->handshake.io;

    uint8_t client_cert_preference_list_size = sizeof(s2n_cert_type_preference_list);
    if (conn->config->cert_req_dss_legacy_compat_enabled) {
        client_cert_preference_list_size = sizeof(s2n_cert_type_preference_list_legacy_dss);
    }
    POSIX_GUARD(s2n_stuffer_write_uint8(out, client_cert_preference_list_size));

    for (int i = 0; i < client_cert_preference_list_size; i++) {
        if (conn->config->cert_req_dss_legacy_compat_enabled) {
            POSIX_GUARD(s2n_stuffer_write_uint8(out, s2n_cert_type_preference_list_legacy_dss[i]));
        } else {
            POSIX_GUARD(s2n_stuffer_write_uint8(out, s2n_cert_type_preference_list[i]));
        }
    }

    if (conn->actual_protocol_version == S2N_TLS12) {
        POSIX_GUARD_RESULT(s2n_signature_algorithms_supported_list_send(conn, out));
    }

    /* Before TLS1.3, certificate_authorities is part of the message instead of an extension */
    POSIX_GUARD(s2n_cert_authorities_send(conn, out));

    return S2N_SUCCESS;
}
