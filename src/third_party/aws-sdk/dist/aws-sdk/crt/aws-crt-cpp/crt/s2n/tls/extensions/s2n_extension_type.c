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

#include "tls/extensions/s2n_extension_type.h"

#include "api/s2n.h"
#include "error/s2n_errno.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_tls13.h"
#include "utils/s2n_bitmap.h"
#include "utils/s2n_safety.h"

#define TLS_EXTENSION_DATA_LENGTH_BYTES 2

/* Because there are 65536 possible extension IANAs, we will only
 * put the lowest (and most common) in a lookup table to conserve space. */
#define S2N_MAX_INDEXED_EXTENSION_IANA 60

const s2n_extension_type_id s2n_unsupported_extension = S2N_SUPPORTED_EXTENSIONS_COUNT;
s2n_extension_type_id s2n_extension_ianas_to_ids[S2N_MAX_INDEXED_EXTENSION_IANA];

int s2n_extension_type_init()
{
    /* Initialize to s2n_unsupported_extension */
    for (size_t i = 0; i < S2N_MAX_INDEXED_EXTENSION_IANA; i++) {
        s2n_extension_ianas_to_ids[i] = s2n_unsupported_extension;
    }

    /* Reverse the mapping */
    for (size_t i = 0; i < S2N_SUPPORTED_EXTENSIONS_COUNT; i++) {
        uint16_t iana_value = s2n_supported_extensions[i];
        if (iana_value < S2N_MAX_INDEXED_EXTENSION_IANA) {
            s2n_extension_ianas_to_ids[iana_value] = i;
        }
    }

    return S2N_SUCCESS;
}

/* Convert the IANA value (which ranges from 0->65535) to an id with a more
 * constrained range. That id can be used for bitfields, array indexes, etc.
 * to avoid allocating too much memory. */
s2n_extension_type_id s2n_extension_iana_value_to_id(const uint16_t iana_value)
{
    /* Check the lookup table */
    if (iana_value < S2N_MAX_INDEXED_EXTENSION_IANA) {
        return s2n_extension_ianas_to_ids[iana_value];
    }

    /* Fall back to the full list. We can handle this more
     * efficiently later if our extension list gets long. */
    for (size_t i = 0; i < S2N_SUPPORTED_EXTENSIONS_COUNT; i++) {
        if (s2n_supported_extensions[i] == iana_value) {
            return i;
        }
    }

    return s2n_unsupported_extension;
}

int s2n_extension_supported_iana_value_to_id(const uint16_t iana_value, s2n_extension_type_id *internal_id)
{
    POSIX_ENSURE_REF(internal_id);

    *internal_id = s2n_extension_iana_value_to_id(iana_value);
    S2N_ERROR_IF(*internal_id == s2n_unsupported_extension, S2N_ERR_UNRECOGNIZED_EXTENSION);
    return S2N_SUCCESS;
}

int s2n_extension_send(const s2n_extension_type *extension_type, struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(extension_type);
    POSIX_ENSURE_REF(extension_type->should_send);
    POSIX_ENSURE_REF(extension_type->send);
    POSIX_ENSURE_REF(conn);

    s2n_extension_type_id extension_id = 0;
    POSIX_GUARD(s2n_extension_supported_iana_value_to_id(extension_type->iana_value, &extension_id));

    /* Do not send response if request not received. */
    if (extension_type->is_response && !S2N_CBIT_TEST(conn->extension_requests_received, extension_id)) {
        return S2N_SUCCESS;
    }

    /* Do not send an extension that is not valid for the protocol version */
    if (extension_type->minimum_version > conn->actual_protocol_version) {
        return S2N_SUCCESS;
    }

    /* Check if we need to send. Some extensions are only sent if specific conditions are met. */
    if (!extension_type->should_send(conn)) {
        return S2N_SUCCESS;
    }

    /* Write extension type */
    POSIX_GUARD(s2n_stuffer_write_uint16(out, extension_type->iana_value));

    /* Reserve space for extension size */
    struct s2n_stuffer_reservation extension_size_bytes = { 0 };
    POSIX_GUARD(s2n_stuffer_reserve_uint16(out, &extension_size_bytes));

    /* Write extension data */
    POSIX_GUARD(extension_type->send(conn, out));

    /* Record extension size */
    POSIX_GUARD(s2n_stuffer_write_vector_size(&extension_size_bytes));

    /* Set request bit flag */
    if (!extension_type->is_response) {
        S2N_CBIT_SET(conn->extension_requests_sent, extension_id);
    }

    return S2N_SUCCESS;
}

