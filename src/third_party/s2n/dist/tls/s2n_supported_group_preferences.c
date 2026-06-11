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

#include "tls/s2n_supported_group_preferences.h"

#include "tls/s2n_ecc_preferences.h"

const uint16_t cnsa_1_supported_group_iana_ids[] = {
    TLS_EC_CURVE_SECP_384_R1
};

const struct s2n_supported_group_preferences cnsa_1_strong_preference = {
    .count = s2n_array_len(cnsa_1_supported_group_iana_ids),
    .iana_ids = cnsa_1_supported_group_iana_ids
};
