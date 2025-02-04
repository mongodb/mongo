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

#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_kem.h"
#include "utils/s2n_result.h"

typedef enum {
    S2N_PERFECT_FORWARD_SECRECY = 0,
    S2N_FIPS_140_3,
    S2N_SECURITY_RULES_COUNT,
} s2n_security_rule_id;

struct s2n_security_rule_result {
    bool found_error;
    bool write_output;
    struct s2n_stuffer output;
};
S2N_RESULT s2n_security_rule_result_init_output(struct s2n_security_rule_result *result);
S2N_CLEANUP_RESULT s2n_security_rule_result_free(struct s2n_security_rule_result *result);

struct s2n_security_policy;
struct s2n_cipher_suite;
struct s2n_signature_scheme;
struct s2n_ecc_named_curve;

struct s2n_security_rule {
    const char *name;
    S2N_RESULT (*validate_cipher_suite)(const struct s2n_cipher_suite *cipher_suite, bool *valid);
    S2N_RESULT (*validate_sig_scheme)(const struct s2n_signature_scheme *sig_scheme, bool *valid);
    S2N_RESULT (*validate_cert_sig_scheme)(const struct s2n_signature_scheme *sig_scheme, bool *valid);
    S2N_RESULT (*validate_curve)(const struct s2n_ecc_named_curve *curve, bool *valid);
    S2N_RESULT (*validate_hybrid_group)(const struct s2n_kem_group *hybrid_group, bool *valid);
    S2N_RESULT (*validate_version)(uint8_t version, bool *valid);
};

S2N_RESULT s2n_security_policy_validate_security_rules(
        const struct s2n_security_policy *policy,
        struct s2n_security_rule_result *result);
