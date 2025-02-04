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

#include "tls/extensions/s2n_client_supported_groups.h"

#include <stdint.h>
#include <sys/param.h>

#include "crypto/s2n_pq.h"
#include "tls/extensions/s2n_ec_point_format.h"
#include "tls/s2n_security_policies.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls13.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_safety.h"

static int s2n_client_supported_groups_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_client_supported_groups_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_client_supported_groups_extension = {
    .iana_value = TLS_EXTENSION_SUPPORTED_GROUPS,
    .is_response = false,
    .send = s2n_client_supported_groups_send,
    .recv = s2n_client_supported_groups_recv,
    .should_send = s2n_extension_should_send_if_ecc_enabled,
    .if_missing = s2n_extension_noop_if_missing,
};

bool s2n_extension_should_send_if_ecc_enabled(struct s2n_connection *conn)
{
    const struct s2n_security_policy *security_policy = NULL;
    return s2n_connection_get_security_policy(conn, &security_policy) == S2N_SUCCESS
            && s2n_ecc_is_extension_required(security_policy);
}

static int s2n_client_supported_groups_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);

    const struct s2n_ecc_preferences *ecc_pref = NULL;
    POSIX_GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_pref));
    POSIX_ENSURE_REF(ecc_pref);

    const struct s2n_kem_preferences *kem_pref = NULL;
    POSIX_GUARD(s2n_connection_get_kem_preferences(conn, &kem_pref));
    POSIX_ENSURE_REF(kem_pref);

    /* Group list len */
    struct s2n_stuffer_reservation group_list_len = { 0 };
    POSIX_GUARD(s2n_stuffer_reserve_uint16(out, &group_list_len));

    /* Send KEM groups list first */
    if (s2n_connection_get_protocol_version(conn) >= S2N_TLS13 && s2n_pq_is_enabled()) {
        for (size_t i = 0; i < kem_pref->tls13_kem_group_count; i++) {
            if (!s2n_kem_group_is_available(kem_pref->tls13_kem_groups[i])) {
                continue;
            }
            POSIX_GUARD(s2n_stuffer_write_uint16(out, kem_pref->tls13_kem_groups[i]->iana_id));
        }
    }

    /* Then send curve list */
    for (size_t i = 0; i < ecc_pref->count; i++) {
        POSIX_GUARD(s2n_stuffer_write_uint16(out, ecc_pref->ecc_curves[i]->iana_id));
    }

    POSIX_GUARD(s2n_stuffer_write_vector_size(&group_list_len));

    return S2N_SUCCESS;
}

S2N_RESULT s2n_supported_groups_parse_count(struct s2n_stuffer *extension, uint16_t *count)
{
    RESULT_ENSURE_REF(count);
    *count = 0;
    RESULT_ENSURE_REF(extension);

    uint16_t supported_groups_list_size = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(extension, &supported_groups_list_size));

    RESULT_ENSURE(supported_groups_list_size <= s2n_stuffer_data_available(extension),
            S2N_ERR_INVALID_PARSED_EXTENSIONS);
    RESULT_ENSURE(supported_groups_list_size % S2N_SUPPORTED_GROUP_SIZE == 0,
            S2N_ERR_INVALID_PARSED_EXTENSIONS);

    *count = supported_groups_list_size / S2N_SUPPORTED_GROUP_SIZE;

    return S2N_RESULT_OK;
}

/* Populates the appropriate index of either the mutually_supported_curves or
 * mutually_supported_kem_groups array based on the received IANA ID. Will
 * ignore unrecognized IANA IDs (and return success). */
