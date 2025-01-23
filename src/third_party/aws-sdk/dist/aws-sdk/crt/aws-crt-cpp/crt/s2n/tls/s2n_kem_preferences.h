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

#pragma once

#include <stdbool.h>

#include "tls/s2n_kem.h"
#include "tls/s2n_kex.h"

struct s2n_kem_preferences {
    /* kems used for hybrid TLS 1.2 */
    uint8_t kem_count;
    const struct s2n_kem **kems;

    /* tls13_kem_groups used for hybrid TLS 1.3 */
    const uint8_t tls13_kem_group_count;
    const struct s2n_kem_group **tls13_kem_groups;

    /* Which draft revision data format should the client use in its ClientHello. Currently the server will auto-detect
     * the format the client used from the TotalLength, and will match the client's behavior for backwards compatibility.
     *
     * Link: https://datatracker.ietf.org/doc/html/draft-ietf-tls-hybrid-design
     *  - Draft 0:   PQ Hybrid KEM format: (Total Length, PQ Length, PQ Share, ECC Length, ECC Share)
     *  - Draft 1-5: PQ Hybrid KEM format: (Total Length, PQ Share, ECC Share)
     */
    uint8_t tls13_pq_hybrid_draft_revision;
};

extern const struct s2n_kem *pq_kems_r3_2021_05[];

extern const struct s2n_kem_group *pq_kem_groups_r3_2021_05[];
extern const struct s2n_kem_group *pq_kem_groups_r3_2023_06[];

extern const struct s2n_kem_preferences kem_preferences_pq_tls_1_0_2021_05;
extern const struct s2n_kem_preferences kem_preferences_pq_tls_1_0_2023_01;
extern const struct s2n_kem_preferences kem_preferences_pq_tls_1_3_2023_06;
extern const struct s2n_kem_preferences kem_preferences_pq_tls_1_3_2023_12;
extern const struct s2n_kem_preferences kem_preferences_pq_tls_1_3_ietf_2024_10;
extern const struct s2n_kem_preferences kem_preferences_pq_tls_1_3_mixed_2024_10;
extern const struct s2n_kem_preferences kem_preferences_all;
extern const struct s2n_kem_preferences kem_preferences_null;

bool s2n_kem_preferences_includes_tls13_kem_group(const struct s2n_kem_preferences *kem_preferences,
        uint16_t query_iana_id);

bool s2n_tls13_client_must_use_hybrid_kem_length_prefix(const struct s2n_kem_preferences *kem_pref);

const struct s2n_kem_group *s2n_kem_preferences_get_highest_priority_group(const struct s2n_kem_preferences *kem_preferences);

S2N_RESULT s2n_kem_preferences_groups_available(const struct s2n_kem_preferences *kem_preferences, uint32_t *groups_available);
