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

#include "tls/extensions/s2n_server_key_share.h"

#include "crypto/s2n_pq.h"
#include "tls/s2n_security_policies.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls13.h"
#include "utils/s2n_safety.h"

static int s2n_server_key_share_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_server_key_share_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_server_key_share_extension = {
    .iana_value = TLS_EXTENSION_KEY_SHARE,
    .minimum_version = S2N_TLS13,
    .is_response = true,
    .send = s2n_server_key_share_send,
    .recv = s2n_server_key_share_recv,
    .should_send = s2n_extension_always_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static int s2n_server_key_share_send_hybrid_partial_ecc(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(out);

    struct s2n_kem_group_params *server_kem_group_params = &conn->kex_params.server_kem_group_params;
    struct s2n_kem_params *client_kem_params = &conn->kex_params.client_kem_group_params.kem_params;

    struct s2n_ecc_evp_params *server_ecc_params = &server_kem_group_params->ecc_params;
    POSIX_ENSURE_REF(server_ecc_params->negotiated_curve);
    if (client_kem_params->len_prefixed) {
        POSIX_GUARD(s2n_stuffer_write_uint16(out, server_ecc_params->negotiated_curve->share_size));
    }
    POSIX_GUARD(s2n_ecc_evp_generate_ephemeral_key(server_ecc_params));
    POSIX_GUARD(s2n_ecc_evp_write_params_point(server_ecc_params, out));

    return S2N_SUCCESS;
}

static int s2n_server_key_share_generate_pq_hybrid(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(out);
    POSIX_ENSURE_REF(conn);

    POSIX_ENSURE(s2n_pq_is_enabled(), S2N_ERR_UNIMPLEMENTED);

    struct s2n_kem_group_params *server_kem_group_params = &conn->kex_params.server_kem_group_params;
    struct s2n_kem_params *client_kem_params = &conn->kex_params.client_kem_group_params.kem_params;
    POSIX_ENSURE_REF(client_kem_params->public_key.data);

    POSIX_ENSURE_REF(server_kem_group_params->kem_group);
    POSIX_GUARD(s2n_stuffer_write_uint16(out, server_kem_group_params->kem_group->iana_id));

    struct s2n_stuffer_reservation total_share_size = { 0 };
    POSIX_GUARD(s2n_stuffer_reserve_uint16(out, &total_share_size));

    /* s2n_kem_send_ciphertext() will generate the PQ shared secret and use
     * the client's public key to encapsulate; the PQ shared secret will be
     * stored in client_kem_params, and will be used during the hybrid shared
     * secret derivation. */
    if (server_kem_group_params->kem_group->send_kem_first) {
        POSIX_GUARD(s2n_kem_send_ciphertext(out, client_kem_params));
        POSIX_GUARD(s2n_server_key_share_send_hybrid_partial_ecc(conn, out));
    } else {
        POSIX_GUARD(s2n_server_key_share_send_hybrid_partial_ecc(conn, out));
        POSIX_GUARD(s2n_kem_send_ciphertext(out, client_kem_params));
    }

    POSIX_GUARD(s2n_stuffer_write_vector_size(&total_share_size));
    return S2N_SUCCESS;
}

/* Check that client has sent a corresponding key share for the server's KEM group */
int s2n_server_key_share_send_check_pq_hybrid(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    POSIX_ENSURE(s2n_pq_is_enabled(), S2N_ERR_UNIMPLEMENTED);

    POSIX_ENSURE_REF(conn->kex_params.server_kem_group_params.kem_group);
    POSIX_ENSURE_REF(conn->kex_params.server_kem_group_params.kem_params.kem);
    POSIX_ENSURE_REF(conn->kex_params.server_kem_group_params.ecc_params.negotiated_curve);

    const struct s2n_kem_group *server_kem_group = conn->kex_params.server_kem_group_params.kem_group;

    const struct s2n_kem_preferences *kem_pref = NULL;
    POSIX_GUARD(s2n_connection_get_kem_preferences(conn, &kem_pref));
    POSIX_ENSURE_REF(kem_pref);

    POSIX_ENSURE(s2n_kem_preferences_includes_tls13_kem_group(kem_pref, server_kem_group->iana_id),
            S2N_ERR_KEM_UNSUPPORTED_PARAMS);

    struct s2n_kem_group_params *client_params = &conn->kex_params.client_kem_group_params;
    POSIX_ENSURE(client_params->kem_group == server_kem_group, S2N_ERR_BAD_KEY_SHARE);

    POSIX_ENSURE(client_params->ecc_params.negotiated_curve == server_kem_group->curve, S2N_ERR_BAD_KEY_SHARE);
    POSIX_ENSURE(client_params->ecc_params.evp_pkey != NULL, S2N_ERR_BAD_KEY_SHARE);

    POSIX_ENSURE(client_params->kem_params.kem == server_kem_group->kem, S2N_ERR_BAD_KEY_SHARE);
    POSIX_ENSURE(client_params->kem_params.public_key.size == server_kem_group->kem->public_key_length, S2N_ERR_BAD_KEY_SHARE);
    POSIX_ENSURE(client_params->kem_params.public_key.data != NULL, S2N_ERR_BAD_KEY_SHARE);

    return S2N_SUCCESS;
}

/* Check that client has sent a corresponding key share for the server's EC curve */
int s2n_server_key_share_send_check_ecdhe(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    const struct s2n_ecc_preferences *ecc_pref = NULL;
    POSIX_GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_pref));
    POSIX_ENSURE_REF(ecc_pref);

    const struct s2n_ecc_named_curve *server_curve = conn->kex_params.server_ecc_evp_params.negotiated_curve;
    POSIX_ENSURE_REF(server_curve);

    struct s2n_ecc_evp_params *client_params = &conn->kex_params.client_ecc_evp_params;
    POSIX_ENSURE(client_params->negotiated_curve == server_curve, S2N_ERR_BAD_KEY_SHARE);
    POSIX_ENSURE(client_params->evp_pkey != NULL, S2N_ERR_BAD_KEY_SHARE);

    return S2N_SUCCESS;
}