static int s2n_client_supported_groups_recv_iana_id(struct s2n_connection *conn, uint16_t iana_id)
{
    POSIX_ENSURE_REF(conn);

    const struct s2n_ecc_preferences *ecc_pref = NULL;
    POSIX_GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_pref));
    POSIX_ENSURE_REF(ecc_pref);

    for (size_t i = 0; i < ecc_pref->count; i++) {
        const struct s2n_ecc_named_curve *supported_curve = ecc_pref->ecc_curves[i];
        if (iana_id == supported_curve->iana_id) {
            conn->kex_params.mutually_supported_curves[i] = supported_curve;
            return S2N_SUCCESS;
        }
    }

    /* Return early if PQ is disabled, or if TLS version is less than 1.3, so as to ignore PQ IDs */
    if (!s2n_pq_is_enabled() || s2n_connection_get_protocol_version(conn) < S2N_TLS13) {
        return S2N_SUCCESS;
    }

    const struct s2n_kem_preferences *kem_pref = NULL;
    POSIX_GUARD(s2n_connection_get_kem_preferences(conn, &kem_pref));
    POSIX_ENSURE_REF(kem_pref);

    for (size_t i = 0; i < kem_pref->tls13_kem_group_count; i++) {
        const struct s2n_kem_group *supported_kem_group = kem_pref->tls13_kem_groups[i];
        if (s2n_kem_group_is_available(supported_kem_group) && iana_id == supported_kem_group->iana_id) {
            conn->kex_params.mutually_supported_kem_groups[i] = supported_kem_group;
            return S2N_SUCCESS;
        }
    }

    return S2N_SUCCESS;
}

static int s2n_choose_supported_group(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    const struct s2n_ecc_preferences *ecc_pref = NULL;
    POSIX_GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_pref));
    POSIX_ENSURE_REF(ecc_pref);

    const struct s2n_kem_preferences *kem_pref = NULL;
    POSIX_GUARD(s2n_connection_get_kem_preferences(conn, &kem_pref));
    POSIX_ENSURE_REF(kem_pref);

    /* Ensure that only the intended group will be non-NULL (if no group is chosen, everything
     * should be NULL). */
    conn->kex_params.server_kem_group_params.kem_group = NULL;
    conn->kex_params.server_kem_group_params.ecc_params.negotiated_curve = NULL;
    conn->kex_params.server_kem_group_params.kem_params.kem = NULL;
    conn->kex_params.server_ecc_evp_params.negotiated_curve = NULL;

    /* Prefer to negotiate hybrid PQ over ECC. If PQ is disabled, we will never choose a
     * PQ group because the mutually_supported_kem_groups array will not have been
     * populated with anything. */
    for (size_t i = 0; i < kem_pref->tls13_kem_group_count; i++) {
        const struct s2n_kem_group *candidate_kem_group = conn->kex_params.mutually_supported_kem_groups[i];
        if (candidate_kem_group != NULL && s2n_kem_group_is_available(candidate_kem_group)) {
            conn->kex_params.server_kem_group_params.kem_group = candidate_kem_group;
            conn->kex_params.server_kem_group_params.ecc_params.negotiated_curve = candidate_kem_group->curve;
            conn->kex_params.server_kem_group_params.kem_params.kem = candidate_kem_group->kem;
            return S2N_SUCCESS;
        }
    }

    for (size_t i = 0; i < ecc_pref->count; i++) {
        const struct s2n_ecc_named_curve *candidate_curve = conn->kex_params.mutually_supported_curves[i];
        if (candidate_curve != NULL) {
            conn->kex_params.server_ecc_evp_params.negotiated_curve = candidate_curve;
            return S2N_SUCCESS;
        }
    }

    return S2N_SUCCESS;
}

static int s2n_client_supported_groups_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(extension);

    uint16_t supported_groups_count = 0;
    if (s2n_result_is_error(s2n_supported_groups_parse_count(extension, &supported_groups_count))) {
        /* Malformed length, ignore the extension */
        return S2N_SUCCESS;
    }

    for (size_t i = 0; i < supported_groups_count; i++) {
        uint16_t iana_id = 0;
        POSIX_GUARD(s2n_stuffer_read_uint16(extension, &iana_id));
        POSIX_GUARD(s2n_client_supported_groups_recv_iana_id(conn, iana_id));
    }

    POSIX_GUARD(s2n_choose_supported_group(conn));

    return S2N_SUCCESS;
}

/* Old-style extension functions -- remove after extensions refactor is complete */

int s2n_recv_client_supported_groups(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    return s2n_extension_recv(&s2n_client_supported_groups_extension, conn, extension);
}
