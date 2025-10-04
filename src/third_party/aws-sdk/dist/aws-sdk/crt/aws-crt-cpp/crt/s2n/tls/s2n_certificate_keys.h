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

#include <stdint.h>

struct s2n_certificate_key {
    const char *name;
    uint16_t public_key_libcrypto_nid;

    /* modulus for RSA key, size for EC key */
    uint16_t bits;
};

struct s2n_certificate_key_preferences {
    uint8_t count;
    const struct s2n_certificate_key *const *certificate_keys;
};

extern const struct s2n_certificate_key s2n_rsa_rsae_1024;
extern const struct s2n_certificate_key s2n_rsa_rsae_2048;
extern const struct s2n_certificate_key s2n_rsa_rsae_3072;
extern const struct s2n_certificate_key s2n_rsa_rsae_4096;

extern const struct s2n_certificate_key s2n_rsa_pss_1024;
extern const struct s2n_certificate_key s2n_rsa_pss_2048;
extern const struct s2n_certificate_key s2n_rsa_pss_3072;
extern const struct s2n_certificate_key s2n_rsa_pss_4096;

extern const struct s2n_certificate_key s2n_ec_p256;
extern const struct s2n_certificate_key s2n_ec_p384;
extern const struct s2n_certificate_key s2n_ec_p521;

extern struct s2n_certificate_key_preferences s2n_certificate_key_preferences_rfc9151;