static int s2n_server_key_share_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(out);

    const struct s2n_ecc_named_curve *curve = conn->kex_params.server_ecc_evp_params.negotiated_curve;
    const struct s2n_kem_group *kem_group = conn->kex_params.server_kem_group_params.kem_group;

    /* Boolean XOR: exactly one of {server_curve, server_kem_group} should be non-null. */
    POSIX_ENSURE((curve == NULL) != (kem_group == NULL), S2N_ERR_ECDHE_UNSUPPORTED_CURVE);

    /* Retry requests only require the selected named group, not an actual share.
     * https://tools.ietf.org/html/rfc8446#section-4.2.8 */
    if (s2n_is_hello_retry_message(conn)) {
        uint16_t named_group_id = 0;
        if (curve != NULL) {
            named_group_id = curve->iana_id;
        } else {
            named_group_id = kem_group->iana_id;
        }

        POSIX_GUARD(s2n_stuffer_write_uint16(out, named_group_id));
        return S2N_SUCCESS;
    }

    if (curve != NULL) {
        POSIX_GUARD(s2n_server_key_share_send_check_ecdhe(conn));
        POSIX_GUARD(s2n_ecdhe_parameters_send(&conn->kex_params.server_ecc_evp_params, out));
    } else {
        POSIX_GUARD(s2n_server_key_share_send_check_pq_hybrid(conn));
        POSIX_GUARD(s2n_server_key_share_generate_pq_hybrid(conn, out));
    }

    return S2N_SUCCESS;
}