int s2n_extension_recv(const s2n_extension_type *extension_type, struct s2n_connection *conn, struct s2n_stuffer *in)
{
    POSIX_ENSURE_REF(extension_type);
    POSIX_ENSURE_REF(extension_type->recv);
    POSIX_ENSURE_REF(conn);

    s2n_extension_type_id extension_id = 0;
    POSIX_GUARD(s2n_extension_supported_iana_value_to_id(extension_type->iana_value, &extension_id));

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2
     *# Implementations MUST NOT send extension responses if the remote
     *# endpoint did not send the corresponding extension requests, with the
     *# exception of the "cookie" extension in the HelloRetryRequest.  Upon
     *# receiving such an extension, an endpoint MUST abort the handshake
     *# with an "unsupported_extension" alert.
     *
     *= https://www.rfc-editor.org/rfc/rfc7627#section-5.3
     *# If the original session did not use the "extended_master_secret"
     *# extension but the new ServerHello contains the extension, the
     *# client MUST abort the handshake.
     *
     *= https://www.rfc-editor.org/rfc/rfc8446#4.1.4
     *# As with the ServerHello, a HelloRetryRequest MUST NOT contain any
     *# extensions that were not first offered by the client in its
     *# ClientHello, with the exception of optionally the "cookie" (see
     *# Section 4.2.2) extension.
     **/
    if (extension_type->is_response && !S2N_CBIT_TEST(conn->extension_requests_sent, extension_id)) {
        POSIX_BAIL(S2N_ERR_UNSUPPORTED_EXTENSION);
    }

    /* Do not process an extension not valid for the protocol version */
    if (extension_type->minimum_version > conn->actual_protocol_version) {
        return S2N_SUCCESS;
    }

    POSIX_GUARD(extension_type->recv(conn, in));

    /* Set request bit flag */
    if (extension_type->is_response) {
        S2N_CBIT_SET(conn->extension_responses_received, extension_id);
    } else {
        S2N_CBIT_SET(conn->extension_requests_received, extension_id);
    }

    return S2N_SUCCESS;
}

int s2n_extension_is_missing(const s2n_extension_type *extension_type, struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(extension_type);
    POSIX_ENSURE_REF(extension_type->if_missing);
    POSIX_ENSURE_REF(conn);

    s2n_extension_type_id extension_id = 0;
    POSIX_GUARD(s2n_extension_supported_iana_value_to_id(extension_type->iana_value, &extension_id));

    /* Do not consider an extension missing if we did not send a request */
    if (extension_type->is_response && !S2N_CBIT_TEST(conn->extension_requests_sent, extension_id)) {
        return S2N_SUCCESS;
    }

    /* Do not consider an extension missing if it is not valid for the protocol version */
    if (extension_type->minimum_version > conn->actual_protocol_version) {
        return S2N_SUCCESS;
    }

    POSIX_GUARD(extension_type->if_missing(conn));

    return S2N_SUCCESS;
}

int s2n_extension_send_unimplemented(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
}

int s2n_extension_recv_unimplemented(struct s2n_connection *conn, struct s2n_stuffer *in)
{
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
}

int s2n_extension_send_noop(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    return S2N_SUCCESS;
}

int s2n_extension_recv_noop(struct s2n_connection *conn, struct s2n_stuffer *in)
{
    return S2N_SUCCESS;
}

bool s2n_extension_always_send(struct s2n_connection *conn)
{
    return true;
}

bool s2n_extension_never_send(struct s2n_connection *conn)
{
    return false;
}

bool s2n_extension_send_if_tls13_connection(struct s2n_connection *conn)
{
    return s2n_connection_get_protocol_version(conn) >= S2N_TLS13;
}

int s2n_extension_error_if_missing(struct s2n_connection *conn)
{
    POSIX_BAIL(S2N_ERR_MISSING_EXTENSION);
}

int s2n_extension_noop_if_missing(struct s2n_connection *conn)
{
    return S2N_SUCCESS;
}
