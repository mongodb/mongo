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

#include "utils/s2n_safety.h"

void *s2n_ensure_memmove_trace(void *to, const void *from, size_t size)
{
    PTR_ENSURE_REF(to);
    PTR_ENSURE_REF(from);

    /* use memmove instead of memcpy since it'll handle overlapping regions and not result in UB */
    void *result = memmove(to, from, size);
    PTR_ENSURE_REF(result);
    return result;
}