static int s2n_server_key_share_recv_hybrid_partial_ecc(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(extension);

    struct s2n_kem_params *client_kem_params = &conn->kex_params.client_kem_group_params.kem_params;
    struct s2n_kem_group_params *server_kem_group_params = &conn->kex_params.server_kem_group_params;
    const struct s2n_kem_group *server_kem_group = server_kem_group_params->kem_group;
    POSIX_ENSURE_REF(server_kem_group);
    uint16_t expected_ecc_share_size = server_kem_group->curve->share_size;

    /* Parse ECC key share */
    if (client_kem_params->len_prefixed) {
        uint16_t actual_ecc_share_size = 0;
        POSIX_GUARD(s2n_stuffer_read_uint16(extension, &actual_ecc_share_size));
        POSIX_ENSURE(actual_ecc_share_size == expected_ecc_share_size, S2N_ERR_BAD_KEY_SHARE);
    }

    struct s2n_blob point_blob = { 0 };
    POSIX_ENSURE(s2n_ecc_evp_read_params_point(extension, expected_ecc_share_size, &point_blob) == S2N_SUCCESS, S2N_ERR_BAD_KEY_SHARE);
    POSIX_ENSURE(s2n_ecc_evp_parse_params_point(&point_blob, &server_kem_group_params->ecc_params) == S2N_SUCCESS, S2N_ERR_BAD_KEY_SHARE);
    POSIX_ENSURE(server_kem_group_params->ecc_params.evp_pkey != NULL, S2N_ERR_BAD_KEY_SHARE);

    return S2N_SUCCESS;
}

static int s2n_server_key_share_recv_pq_hybrid(struct s2n_connection *conn, uint16_t named_group_iana,
        struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(extension);

    /* If PQ is disabled, the client should not have sent any PQ IDs
     * in the supported_groups list of the initial ClientHello */
    POSIX_ENSURE(s2n_pq_is_enabled(), S2N_ERR_ECDHE_UNSUPPORTED_CURVE);

    const struct s2n_kem_preferences *kem_pref = NULL;
    POSIX_GUARD(s2n_connection_get_kem_preferences(conn, &kem_pref));
    POSIX_ENSURE_REF(kem_pref);

    /* This check should have been done higher up, but including it here as well for extra defense.
     * Uses S2N_ERR_ECDHE_UNSUPPORTED_CURVE for backward compatibility. */
    POSIX_ENSURE(s2n_kem_preferences_includes_tls13_kem_group(kem_pref, named_group_iana), S2N_ERR_ECDHE_UNSUPPORTED_CURVE);

    size_t kem_group_index = 0;
    for (size_t i = 0; i < kem_pref->tls13_kem_group_count; i++) {
        if (named_group_iana == kem_pref->tls13_kem_groups[i]->iana_id
                && s2n_kem_group_is_available(kem_pref->tls13_kem_groups[i])) {
            kem_group_index = i;
            break;
        }
    }

    struct s2n_kem_group_params *server_kem_group_params = &conn->kex_params.server_kem_group_params;
    server_kem_group_params->kem_group = kem_pref->tls13_kem_groups[kem_group_index];
    server_kem_group_params->kem_params.kem = kem_pref->tls13_kem_groups[kem_group_index]->kem;
    server_kem_group_params->ecc_params.negotiated_curve = kem_pref->tls13_kem_groups[kem_group_index]->curve;

    /* If this a HRR, the server will only have sent the named group ID. We assign the
     * appropriate KEM group params above, then exit early so that the client can
     * generate the correct key share. */
    if (s2n_is_hello_retry_message(conn)) {
        return S2N_SUCCESS;
    }

    /* Ensure that the server's key share corresponds with a key share previously sent by the client */
    struct s2n_kem_group_params *client_kem_group_params = &conn->kex_params.client_kem_group_params;
    POSIX_ENSURE(client_kem_group_params->kem_params.private_key.data, S2N_ERR_BAD_KEY_SHARE);
    POSIX_ENSURE(client_kem_group_params->ecc_params.evp_pkey, S2N_ERR_BAD_KEY_SHARE);
    POSIX_ENSURE(client_kem_group_params->kem_group == server_kem_group_params->kem_group, S2N_ERR_BAD_KEY_SHARE);

    uint16_t actual_hybrid_share_size = 0;
    POSIX_GUARD(s2n_stuffer_read_uint16(extension, &actual_hybrid_share_size));
    POSIX_ENSURE(s2n_stuffer_data_available(extension) == actual_hybrid_share_size, S2N_ERR_BAD_KEY_SHARE);

    struct s2n_kem_params *client_kem_params = &conn->kex_params.client_kem_group_params.kem_params;

    /* Don't need to call s2n_is_tls13_hybrid_kem_length_prefixed() to set client_kem_params->len_prefixed since we are
     * the client, and server-side should auto-detect hybrid share size and match our behavior. */

    if (!server_kem_group_params->kem_group->send_kem_first) {
        POSIX_ENSURE(s2n_server_key_share_recv_hybrid_partial_ecc(conn, extension) == S2N_SUCCESS, S2N_ERR_BAD_KEY_SHARE);
        POSIX_ENSURE(s2n_kem_recv_ciphertext(extension, client_kem_params) == S2N_SUCCESS, S2N_ERR_BAD_KEY_SHARE);
    } else {
        POSIX_ENSURE(s2n_kem_recv_ciphertext(extension, client_kem_params) == S2N_SUCCESS, S2N_ERR_BAD_KEY_SHARE);
        POSIX_ENSURE(s2n_server_key_share_recv_hybrid_partial_ecc(conn, extension) == S2N_SUCCESS, S2N_ERR_BAD_KEY_SHARE);
    }

    return S2N_SUCCESS;
}

