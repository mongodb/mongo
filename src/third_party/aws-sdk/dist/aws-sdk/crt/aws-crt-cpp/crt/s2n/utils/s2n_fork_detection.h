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

#include "utils/s2n_result.h"

S2N_RESULT s2n_get_fork_generation_number(uint64_t *return_fork_generation_number);
bool s2n_is_madv_wipeonfork_supported(void);
bool s2n_is_map_inherit_zero_supported(void);
bool s2n_is_pthread_atfork_supported(void);

/* Use for testing only */
S2N_RESULT s2n_ignore_wipeonfork_and_inherit_zero_for_testing(void);
S2N_RESULT s2n_ignore_pthread_atfork_for_testing(void);
