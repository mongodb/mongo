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

#include "utils/s2n_result.h"

uint64_t s2n_libcrypto_awslc_api_version(void);
S2N_RESULT s2n_libcrypto_validate_runtime(void);
const char *s2n_libcrypto_get_version_name(void);
bool s2n_libcrypto_supports_flag_no_check_time();
