/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/math.h>
#include <stdarg.h>

AWS_COMMON_API int aws_add_size_checked_varargs(size_t num, size_t *r, ...) {
    va_list argp;
    va_start(argp, r);

    size_t accum = 0;
    for (size_t i = 0; i < num; ++i) {
        size_t next = va_arg(argp, size_t);
        if (aws_add_size_checked(accum, next, &accum) == AWS_OP_ERR) {
            va_end(argp);
            return AWS_OP_ERR;
        }
    }
    *r = accum;
    va_end(argp);
    return AWS_OP_SUCCESS;
}