static int s2n_server_key_share_recv_ecc(struct s2n_connection *conn, uint16_t named_group_iana,
        struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(extension);

    const struct s2n_ecc_preferences *ecc_pref = NULL;
    POSIX_GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_pref));
    POSIX_ENSURE_REF(ecc_pref);

    /* This check should have been done higher up, but including it here as well for extra defense. */
    POSIX_ENSURE(s2n_ecc_preferences_includes_curve(ecc_pref, named_group_iana),
            S2N_ERR_ECDHE_UNSUPPORTED_CURVE);

    size_t supported_curve_index = 0;

    for (size_t i = 0; i < ecc_pref->count; i++) {
        if (named_group_iana == ecc_pref->ecc_curves[i]->iana_id) {
            supported_curve_index = i;
            break;
        }
    }

    struct s2n_ecc_evp_params *server_ecc_evp_params = &conn->kex_params.server_ecc_evp_params;
    const struct s2n_ecc_named_curve *negotiated_curve = ecc_pref->ecc_curves[supported_curve_index];

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#4.2.8
     *# If using (EC)DHE key establishment and a HelloRetryRequest containing a
     *# "key_share" extension was received by the client, the client MUST
     *# verify that the selected NamedGroup in the ServerHello is the same as
     *# that in the HelloRetryRequest. If this check fails, the client MUST
     *# abort the handshake with an "illegal_parameter" alert.
     **/
    if (s2n_is_hello_retry_handshake(conn) && !s2n_is_hello_retry_message(conn)) {
        POSIX_ENSURE_REF(server_ecc_evp_params->negotiated_curve);
        const struct s2n_ecc_named_curve *previous_negotiated_curve = server_ecc_evp_params->negotiated_curve;
        POSIX_ENSURE(negotiated_curve == previous_negotiated_curve,
                S2N_ERR_BAD_MESSAGE);
    }

    server_ecc_evp_params->negotiated_curve = negotiated_curve;

    /* Now that ECC has been negotiated, null out this connection's preferred Hybrid KEMs. They will not be used any
     * more during this TLS connection, but can still be printed by s2nc's client debugging output. */
    conn->kex_params.client_kem_group_params.kem_group = NULL;
    conn->kex_params.server_kem_group_params.ecc_params.negotiated_curve = NULL;
    conn->kex_params.client_kem_group_params.kem_params.kem = NULL;

    /* If this is a HelloRetryRequest, we won't have a key share. We just have the selected group.
     * Set the server negotiated curve and exit early so a proper keyshare can be generated. */
    if (s2n_is_hello_retry_message(conn)) {
        return S2N_SUCCESS;
    }

    /* Verify key share sent by client */
    struct s2n_ecc_evp_params *client_ecc_evp_params = &conn->kex_params.client_ecc_evp_params;
    POSIX_ENSURE(client_ecc_evp_params->negotiated_curve == server_ecc_evp_params->negotiated_curve, S2N_ERR_BAD_KEY_SHARE);
    POSIX_ENSURE(client_ecc_evp_params->evp_pkey, S2N_ERR_BAD_KEY_SHARE);

    uint16_t share_size = 0;
    S2N_ERROR_IF(s2n_stuffer_data_available(extension) < sizeof(share_size), S2N_ERR_BAD_KEY_SHARE);
    POSIX_GUARD(s2n_stuffer_read_uint16(extension, &share_size));
    S2N_ERROR_IF(s2n_stuffer_data_available(extension) < share_size, S2N_ERR_BAD_KEY_SHARE);

    /* Proceed to parse share */
    struct s2n_blob point_blob = { 0 };
    S2N_ERROR_IF(s2n_ecc_evp_read_params_point(extension, share_size, &point_blob) < 0, S2N_ERR_BAD_KEY_SHARE);
    S2N_ERROR_IF(s2n_ecc_evp_parse_params_point(&point_blob, server_ecc_evp_params) < 0, S2N_ERR_BAD_KEY_SHARE);
    S2N_ERROR_IF(server_ecc_evp_params->evp_pkey == NULL, S2N_ERR_BAD_KEY_SHARE);

    return S2N_SUCCESS;
}

