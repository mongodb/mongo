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

#include "tls/extensions/s2n_client_key_share.h"

#include "crypto/s2n_pq.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/extensions/s2n_key_share.h"
#include "tls/s2n_kem_preferences.h"
#include "tls/s2n_security_policies.h"
#include "tls/s2n_tls13.h"
#include "utils/s2n_safety.h"

/**
 * Specified in https://tools.ietf.org/html/rfc8446#section-4.2.8
 * "The "key_share" extension contains the endpoint's cryptographic parameters."
 *
 * Structure:
 * Extension type (2 bytes)
 * Extension data size (2 bytes)
 * Client shares size (2 bytes)
 * Client shares:
 *      Named group (2 bytes)
 *      Key share size (2 bytes)
 *      Key share (variable size)
 *
 * This extension only modifies the connection's client ecc_evp_params. It does
 * not make any decisions about which set of params to use.
 *
 * The server will NOT alert when processing a client extension that violates the RFC.
 * So the server will accept:
 * - Multiple key shares for the same named group. The server will accept the first
 *   key share for the group and ignore any duplicates.
 * - Key shares for named groups not in the client's supported_groups extension.
 **/

static int s2n_client_key_share_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_client_key_share_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_client_key_share_extension = {
    .iana_value = TLS_EXTENSION_KEY_SHARE,
    .minimum_version = S2N_TLS13,
    .is_response = false,
    .send = s2n_client_key_share_send,
    .recv = s2n_client_key_share_recv,
    .should_send = s2n_extension_always_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static int s2n_generate_default_ecc_key_share(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);
    const struct s2n_ecc_preferences *ecc_pref = NULL;
    POSIX_GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_pref));
    POSIX_ENSURE_REF(ecc_pref);

    /* We only ever send a single EC key share: either the share requested by the server
     * during a retry, or the most preferred share according to local preferences.
     */
    struct s2n_ecc_evp_params *client_params = &conn->kex_params.client_ecc_evp_params;
    if (s2n_is_hello_retry_handshake(conn)) {
        const struct s2n_ecc_named_curve *server_curve = conn->kex_params.server_ecc_evp_params.negotiated_curve;

        /* If the server did not request a specific ECC keyshare, don't send one */
        if (!server_curve) {
            return S2N_SUCCESS;
        }

        /* If the server requested a new ECC keyshare, free the old one */
        if (server_curve != client_params->negotiated_curve) {
            POSIX_GUARD(s2n_ecc_evp_params_free(client_params));
        }

        /**
         *= https://www.rfc-editor.org/rfc/rfc8446#4.2.8
         *# Otherwise, when sending the new ClientHello, the client MUST
         *# replace the original "key_share" extension with one containing only a
         *# new KeyShareEntry for the group indicated in the selected_group field
         *# of the triggering HelloRetryRequest.
         **/
        client_params->negotiated_curve = server_curve;
    } else {
        client_params->negotiated_curve = ecc_pref->ecc_curves[0];
    }
    POSIX_GUARD(s2n_ecdhe_parameters_send(client_params, out));

    return S2N_SUCCESS;
}

static int s2n_generate_pq_hybrid_key_share(struct s2n_stuffer *out, struct s2n_kem_group_params *kem_group_params)
{
    POSIX_ENSURE_REF(out);
    POSIX_ENSURE_REF(kem_group_params);

    /* This function should never be called when PQ is disabled */
    POSIX_ENSURE(s2n_pq_is_enabled(), S2N_ERR_UNIMPLEMENTED);

    const struct s2n_kem_group *kem_group = kem_group_params->kem_group;
    POSIX_ENSURE_REF(kem_group);

    POSIX_GUARD(s2n_stuffer_write_uint16(out, kem_group->iana_id));

    struct s2n_stuffer_reservation total_share_size = { 0 };
    POSIX_GUARD(s2n_stuffer_reserve_uint16(out, &total_share_size));

    struct s2n_ecc_evp_params *ecc_params = &kem_group_params->ecc_params;
    ecc_params->negotiated_curve = kem_group->curve;

    struct s2n_kem_params *kem_params = &kem_group_params->kem_params;
    kem_params->kem = kem_group->kem;

    if (kem_group->send_kem_first) {
        POSIX_GUARD(s2n_kem_send_public_key(out, kem_params));
        POSIX_GUARD_RESULT(s2n_ecdhe_send_public_key(ecc_params, out, kem_params->len_prefixed));
    } else {
        POSIX_GUARD_RESULT(s2n_ecdhe_send_public_key(ecc_params, out, kem_params->len_prefixed));
        POSIX_GUARD(s2n_kem_send_public_key(out, kem_params));
    }

    POSIX_GUARD(s2n_stuffer_write_vector_size(&total_share_size));

    return S2N_SUCCESS;
}

