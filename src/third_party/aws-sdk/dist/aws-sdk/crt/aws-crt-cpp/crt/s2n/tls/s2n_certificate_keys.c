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

#include "tls/s2n_certificate_keys.h"

#include <openssl/objects.h>

#include "utils/s2n_safety.h"

const struct s2n_certificate_key s2n_rsa_rsae_1024 = {
    .public_key_libcrypto_nid = NID_rsaEncryption,
    .name = "rsa_1024",
    .bits = 1024,
};

const struct s2n_certificate_key s2n_rsa_rsae_2048 = {
    .public_key_libcrypto_nid = NID_rsaEncryption,
    .name = "rsa_2048",
    .bits = 2048,
};

const struct s2n_certificate_key s2n_rsa_rsae_3072 = {
    .public_key_libcrypto_nid = NID_rsaEncryption,
    .name = "rsa_3072",
    .bits = 3072,
};

const struct s2n_certificate_key s2n_rsa_rsae_4096 = {
    .public_key_libcrypto_nid = NID_rsaEncryption,
    .name = "rsa_4096",
    .bits = 4096,
};

const struct s2n_certificate_key s2n_rsa_pss_1024 = {
    .public_key_libcrypto_nid = NID_rsassaPss,
    .name = "rsa_pss_1024",
    .bits = 1024,
};

const struct s2n_certificate_key s2n_rsa_pss_2048 = {
    .public_key_libcrypto_nid = NID_rsassaPss,
    .name = "rsa_pss_2048",
    .bits = 2048,
};

const struct s2n_certificate_key s2n_rsa_pss_3072 = {
    .public_key_libcrypto_nid = NID_rsassaPss,
    .name = "rsa_pss_3072",
    .bits = 3072,
};

const struct s2n_certificate_key s2n_rsa_pss_4096 = {
    .public_key_libcrypto_nid = NID_rsassaPss,
    .name = "rsa_pss_4096",
    .bits = 4096,
};

const struct s2n_certificate_key s2n_ec_p256 = {
    .public_key_libcrypto_nid = NID_X9_62_prime256v1,
    .name = "ecdsa_p256",
    .bits = 256,
};

const struct s2n_certificate_key s2n_ec_p384 = {
    .public_key_libcrypto_nid = NID_secp384r1,
    .name = "ecdsa_p384",
    .bits = 384,
};

const struct s2n_certificate_key s2n_ec_p521 = {
    .public_key_libcrypto_nid = NID_secp521r1,
    .name = "ecdsa_p521",
    .bits = 521,
};

const struct s2n_certificate_key *s2n_certificate_keys_rfc9151[] = {
    /**
     *= https://www.rfc-editor.org/rfc/rfc9151#section-5.1
     *# CNSA (D)TLS connections MUST use secp384r1
     **/
    &s2n_ec_p384,

    /**
     *= https://www.rfc-editor.org/rfc/rfc9151#section-5.2
     *# CNSA specifies a minimum modulus size of 3072 bits; however, only two
     *# modulus sizes (3072 bits and 4096 bits) are supported by this profile.
     **/
    &s2n_rsa_rsae_3072,
    &s2n_rsa_rsae_4096,
    &s2n_rsa_pss_3072,
    &s2n_rsa_pss_4096,
};

struct s2n_certificate_key_preferences s2n_certificate_key_preferences_rfc9151 = {
    .count = s2n_array_len(s2n_certificate_keys_rfc9151),
    .certificate_keys = s2n_certificate_keys_rfc9151,
};