/*
 * From https://tools.ietf.org/html/rfc8446#section-4.2.8
 *
 * If using (EC)DHE key establishment, servers offer exactly one
 * KeyShareEntry in the ServerHello.  This value MUST be in the same
 * group as the KeyShareEntry value offered by the client that the
 * server has selected for the negotiated key exchange.
 */
static int s2n_server_key_share_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(extension);

    uint16_t negotiated_named_group_iana = 0;
    S2N_ERROR_IF(s2n_stuffer_data_available(extension) < sizeof(negotiated_named_group_iana), S2N_ERR_BAD_KEY_SHARE);
    POSIX_GUARD(s2n_stuffer_read_uint16(extension, &negotiated_named_group_iana));

    const struct s2n_kem_preferences *kem_pref = NULL;
    POSIX_GUARD(s2n_connection_get_kem_preferences(conn, &kem_pref));
    POSIX_ENSURE_REF(kem_pref);

    const struct s2n_ecc_preferences *ecc_pref = NULL;
    POSIX_GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_pref));
    POSIX_ENSURE_REF(ecc_pref);

    if (s2n_ecc_preferences_includes_curve(ecc_pref, negotiated_named_group_iana)) {
        POSIX_GUARD(s2n_server_key_share_recv_ecc(conn, negotiated_named_group_iana, extension));
    } else if (s2n_kem_preferences_includes_tls13_kem_group(kem_pref, negotiated_named_group_iana)) {
        POSIX_GUARD(s2n_server_key_share_recv_pq_hybrid(conn, negotiated_named_group_iana, extension));
    } else {
        POSIX_BAIL(S2N_ERR_ECDHE_UNSUPPORTED_CURVE);
    }

    return S2N_SUCCESS;
}

