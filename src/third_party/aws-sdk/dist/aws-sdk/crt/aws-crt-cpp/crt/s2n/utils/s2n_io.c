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

#include "utils/s2n_io.h"

#include <errno.h>

#include "utils/s2n_safety.h"

S2N_RESULT s2n_io_check_write_result(ssize_t result)
{
    if (result < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            RESULT_BAIL(S2N_ERR_IO_BLOCKED);
        }
        RESULT_BAIL(S2N_ERR_IO);
    }
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_io_check_read_result(ssize_t result)
{
    RESULT_GUARD(s2n_io_check_write_result(result));
    if (result == 0) {
        RESULT_BAIL(S2N_ERR_CLOSED);
    }
    return S2N_RESULT_OK;
}
