/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_COMMON_POSIX_COMMON_INL
#define AWS_COMMON_POSIX_COMMON_INL

#include <aws/common/common.h>

#include <errno.h>

AWS_EXTERN_C_BEGIN

static inline int aws_private_convert_and_raise_error_code(int error_code) {
    switch (error_code) {
        case 0:
            return AWS_OP_SUCCESS;
        case EINVAL:
            return aws_raise_error(AWS_ERROR_MUTEX_NOT_INIT);
        case EBUSY:
            return aws_raise_error(AWS_ERROR_MUTEX_TIMEOUT);
        case EPERM:
            return aws_raise_error(AWS_ERROR_MUTEX_CALLER_NOT_OWNER);
        case ENOMEM:
            return aws_raise_error(AWS_ERROR_OOM);
        case EDEADLK:
            return aws_raise_error(AWS_ERROR_THREAD_DEADLOCK_DETECTED);
        default:
            return aws_raise_error(AWS_ERROR_MUTEX_FAILED);
    }
}

AWS_EXTERN_C_END

#endif /* AWS_COMMON_POSIX_COMMON_INL */