/* Selects highest priority mutually supported key share, or indicates need for HRR */
int s2n_extensions_server_key_share_select(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    /* Get the client's preferred groups for the KeyShares that were actually sent by the client */
    const struct s2n_ecc_named_curve *client_curve = conn->kex_params.client_ecc_evp_params.negotiated_curve;
    const struct s2n_kem_group *client_kem_group = conn->kex_params.client_kem_group_params.kem_group;

    /* Get the server's preferred groups (which may or may not have been sent in the KeyShare by the client) */
    const struct s2n_ecc_named_curve *server_curve = conn->kex_params.server_ecc_evp_params.negotiated_curve;
    const struct s2n_kem_group *server_kem_group = conn->kex_params.server_kem_group_params.kem_group;

    /* Boolean XOR check. When receiving the supported_groups extension, s2n server
     * should (exclusively) set either server_curve or server_kem_group based on the
     * set of mutually supported groups. If both server_curve and server_kem_group
     * are NULL, it is because client and server do not share any mutually supported
     * groups; key negotiation is not possible and the handshake should be aborted
     * without sending HRR. (The case of both being non-NULL should never occur, and
     * is an error.) */
    POSIX_ENSURE((server_curve == NULL) != (server_kem_group == NULL), S2N_ERR_ECDHE_UNSUPPORTED_CURVE);

    /* To avoid extra round trips, we prefer to negotiate a group for which we have already
     * received a key share (even if it is different than the group previously chosen). In
     * general, we prefer to negotiate PQ over ECDHE; however, if both client and server
     * support PQ, but the client sent only EC key shares, then we will negotiate ECHDE. */

    /* Option 1: Select the best mutually supported PQ Hybrid Group that can be negotiated in 1-RTT */
    if (client_kem_group != NULL) {
        POSIX_ENSURE_REF(conn->kex_params.client_kem_group_params.ecc_params.negotiated_curve);
        POSIX_ENSURE_REF(conn->kex_params.client_kem_group_params.kem_params.kem);

        conn->kex_params.server_kem_group_params.kem_group = conn->kex_params.client_kem_group_params.kem_group;
        conn->kex_params.server_kem_group_params.ecc_params.negotiated_curve = conn->kex_params.client_kem_group_params.ecc_params.negotiated_curve;
        conn->kex_params.server_kem_group_params.kem_params.kem = conn->kex_params.client_kem_group_params.kem_params.kem;
        conn->kex_params.server_ecc_evp_params.negotiated_curve = NULL;
        return S2N_SUCCESS;
    }

    /* Option 2: Otherwise, if any PQ Hybrid Groups can be negotiated in 2-RTT's select that one. This ensures that
     * clients who offer PQ (and presumably therefore have concerns about quantum computing impacting the long term
     * confidentiality of their data), have their choice to offer PQ respected, even if they predict the server-side
     * supports a different PQ KeyShare algorithms. This ensures clients with PQ support are never downgraded to non-PQ
     * algorithms. */
    if (server_kem_group != NULL) {
        /* Null out any available ECC curves so that they won't be sent in the ClientHelloRetry */
        conn->kex_params.server_ecc_evp_params.negotiated_curve = NULL;
        POSIX_GUARD(s2n_set_hello_retry_required(conn));
        return S2N_SUCCESS;
    }

    /* Option 3: Otherwise, if there is a mutually supported classical ECDHE-only group can be negotiated in 1-RTT, select that one */
    if (client_curve) {
        conn->kex_params.server_ecc_evp_params.negotiated_curve = conn->kex_params.client_ecc_evp_params.negotiated_curve;
        conn->kex_params.server_kem_group_params.kem_group = NULL;
        conn->kex_params.server_kem_group_params.ecc_params.negotiated_curve = NULL;
        conn->kex_params.server_kem_group_params.kem_params.kem = NULL;
        return S2N_SUCCESS;
    }

    /* Option 4: Server and client have at least 1 mutually supported group, but the client did not send key shares for
     * any of them. Send a HelloRetryRequest indicating the server's preference. */
    POSIX_GUARD(s2n_set_hello_retry_required(conn));
    return S2N_SUCCESS;
}

/* Old-style extension functions -- remove after extensions refactor is complete */

/*
 * Calculate the data length for Server Key Share extension
 * based on negotiated_curve selected in server_ecc_evp_params.
 *
 * Retry requests have a different key share format,
 * https://tools.ietf.org/html/rfc8446#section-4.2.8
 *
 * This functions does not error, but s2n_extensions_server_key_share_send() would
 */
int s2n_extensions_server_key_share_send_size(struct s2n_connection *conn)
{
    const struct s2n_ecc_named_curve *curve = conn->kex_params.server_ecc_evp_params.negotiated_curve;
    int key_share_size = S2N_SIZE_OF_EXTENSION_TYPE
            + S2N_SIZE_OF_EXTENSION_DATA_SIZE
            + S2N_SIZE_OF_NAMED_GROUP;

    /* If this is a KeyShareHelloRetryRequest we don't include the share size */
    if (s2n_is_hello_retry_message(conn)) {
        return key_share_size;
    }

    if (curve == NULL) {
        return 0;
    }

    /* If this is a full KeyShareEntry, include the share size */
    key_share_size += (S2N_SIZE_OF_KEY_SHARE_SIZE + curve->share_size);

    return key_share_size;
}

/*
 * Sends Key Share extension in Server Hello.
 *
 * Expects negotiated_curve to be set and generates a ephemeral key for key sharing
 */
int s2n_extensions_server_key_share_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    return s2n_extension_send(&s2n_server_key_share_extension, conn, out);
}

/*
 * Client receives a Server Hello key share.
 *
 * If the curve is supported, conn->kex_params.server_ecc_evp_params will be set.
 */
int s2n_extensions_server_key_share_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    return s2n_extension_recv(&s2n_server_key_share_extension, conn, extension);
}
