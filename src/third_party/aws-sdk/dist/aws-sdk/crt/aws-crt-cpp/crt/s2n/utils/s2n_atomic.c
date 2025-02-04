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

#include "utils/s2n_atomic.h"

#include <signal.h>

#include "utils/s2n_safety.h"

static sig_atomic_t set_val = true;
static sig_atomic_t clear_val = false;

S2N_RESULT s2n_atomic_init()
{
#if S2N_ATOMIC_SUPPORTED && S2N_THREAD_SANITIZER
    RESULT_ENSURE(__atomic_always_lock_free(sizeof(s2n_atomic_flag), NULL), S2N_ERR_ATOMIC);
#endif
    return S2N_RESULT_OK;
}

void s2n_atomic_flag_set(s2n_atomic_flag *var)
{
#if S2N_ATOMIC_SUPPORTED && S2N_THREAD_SANITIZER
    __atomic_store(&var->val, &set_val, __ATOMIC_RELAXED);
#else
    var->val = set_val;
#endif
}

void s2n_atomic_flag_clear(s2n_atomic_flag *var)
{
#if S2N_ATOMIC_SUPPORTED && S2N_THREAD_SANITIZER
    __atomic_store(&var->val, &clear_val, __ATOMIC_RELAXED);
#else
    var->val = clear_val;
#endif
}

bool s2n_atomic_flag_test(s2n_atomic_flag *var)
{
#if S2N_ATOMIC_SUPPORTED && S2N_THREAD_SANITIZER
    sig_atomic_t result = 0;
    __atomic_load(&var->val, &result, __ATOMIC_RELAXED);
    return result;
#else
    return var->val;
#endif
}
