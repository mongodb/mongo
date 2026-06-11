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

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include "stuffer/s2n_stuffer.h"
#include "tls/policy/s2n_policy_feature.h"
#include "tls/s2n_security_policies.h"
#include "tls/s2n_security_rules.h"
#include "utils/s2n_safety.h"

#define BOOL_STR(b) ((b) ? "yes" : "no")

extern const struct s2n_security_rule security_rule_definitions[S2N_SECURITY_RULES_COUNT];

static const char *version_strs[] = {
    [S2N_SSLv2] = "SSLv2",
    [S2N_SSLv3] = "SSLv3",
    [S2N_TLS10] = "TLS1.0",
    [S2N_TLS11] = "TLS1.1",
    [S2N_TLS12] = "TLS1.2",
    [S2N_TLS13] = "TLS1.3",
};

static S2N_RESULT s2n_security_policy_write_format_v1_to_stuffer(const struct s2n_security_policy *policy, struct s2n_stuffer *stuffer)
{
    RESULT_ENSURE_REF(policy);
    RESULT_ENSURE_REF(stuffer);

    const char *version_str = NULL;
    if (policy->minimum_protocol_version <= S2N_TLS13) {
        version_str = version_strs[policy->minimum_protocol_version];
    }
    RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "min version: %s\n", version_str ? version_str : "None"));

    RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "rules:\n"));
    for (size_t i = 0; i < S2N_SECURITY_RULES_COUNT; i++) {
        RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "- %s: %s\n",
                security_rule_definitions[i].name, BOOL_STR(policy->rules[i])));
    }

    RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "cipher suites:\n"));
    if (policy->cipher_preferences->allow_chacha20_boosting) {
        RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "- chacha20 boosting enabled\n"));
    }
    for (size_t i = 0; i < policy->cipher_preferences->count; i++) {
        RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "- %s\n", policy->cipher_preferences->suites[i]->iana_name));
    }

    RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "signature schemes:\n"));
    for (size_t i = 0; i < policy->signature_preferences->count; i++) {
        RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "- %s\n", policy->signature_preferences->signature_schemes[i]->name));
    }

    RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "curves:\n"));
    for (size_t i = 0; i < policy->ecc_preferences->count; i++) {
        RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "- %s\n", policy->ecc_preferences->ecc_curves[i]->name));
    }

    for (size_t i = 0; policy->strongly_preferred_groups != NULL && i < policy->strongly_preferred_groups->count; i++) {
        RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "strongly preferred groups:\n"));
        const struct s2n_ecc_named_curve *strongly_preferred_curve = NULL;
        const struct s2n_kem_group *strongly_preferred_kem_group = NULL;
        bool found = false;
        RESULT_GUARD_POSIX(s2n_find_ecc_curve_from_iana_id(policy->strongly_preferred_groups->iana_ids[i], &strongly_preferred_curve, &found));
        RESULT_GUARD_POSIX(s2n_find_kem_group_from_iana_id(policy->strongly_preferred_groups->iana_ids[i], &strongly_preferred_kem_group, &found));
        RESULT_ENSURE((strongly_preferred_curve == NULL) != (strongly_preferred_kem_group == NULL), S2N_ERR_INVALID_SUPPORTED_GROUP_STATE);

        if (strongly_preferred_curve != NULL) {
            RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "- %s\n", strongly_preferred_curve->name));
        }

        if (strongly_preferred_kem_group != NULL) {
            RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "- %s\n", strongly_preferred_kem_group->name));
        }
    }

    if (policy->certificate_signature_preferences) {
        if (policy->certificate_preferences_apply_locally) {
            RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "certificate preferences apply locally\n"));
        }
        RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "certificate signature schemes:\n"));
        for (size_t i = 0; i < policy->certificate_signature_preferences->count; i++) {
            RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "- %s\n",
                    policy->certificate_signature_preferences->signature_schemes[i]->name));
        }
    }

    if (policy->certificate_key_preferences) {
        RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "certificate keys:\n"));
        for (size_t i = 0; i < policy->certificate_key_preferences->count; i++) {
            RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "- %s\n",
                    policy->certificate_key_preferences->certificate_keys[i]->name));
        }
    }

    if (policy->kem_preferences && policy->kem_preferences != &kem_preferences_null) {
        RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "pq:\n"));
        RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "- revision: %i\n",
                policy->kem_preferences->tls13_pq_hybrid_draft_revision));

        if (policy->kem_preferences->kem_count > 0) {
            RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "- kems:\n"));
            for (size_t i = 0; i < policy->kem_preferences->kem_count; i++) {
                RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "-- %s\n",
                        policy->kem_preferences->kems[i]->name));
            }
        }

        RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "- kem groups:\n"));
        for (size_t i = 0; i < policy->kem_preferences->tls13_kem_group_count; i++) {
            RESULT_GUARD_POSIX(s2n_stuffer_printf(stuffer, "-- %s\n",
                    policy->kem_preferences->tls13_kem_groups[i]->name));
        }
    }

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_security_policy_write_to_stuffer(const struct s2n_security_policy *policy,
        s2n_policy_format format, struct s2n_stuffer *stuffer)
{
    RESULT_ENSURE_REF(policy);
    RESULT_ENSURE_REF(stuffer);

    switch (format) {
        case S2N_POLICY_FORMAT_DEBUG_V1:
            RESULT_GUARD(s2n_security_policy_write_format_v1_to_stuffer(policy, stuffer));
            break;
        default:
            RESULT_BAIL(S2N_ERR_INVALID_ARGUMENT);
    }

    return S2N_RESULT_OK;
}

