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
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_config.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

/* In TLS1.2, the certificate list is just an opaque vector of certificates:
 *
 *      opaque ASN.1Cert<1..2^24-1>;
 *
 *      struct {
 *          ASN.1Cert certificate_list<0..2^24-1>;
 *      } Certificate;
 *
 * This construction allowed us to store the entire certificate_list blob
 * and return it from the s2n_connection_get_client_cert_chain method for
 * customers to examine.
 *
 * However, TLS1.3 introduced per-certificate extensions:
 *
 *      struct {
 *          opaque cert_data<1..2^24-1>;
 * ---->    Extension extensions<0..2^16-1>;    <----
 *      } CertificateEntry;
 *
 *      struct {
 *          opaque certificate_request_context<0..2^8-1>;
 *          CertificateEntry certificate_list<0..2^24-1>;
 *      } Certificate;
 *
 * So in order to store / return the certificates in the same format as in TLS1.2,
 * we need to first strip out the extensions.
 */
static S2N_RESULT s2n_client_cert_chain_store(struct s2n_connection *conn,
        struct s2n_blob *raw_cert_chain)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(raw_cert_chain);

    /* There shouldn't already be a client cert chain, but free just in case */
    RESULT_GUARD_POSIX(s2n_free(&conn->handshake_params.client_cert_chain));

    /* Earlier versions are a basic copy */
    if (conn->actual_protocol_version < S2N_TLS13) {
        RESULT_GUARD_POSIX(s2n_dup(raw_cert_chain, &conn->handshake_params.client_cert_chain));
        return S2N_RESULT_OK;
    }

    DEFER_CLEANUP(struct s2n_blob output = { 0 }, s2n_free);
    RESULT_GUARD_POSIX(s2n_realloc(&output, raw_cert_chain->size));

    struct s2n_stuffer cert_chain_in = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init_written(&cert_chain_in, raw_cert_chain));

    struct s2n_stuffer cert_chain_out = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init(&cert_chain_out, &output));

    uint32_t cert_size = 0;
    uint16_t extensions_size = 0;
    while (s2n_stuffer_data_available(&cert_chain_in)) {
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint24(&cert_chain_in, &cert_size));
        RESULT_GUARD_POSIX(s2n_stuffer_write_uint24(&cert_chain_out, cert_size));
        RESULT_GUARD_POSIX(s2n_stuffer_copy(&cert_chain_in, &cert_chain_out, cert_size));

        /* The new TLS1.3 format includes extensions, which we must skip.
         * Customers will not expect TLS extensions in a DER-encoded certificate.
         */
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(&cert_chain_in, &extensions_size));
        RESULT_GUARD_POSIX(s2n_stuffer_skip_read(&cert_chain_in, extensions_size));
    }

    /* We will have allocated more memory than actually necessary.
     * If this becomes a problem, we should consider reallocing the correct amount of memory here.
     */
    output.size = s2n_stuffer_data_available(&cert_chain_out);

    conn->handshake_params.client_cert_chain = output;
    ZERO_TO_DISABLE_DEFER_CLEANUP(output);
    return S2N_RESULT_OK;
}

int s2n_client_cert_recv(struct s2n_connection *conn)
{
    if (conn->actual_protocol_version == S2N_TLS13) {
        uint8_t certificate_request_context_len = 0;
        POSIX_GUARD(s2n_stuffer_read_uint8(&conn->handshake.io, &certificate_request_context_len));
        S2N_ERROR_IF(certificate_request_context_len != 0, S2N_ERR_BAD_MESSAGE);
    }

    struct s2n_stuffer *in = &conn->handshake.io;

    uint32_t cert_chain_size = 0;
    POSIX_GUARD(s2n_stuffer_read_uint24(in, &cert_chain_size));
    POSIX_ENSURE(cert_chain_size <= s2n_stuffer_data_available(in), S2N_ERR_BAD_MESSAGE);
    if (cert_chain_size == 0) {
        POSIX_GUARD(s2n_conn_set_handshake_no_client_cert(conn));
        return S2N_SUCCESS;
    }

    uint8_t *cert_chain_data = s2n_stuffer_raw_read(in, cert_chain_size);
    POSIX_ENSURE_REF(cert_chain_data);

    struct s2n_blob cert_chain = { 0 };
    POSIX_GUARD(s2n_blob_init(&cert_chain, cert_chain_data, cert_chain_size));
    POSIX_ENSURE(s2n_result_is_ok(s2n_client_cert_chain_store(conn, &cert_chain)),
            S2N_ERR_BAD_MESSAGE);

    s2n_cert_public_key public_key = { 0 };
    POSIX_GUARD(s2n_pkey_zero_init(&public_key));

    /* Determine the Cert Type, Verify the Cert, and extract the Public Key */
    s2n_pkey_type pkey_type = S2N_PKEY_TYPE_UNKNOWN;
    POSIX_GUARD_RESULT(s2n_x509_validator_validate_cert_chain(&conn->x509_validator, conn, cert_chain_data,
            cert_chain_size, &pkey_type, &public_key));

    conn->handshake_params.client_cert_pkey_type = pkey_type;
    POSIX_GUARD_RESULT(s2n_pkey_setup_for_type(&public_key, pkey_type));

    POSIX_GUARD(s2n_pkey_check_key_exists(&public_key));
    conn->handshake_params.client_public_key = public_key;

    return S2N_SUCCESS;
}

int s2n_client_cert_send(struct s2n_connection *conn)
{
    struct s2n_cert_chain_and_key *chain_and_key = conn->handshake_params.our_chain_and_key;

    if (conn->actual_protocol_version >= S2N_TLS13) {
        /* If this message is in response to a CertificateRequest, the value of
         * certificate_request_context in that message.
         * https://tools.ietf.org/html/rfc8446#section-4.4.2
         *
         * This field SHALL be zero length unless used for the post-handshake authentication
         * https://tools.ietf.org/html/rfc8446#section-4.3.2
         */
        uint8_t certificate_request_context_len = 0;
        POSIX_GUARD(s2n_stuffer_write_uint8(&conn->handshake.io, certificate_request_context_len));
    }

    if (chain_and_key == NULL) {
        POSIX_GUARD(s2n_conn_set_handshake_no_client_cert(conn));
        POSIX_GUARD(s2n_send_empty_cert_chain(&conn->handshake.io));
        return 0;
    }

    POSIX_GUARD(s2n_send_cert_chain(conn, &conn->handshake.io, chain_and_key));
    return S2N_SUCCESS;
}
