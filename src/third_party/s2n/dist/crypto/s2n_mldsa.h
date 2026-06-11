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

#include "crypto/s2n_pkey.h"
#include "utils/s2n_result.h"

#if S2N_LIBCRYPTO_SUPPORTS_MLDSA
    #define S2N_NID_MLDSA44 NID_MLDSA44
    #define S2N_NID_MLDSA65 NID_MLDSA65
    #define S2N_NID_MLDSA87 NID_MLDSA87
#else
    #define S2N_NID_MLDSA44 NID_undef
    #define S2N_NID_MLDSA65 NID_undef
    #define S2N_NID_MLDSA87 NID_undef
#endif

/*
 * The draft ML-DSA PKI RFC
 * (https://www.ietf.org/archive/id/draft-ietf-lamps-dilithium-certificates-07.html)
 * includes a very helpful table of constants related to ML-DSA in Appendix B:
 * |=======+=======+=====+========+========+========|
 * | Level | (k,l) | eta |  Sig.  | Public | Private|
 * |       |       |     |  (B)   | Key(B) | Key(B) |
 * |=======+=======+=====+========+========+========|
 * |   2   | (4,4) |  2  |  2420  |  1312  |  32    |
 * |   3   | (6,5) |  4  |  3309  |  1952  |  32    |
 * |   5   | (8,7) |  2  |  4627  |  2592  |  32    |
 * |=======+=======+=====+========+========+========|
 */
#define S2N_MLDSA_MAX_PUB_KEY_SIZE 2592

bool s2n_mldsa_is_supported();
S2N_RESULT s2n_mldsa_init_mu_hash(struct s2n_hash_state *state, const struct s2n_pkey *pub_key);