static int s2n_generate_default_pq_hybrid_key_share(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(out);

    /* Client should skip sending PQ groups/key shares if PQ is disabled */
    if (!s2n_pq_is_enabled()) {
        return S2N_SUCCESS;
    }

    const struct s2n_kem_preferences *kem_pref = NULL;
    POSIX_GUARD(s2n_connection_get_kem_preferences(conn, &kem_pref));
    POSIX_ENSURE_REF(kem_pref);

    uint32_t available_groups = 0;
    POSIX_GUARD_RESULT(s2n_kem_preferences_groups_available(kem_pref, &available_groups));
    if (available_groups == 0) {
        return S2N_SUCCESS;
    }

    /* We only ever send a single PQ key share: either the share requested by the server
     * during a retry, or the most preferred share according to local preferences.
     */
    struct s2n_kem_group_params *client_params = &conn->kex_params.client_kem_group_params;

    if (s2n_is_hello_retry_handshake(conn)) {
        const struct s2n_kem_group *server_group = conn->kex_params.server_kem_group_params.kem_group;

        /* If the server did not request a specific PQ keyshare, don't send one */
        if (!server_group) {
            return S2N_SUCCESS;
        }

        /* If the server requested a new PQ keyshare, free the old one */
        if (client_params->kem_group != server_group) {
            POSIX_GUARD(s2n_kem_group_free(client_params));
        }

        /**
         *= https://www.rfc-editor.org/rfc/rfc8446#4.2.8
         *# Otherwise, when sending the new ClientHello, the client MUST
         *# replace the original "key_share" extension with one containing only a
         *# new KeyShareEntry for the group indicated in the selected_group field
         *# of the triggering HelloRetryRequest.
         **/
        client_params->kem_group = server_group;
    } else {
        client_params->kem_group = s2n_kem_preferences_get_highest_priority_group(kem_pref);
        POSIX_ENSURE_REF(client_params->kem_group);
        client_params->kem_params.len_prefixed = s2n_tls13_client_must_use_hybrid_kem_length_prefix(kem_pref);
    }

    POSIX_GUARD(s2n_generate_pq_hybrid_key_share(out, client_params));

    return S2N_SUCCESS;
}

static int s2n_client_key_share_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    if (s2n_is_hello_retry_handshake(conn)) {
        const struct s2n_ecc_named_curve *server_curve = conn->kex_params.server_ecc_evp_params.negotiated_curve;
        const struct s2n_ecc_named_curve *client_curve = conn->kex_params.client_ecc_evp_params.negotiated_curve;
        const struct s2n_kem_group *server_group = conn->kex_params.server_kem_group_params.kem_group;
        const struct s2n_kem_group *client_group = conn->kex_params.client_kem_group_params.kem_group;

        /* Ensure a new key share will be sent after a hello retry request */
        POSIX_ENSURE(server_curve != client_curve || server_group != client_group, S2N_ERR_BAD_KEY_SHARE);
    }

    struct s2n_stuffer_reservation shares_size = { 0 };
    POSIX_GUARD(s2n_stuffer_reserve_uint16(out, &shares_size));
    POSIX_GUARD(s2n_generate_default_pq_hybrid_key_share(conn, out));
    POSIX_GUARD(s2n_generate_default_ecc_key_share(conn, out));
    POSIX_GUARD(s2n_stuffer_write_vector_size(&shares_size));

    /* We must have written at least one share */
    POSIX_ENSURE(s2n_stuffer_data_available(out) > shares_size.length, S2N_ERR_BAD_KEY_SHARE);

    return S2N_SUCCESS;
}

