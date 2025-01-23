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

#include "crypto/s2n_hmac.h"
#include "utils/s2n_blob.h"

/*
 * Label structure is `opaque label<7..255> = "tls13 " + Label` per RFC8446.
 * So, we have 255-sizeof("tls13 ") = 249, the maximum label length.
 *
 * Note that all labels defined by RFC 8446 are <12 characters, which
 * avoids an extra hash iteration. However, the exporter functionality
 * (s2n_connection_tls_exporter) allows for longer labels.
 */
#define S2N_MAX_HKDF_EXPAND_LABEL_LENGTH 249

int s2n_hkdf(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *salt,
        const struct s2n_blob *key, const struct s2n_blob *info, struct s2n_blob *output);

int s2n_hkdf_extract(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *salt,
        const struct s2n_blob *key, struct s2n_blob *pseudo_rand_key);

int s2n_hkdf_expand_label(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *secret, const struct s2n_blob *label,
        const struct s2n_blob *context, struct s2n_blob *output);

bool s2n_libcrypto_supports_hkdf();
