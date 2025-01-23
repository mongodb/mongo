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

/* While we shouldn't need to reset errno before executing `action`,
 * we do so just in case action doesn't set errno properly on failure.
 */
#define S2N_IO_RETRY_EINTR(result, action) \
    do {                                   \
        errno = 0;                         \
        result = action;                   \
    } while (result < 0 && errno == EINTR)

S2N_RESULT s2n_io_check_write_result(ssize_t result);
S2N_RESULT s2n_io_check_read_result(ssize_t result);