static int s2n_client_key_share_parse_ecc(struct s2n_stuffer *key_share, const struct s2n_ecc_named_curve *curve,
        struct s2n_ecc_evp_params *ecc_params)
{
    POSIX_ENSURE_REF(key_share);
    POSIX_ENSURE_REF(curve);
    POSIX_ENSURE_REF(ecc_params);

    struct s2n_blob point_blob = { 0 };
    POSIX_GUARD(s2n_ecc_evp_read_params_point(key_share, curve->share_size, &point_blob));

    /* Ignore curves with points we can't parse */
    ecc_params->negotiated_curve = curve;
    if (s2n_ecc_evp_parse_params_point(&point_blob, ecc_params) != S2N_SUCCESS) {
        ecc_params->negotiated_curve = NULL;
        POSIX_GUARD(s2n_ecc_evp_params_free(ecc_params));
    }

    return S2N_SUCCESS;
}

static int s2n_client_key_share_recv_ecc(struct s2n_connection *conn, struct s2n_stuffer *key_share, uint16_t curve_iana_id)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(key_share);

    const struct s2n_ecc_preferences *ecc_pref = NULL;
    POSIX_GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_pref));
    POSIX_ENSURE_REF(ecc_pref);

    struct s2n_ecc_evp_params *client_params = &conn->kex_params.client_ecc_evp_params;

    const struct s2n_ecc_named_curve *curve = NULL;
    for (size_t i = 0; i < ecc_pref->count; i++) {
        const struct s2n_ecc_named_curve *supported_curve = ecc_pref->ecc_curves[i];
        POSIX_ENSURE_REF(supported_curve);

        /* Stop if we reach the current highest priority share.
         * Any share of lower priority is discarded.
         */
        if (client_params->negotiated_curve == supported_curve) {
            break;
        }

        /* Skip if not supported by the client.
         * The client must not send shares it doesn't support, but the server
         * is not required to error if they are encountered.
         */
        if (!conn->kex_params.mutually_supported_curves[i]) {
            continue;
        }

        /* Stop if we find a match */
        if (curve_iana_id == supported_curve->iana_id) {
            curve = supported_curve;
            break;
        }
    }

    /* Ignore unsupported curves */
    if (!curve) {
        return S2N_SUCCESS;
    }

    /* Ignore curves with unexpected share sizes */
    if (key_share->blob.size != curve->share_size) {
        return S2N_SUCCESS;
    }

    DEFER_CLEANUP(struct s2n_ecc_evp_params new_client_params = { 0 }, s2n_ecc_evp_params_free);

    POSIX_GUARD(s2n_client_key_share_parse_ecc(key_share, curve, &new_client_params));
    /* negotiated_curve will be NULL if the key share was not parsed successfully */
    if (!new_client_params.negotiated_curve) {
        return S2N_SUCCESS;
    }

    POSIX_GUARD(s2n_ecc_evp_params_free(client_params));
    *client_params = new_client_params;

    ZERO_TO_DISABLE_DEFER_CLEANUP(new_client_params);
    return S2N_SUCCESS;
}