int s2n_security_policy_write_length(const struct s2n_security_policy *policy,
        s2n_policy_format format, uint32_t *length)
{
    POSIX_ENSURE_REF(policy);
    POSIX_ENSURE_REF(length);

    DEFER_CLEANUP(struct s2n_stuffer stuffer = { 0 }, s2n_stuffer_free);
    POSIX_GUARD(s2n_stuffer_growable_alloc(&stuffer, 1024));

    POSIX_GUARD_RESULT(s2n_security_policy_write_to_stuffer(policy, format, &stuffer));

    *length = s2n_stuffer_data_available(&stuffer);

    return S2N_SUCCESS;
}

int s2n_security_policy_write_bytes(const struct s2n_security_policy *policy,
        s2n_policy_format format, uint8_t *buffer, uint32_t buffer_length, uint32_t *output_size)
{
    POSIX_ENSURE_REF(policy);
    POSIX_ENSURE_REF(buffer);
    POSIX_ENSURE_REF(output_size);
    *output_size = 0;

    /* Intermediate stuffer is needed because s2n_stuffer_printf requires temporary space for null 
     * terminators. We cannot write directly to application memory which may not have the extra byte
     * available 
     */
    DEFER_CLEANUP(struct s2n_stuffer stuffer = { 0 }, s2n_stuffer_free);
    POSIX_GUARD(s2n_stuffer_growable_alloc(&stuffer, 1024));
    POSIX_GUARD_RESULT(s2n_security_policy_write_to_stuffer(policy, format, &stuffer));
    uint32_t required_size = s2n_stuffer_data_available(&stuffer);
    POSIX_ENSURE(buffer_length >= required_size, S2N_ERR_INSUFFICIENT_MEM_SIZE);

    POSIX_CHECKED_MEMCPY(buffer, stuffer.blob.data, required_size);
    *output_size = s2n_stuffer_data_available(&stuffer);
    return S2N_SUCCESS;
}

int s2n_security_policy_write_fd(const struct s2n_security_policy *policy,
        s2n_policy_format format, int fd, uint32_t *output_size)
{
    POSIX_ENSURE_REF(policy);
    POSIX_ENSURE_REF(output_size);
    POSIX_ENSURE(fd >= 0, S2N_ERR_INVALID_ARGUMENT);
    *output_size = 0;

    DEFER_CLEANUP(struct s2n_stuffer stuffer = { 0 }, s2n_stuffer_free);
    POSIX_GUARD(s2n_stuffer_growable_alloc(&stuffer, 1024));

    POSIX_GUARD_RESULT(s2n_security_policy_write_to_stuffer(policy, format, &stuffer));

    uint32_t data_size = s2n_stuffer_data_available(&stuffer);
    ssize_t written = write(fd, stuffer.blob.data, data_size);
    POSIX_ENSURE(written == (ssize_t) data_size, S2N_ERR_IO);

    *output_size = (uint32_t) written;
    return S2N_SUCCESS;
}
