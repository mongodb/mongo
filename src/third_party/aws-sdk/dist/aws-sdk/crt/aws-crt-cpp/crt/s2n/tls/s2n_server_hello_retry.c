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
#include <stdbool.h>

#include "crypto/s2n_pq.h"
#include "error/s2n_errno.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_server_extensions.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls13.h"
#include "tls/s2n_tls13_handshake.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

/* From RFC5246 7.4.1.2. */
#define S2N_TLS_COMPRESSION_METHOD_NULL 0

/* from RFC: https://tools.ietf.org/html/rfc8446#section-4.1.3*/
uint8_t hello_retry_req_random[S2N_TLS_RANDOM_DATA_LEN] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C
};

int s2n_server_hello_retry_send(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    POSIX_CHECKED_MEMCPY(conn->handshake_params.server_random, hello_retry_req_random, S2N_TLS_RANDOM_DATA_LEN);

    POSIX_GUARD(s2n_server_hello_write_message(conn));

    /* Write the extensions */
    POSIX_GUARD(s2n_server_extensions_send(conn, &conn->handshake.io));

    /* Update transcript */
    POSIX_GUARD(s2n_server_hello_retry_recreate_transcript(conn));

    /* Reset handshake values */
    conn->handshake.client_hello_received = 0;
    conn->client_hello.parsed = 0;
    POSIX_CHECKED_MEMSET((uint8_t *) conn->extension_requests_received, 0, sizeof(s2n_extension_bitfield));

    return 0;
}

int s2n_server_hello_retry_recv(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE(conn->actual_protocol_version >= S2N_TLS13, S2N_ERR_INVALID_HELLO_RETRY);

    const struct s2n_ecc_preferences *ecc_pref = NULL;
    POSIX_GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_pref));
    POSIX_ENSURE_REF(ecc_pref);

    const struct s2n_kem_preferences *kem_pref = NULL;
    POSIX_GUARD(s2n_connection_get_kem_preferences(conn, &kem_pref));
    POSIX_ENSURE_REF(kem_pref);

    const struct s2n_ecc_named_curve *named_curve = conn->kex_params.server_ecc_evp_params.negotiated_curve;
    const struct s2n_kem_group *server_preferred_kem_group = conn->kex_params.server_kem_group_params.kem_group;
    const struct s2n_kem_group *client_preferred_kem_group = conn->kex_params.client_kem_group_params.kem_group;

    /* Boolean XOR check: exactly one of {named_curve, kem_group} should be non-null. */
    POSIX_ENSURE((named_curve != NULL) != (server_preferred_kem_group != NULL), S2N_ERR_INVALID_HELLO_RETRY);

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#4.2.8
     *# Upon receipt of this extension in a HelloRetryRequest, the client
     *# MUST verify that (1) the selected_group field corresponds to a group
     *# which was provided in the "supported_groups" extension in the
     *# original ClientHello
     **/
    bool selected_group_in_supported_groups = false;
    if (named_curve != NULL && s2n_ecc_preferences_includes_curve(ecc_pref, named_curve->iana_id)) {
        selected_group_in_supported_groups = true;
    }
    if (server_preferred_kem_group != NULL
            && s2n_kem_group_is_available(server_preferred_kem_group)
            && s2n_kem_preferences_includes_tls13_kem_group(kem_pref, server_preferred_kem_group->iana_id)) {
        selected_group_in_supported_groups = true;
    }

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#4.2.8
     *# and (2) the selected_group field does not
     *# correspond to a group which was provided in the "key_share" extension
     *# in the original ClientHello.
     **/
    bool new_key_share_requested = false;

    if (named_curve != NULL) {
        new_key_share_requested = (named_curve != conn->kex_params.client_ecc_evp_params.negotiated_curve);
    }

    if (server_preferred_kem_group != NULL) {
        /* If PQ is disabled, the client should not have sent any PQ IDs
         * in the supported_groups list of the initial ClientHello */
        POSIX_ENSURE(s2n_pq_is_enabled(), S2N_ERR_INVALID_HELLO_RETRY);
        new_key_share_requested = (server_preferred_kem_group != client_preferred_kem_group);
    }

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#4.2.8
     *# If either of these checks fails, then
     *# the client MUST abort the handshake with an "illegal_parameter"
     *# alert.
     * 
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.1.4
     *# Clients MUST abort the handshake with an
     *# "illegal_parameter" alert if the HelloRetryRequest would not result
     *# in any change in the ClientHello.
     **/
    POSIX_ENSURE(new_key_share_requested, S2N_ERR_INVALID_HELLO_RETRY);
    POSIX_ENSURE(selected_group_in_supported_groups, S2N_ERR_INVALID_HELLO_RETRY);

    /* Update transcript hash */
    POSIX_GUARD(s2n_server_hello_retry_recreate_transcript(conn));

    /* Reset handshake values */
    POSIX_CHECKED_MEMSET((uint8_t *) conn->extension_requests_sent, 0, sizeof(s2n_extension_bitfield));

    return S2N_SUCCESS;
}