static int s2n_client_key_share_recv_hybrid_partial_ecc(struct s2n_stuffer *key_share, struct s2n_kem_group_params *new_client_params)
{
    POSIX_ENSURE_REF(new_client_params);
    const struct s2n_kem_group *kem_group = new_client_params->kem_group;
    POSIX_ENSURE_REF(kem_group);
    POSIX_ENSURE_REF(kem_group->curve);

    if (new_client_params->kem_params.len_prefixed) {
        uint16_t ec_share_size = 0;
        POSIX_GUARD(s2n_stuffer_read_uint16(key_share, &ec_share_size));
        POSIX_ENSURE(ec_share_size == kem_group->curve->share_size, S2N_ERR_SIZE_MISMATCH);
    }

    POSIX_GUARD(s2n_client_key_share_parse_ecc(key_share, kem_group->curve, &new_client_params->ecc_params));

    /* If we were unable to parse the EC portion of the share, negotiated_curve
     * will be NULL, and we should ignore the entire key share. */
    POSIX_ENSURE_REF(new_client_params->ecc_params.negotiated_curve);

    return S2N_SUCCESS;
}

static int s2n_client_key_share_recv_pq_hybrid(struct s2n_connection *conn, struct s2n_stuffer *key_share, uint16_t kem_group_iana_id)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(key_share);

    const struct s2n_kem_preferences *kem_pref = NULL;
    POSIX_GUARD(s2n_connection_get_kem_preferences(conn, &kem_pref));
    POSIX_ENSURE_REF(kem_pref);

    /* Ignore key share if PQ is not enabled */
    if (!s2n_pq_is_enabled()) {
        return S2N_SUCCESS;
    }

    struct s2n_kem_group_params *client_params = &conn->kex_params.client_kem_group_params;

    const struct s2n_kem_group *kem_group = NULL;
    for (size_t i = 0; i < kem_pref->tls13_kem_group_count; i++) {
        const struct s2n_kem_group *supported_group = kem_pref->tls13_kem_groups[i];
        POSIX_ENSURE_REF(supported_group);

        /* Skip if the group is not available */
        if (!s2n_kem_group_is_available(supported_group)) {
            continue;
        }

        /* Stop if we reach the current highest priority share.
         * Any share of lower priority is discarded.
         */
        if (client_params->kem_group == supported_group) {
            break;
        }

        /* Skip if not supported by the client.
         * The client must not send shares it doesn't support, but the server
         * is not required to error if they are encountered.
         */
        if (!conn->kex_params.mutually_supported_kem_groups[i]) {
            continue;
        }

        /* Stop if we find a match */
        if (kem_group_iana_id == supported_group->iana_id) {
            kem_group = supported_group;
            break;
        }
    }

    /* Ignore unsupported KEM groups */
    if (!kem_group) {
        return S2N_SUCCESS;
    }

    /* The length of the hybrid key share must be one of two possible lengths. Its internal values are either length
     * prefixed, or they are not. */
    uint16_t actual_hybrid_share_size = key_share->blob.size;
    uint16_t unprefixed_hybrid_share_size = kem_group->curve->share_size + kem_group->kem->public_key_length;
    uint16_t prefixed_hybrid_share_size = (2 * S2N_SIZE_OF_KEY_SHARE_SIZE) + unprefixed_hybrid_share_size;

    /* Ignore KEM groups with unexpected overall total share sizes */
    if ((actual_hybrid_share_size != unprefixed_hybrid_share_size) && (actual_hybrid_share_size != prefixed_hybrid_share_size)) {
        return S2N_SUCCESS;
    }

    bool is_hybrid_share_length_prefixed = (actual_hybrid_share_size == prefixed_hybrid_share_size);

    DEFER_CLEANUP(struct s2n_kem_group_params new_client_params = { 0 }, s2n_kem_group_free);
    new_client_params.kem_group = kem_group;

    /* Need to save whether the client included the length prefix so that we can match their behavior in our response. */
    new_client_params.kem_params.len_prefixed = is_hybrid_share_length_prefixed;
    new_client_params.kem_params.kem = kem_group->kem;

    /* Note: the PQ share size is validated in s2n_kem_recv_public_key() */
    /* Ignore PQ and ECC groups with public keys we can't parse */
    if (kem_group->send_kem_first) {
        if (s2n_kem_recv_public_key(key_share, &new_client_params.kem_params) != S2N_SUCCESS) {
            return S2N_SUCCESS;
        }
        if (s2n_client_key_share_recv_hybrid_partial_ecc(key_share, &new_client_params) != S2N_SUCCESS) {
            return S2N_SUCCESS;
        }
    } else {
        if (s2n_client_key_share_recv_hybrid_partial_ecc(key_share, &new_client_params) != S2N_SUCCESS) {
            return S2N_SUCCESS;
        }
        if (s2n_kem_recv_public_key(key_share, &new_client_params.kem_params) != S2N_SUCCESS) {
            return S2N_SUCCESS;
        }
    }

    POSIX_GUARD(s2n_kem_group_free(client_params));
    *client_params = new_client_params;

    ZERO_TO_DISABLE_DEFER_CLEANUP(new_client_params);
    return S2N_SUCCESS;
}

