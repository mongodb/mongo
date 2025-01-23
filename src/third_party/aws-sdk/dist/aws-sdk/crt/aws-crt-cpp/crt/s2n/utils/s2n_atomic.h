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

#include <signal.h>

#include "utils/s2n_result.h"

/* s2n-tls allows s2n_send and s2n_recv to be called concurrently.
 * There are a handful of values that both methods need to access.
 * However, C99 has no concept of concurrency, so provides no atomic data types.
 */

/* Wrap the underlying value in a struct to encourage developers to only use
 * the provided atomic methods.
 */
typedef struct {
    /* Traditionally, s2n-tls uses sig_atomic_t for its weak guarantee of
     * atomicity for interrupts.
     */
    sig_atomic_t val;
} s2n_atomic_flag;

/* These methods use compiler atomic built-ins if available and lock-free, but otherwise
 * rely on setting / clearing a small value generally being atomic in practice.
 */
S2N_RESULT s2n_atomic_init();
void s2n_atomic_flag_set(s2n_atomic_flag *var);
void s2n_atomic_flag_clear(s2n_atomic_flag *var);
bool s2n_atomic_flag_test(s2n_atomic_flag *var);
