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

#include "api/s2n.h"
#include "crypto/s2n_hash.h"
#include "crypto/s2n_signature.h"
#include "utils/s2n_blob.h"

bool s2n_evp_signing_supported();
S2N_RESULT s2n_evp_signing_set_pkey_overrides(struct s2n_pkey *pkey);
int s2n_evp_sign(const struct s2n_pkey *priv, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, struct s2n_blob *signature);
int s2n_evp_verify(const struct s2n_pkey *pub, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, struct s2n_blob *signature);
