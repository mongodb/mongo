#ifndef AWS_COMMON_ERROR_INL
#define AWS_COMMON_ERROR_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/error.h>

AWS_EXTERN_C_BEGIN

/*
 * Raises `err` to the installed callbacks, and sets the thread's error.
 */
AWS_STATIC_IMPL
int aws_raise_error(int err) {
    /*
     * Certain static analyzers can't see through the out-of-line call to aws_raise_error,
     * and assume that this might return AWS_OP_SUCCESS. We'll put the return inline just
     * to help with their assumptions.
     */

    aws_raise_error_private(err);

    return AWS_OP_ERR;
}

AWS_EXTERN_C_END

#endif /* AWS_COMMON_ERROR_INL */
