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

#include "crypto/s2n_fips.h"
#include "utils/s2n_result.h"
#include "utils/s2n_safety.h"

bool s2n_pq_is_enabled(void);
bool s2n_libcrypto_supports_evp_kem(void);
bool s2n_libcrypto_supports_mlkem(void);
