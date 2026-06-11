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

#include "tls/extensions/s2n_cert_status.h"

#include "tls/s2n_config.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_x509_validator.h"
#include "utils/s2n_safety.h"

#define U24_SIZE 3

static bool s2n_cert_status_should_send(struct s2n_connection *conn);

/*
 * The cert_status extension is sent in response to OCSP status requests in TLS 1.3. The
 * OCSP response is contained in the extension data. In TLS 1.2, the cert_status_response
 * extension is sent instead, indicating that the OCSP response will be sent in a
 * Certificate Status handshake message.
 */
const s2n_extension_type s2n_cert_status_extension = {
    .iana_value = TLS_EXTENSION_STATUS_REQUEST,
    .is_response = true,
    .send = s2n_cert_status_send,
    .recv = s2n_cert_status_recv,
    .should_send = s2n_cert_status_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_cert_status_should_send(struct s2n_connection *conn)
{
    return conn->handshake_params.our_chain_and_key
            && conn->handshake_params.our_chain_and_key->ocsp_status.size > 0;
}

int s2n_cert_status_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);
    struct s2n_blob *ocsp_status = &conn->handshake_params.our_chain_and_key->ocsp_status;
    POSIX_ENSURE_REF(ocsp_status);

    POSIX_GUARD(s2n_stuffer_write_uint8(out, (uint8_t) S2N_STATUS_REQUEST_OCSP));
    POSIX_GUARD(s2n_stuffer_write_uint24(out, ocsp_status->size));
    POSIX_GUARD(s2n_stuffer_write(out, ocsp_status));

    return S2N_SUCCESS;
}

int s2n_cert_status_recv(struct s2n_connection *conn, struct s2n_stuffer *in)
{
    POSIX_ENSURE_REF(conn);
    /**
     *= https://www.rfc-editor.org/rfc/rfc6066#section-8
     *#   struct {
     *#       CertificateStatusType status_type;
     *#       select (status_type) {
     *#          case ocsp: OCSPResponse;
     *#       } response;
     *#   } CertificateStatus;
     *#
     *#   opaque OCSPResponse<1..2^24-1>;
     *#
     *# An "ocsp_response" contains a complete, DER-encoded OCSP response
     *# (using the ASN.1 type OCSPResponse defined in [RFC2560]).  Only one
     *# OCSP response may be sent.
     **/
    uint8_t type = 0;
    POSIX_GUARD(s2n_stuffer_read_uint8(in, &type));
    if (type != S2N_STATUS_REQUEST_OCSP) {
        /* We only support OCSP */
        return S2N_SUCCESS;
    }

    /* The status_type variable is only used when a client requests OCSP stapling from a
     * server. A server can request OCSP stapling from a client, but it is not tracked
     * with this variable.
     */
    if (conn->mode == S2N_CLIENT) {
        conn->status_type = S2N_STATUS_REQUEST_OCSP;
    }

    uint32_t status_size = 0;
    POSIX_GUARD(s2n_stuffer_read_uint24(in, &status_size));
    POSIX_ENSURE_LTE(status_size, s2n_stuffer_data_available(in));

    POSIX_GUARD(s2n_realloc(&conn->status_response, status_size));
    POSIX_GUARD(s2n_stuffer_read_bytes(in, conn->status_response.data, status_size));

    POSIX_GUARD_RESULT(s2n_x509_validator_validate_cert_stapled_ocsp_response(&conn->x509_validator, conn,
            conn->status_response.data, conn->status_response.size));

    return S2N_SUCCESS;
}