/*
 * We chose our most preferred group of the mutually supported groups while processing the
 * supported_groups extension. However, our true most preferred group is always the
 * group that we already have a key share for, since retries are expensive.
 *
 * This method modifies our group selection based on what keyshares are available.
 * It then stores the client keyshare for the selected group, or initiates a retry
 * if no valid keyshares are available.
 */
static int s2n_client_key_share_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(extension);

    uint16_t key_shares_size = 0;
    POSIX_GUARD(s2n_stuffer_read_uint16(extension, &key_shares_size));
    POSIX_ENSURE(s2n_stuffer_data_available(extension) == key_shares_size, S2N_ERR_BAD_MESSAGE);

    uint16_t named_group = 0, share_size = 0;
    struct s2n_blob key_share_blob = { 0 };
    struct s2n_stuffer key_share = { 0 };

    uint16_t keyshare_count = 0;
    while (s2n_stuffer_data_available(extension) > 0) {
        POSIX_GUARD(s2n_stuffer_read_uint16(extension, &named_group));
        POSIX_GUARD(s2n_stuffer_read_uint16(extension, &share_size));
        POSIX_ENSURE(s2n_stuffer_data_available(extension) >= share_size, S2N_ERR_BAD_MESSAGE);

        POSIX_GUARD(s2n_blob_init(&key_share_blob,
                s2n_stuffer_raw_read(extension, share_size), share_size));
        POSIX_GUARD(s2n_stuffer_init(&key_share, &key_share_blob));
        POSIX_GUARD(s2n_stuffer_skip_write(&key_share, share_size));
        keyshare_count++;

        /* Try to parse the share as ECC, then as PQ/hybrid; will ignore
         * shares for unrecognized groups. */
        POSIX_GUARD(s2n_client_key_share_recv_ecc(conn, &key_share, named_group));
        POSIX_GUARD(s2n_client_key_share_recv_pq_hybrid(conn, &key_share, named_group));
    }

    /* During a retry, the client should only have sent one keyshare */
    POSIX_ENSURE(!s2n_is_hello_retry_handshake(conn) || keyshare_count == 1, S2N_ERR_BAD_MESSAGE);

    /**
     * If there were no matching key shares, then we received an empty key share extension
     * or we didn't match a key share with a supported group. We should send a retry.
     *
     *= https://www.rfc-editor.org/rfc/rfc8446#4.1.1
     *# If the server selects an (EC)DHE group and the client did not offer a
     *# compatible "key_share" extension in the initial ClientHello, the
     *# server MUST respond with a HelloRetryRequest (Section 4.1.4) message.
     **/
    struct s2n_ecc_evp_params *client_ecc_params = &conn->kex_params.client_ecc_evp_params;
    struct s2n_kem_group_params *client_pq_params = &conn->kex_params.client_kem_group_params;
    if (!client_pq_params->kem_group && !client_ecc_params->negotiated_curve) {
        POSIX_GUARD(s2n_set_hello_retry_required(conn));
    }

    return S2N_SUCCESS;
}

/* Old-style extension functions -- remove after extensions refactor is complete */

int s2n_extensions_client_key_share_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    return s2n_extension_recv(&s2n_client_key_share_extension, conn, extension);
}
