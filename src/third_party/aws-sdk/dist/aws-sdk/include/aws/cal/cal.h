#ifndef AWS_CAL_CAL_H
#define AWS_CAL_CAL_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>
#include <aws/common/logging.h>

#include <aws/cal/exports.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_allocator;

#define AWS_C_CAL_PACKAGE_ID 7

enum aws_cal_errors {
    AWS_ERROR_CAL_SIGNATURE_VALIDATION_FAILED = AWS_ERROR_ENUM_BEGIN_RANGE(AWS_C_CAL_PACKAGE_ID),
    AWS_ERROR_CAL_MISSING_REQUIRED_KEY_COMPONENT,
    AWS_ERROR_CAL_INVALID_KEY_LENGTH_FOR_ALGORITHM,
    AWS_ERROR_CAL_UNKNOWN_OBJECT_IDENTIFIER,
    AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED,
    AWS_ERROR_CAL_MISMATCHED_DER_TYPE,
    AWS_ERROR_CAL_UNSUPPORTED_ALGORITHM,
    AWS_ERROR_CAL_BUFFER_TOO_LARGE_FOR_ALGORITHM,
    AWS_ERROR_CAL_INVALID_CIPHER_MATERIAL_SIZE_FOR_ALGORITHM,
    AWS_ERROR_CAL_DER_UNSUPPORTED_NEGATIVE_INT,
    AWS_ERROR_CAL_UNSUPPORTED_KEY_FORMAT,
    AWS_ERROR_CAL_CRYPTO_OPERATION_FAILED,
    AWS_ERROR_CAL_END_RANGE = AWS_ERROR_ENUM_END_RANGE(AWS_C_CAL_PACKAGE_ID)
};

enum aws_cal_log_subject {
    AWS_LS_CAL_GENERAL = AWS_LOG_SUBJECT_BEGIN_RANGE(AWS_C_CAL_PACKAGE_ID),
    AWS_LS_CAL_ECC,
    AWS_LS_CAL_HASH,
    AWS_LS_CAL_HMAC,
    AWS_LS_CAL_DER,
    AWS_LS_CAL_LIBCRYPTO_RESOLVE,
    AWS_LS_CAL_RSA,

    AWS_LS_CAL_LAST = AWS_LOG_SUBJECT_END_RANGE(AWS_C_CAL_PACKAGE_ID)
};

AWS_EXTERN_C_BEGIN

AWS_CAL_API void aws_cal_library_init(struct aws_allocator *allocator);
AWS_CAL_API void aws_cal_library_clean_up(void);

/*
 * Every CRT thread that might invoke aws-lc functionality should call this as part of the thread at_exit process
 */
AWS_CAL_API void aws_cal_thread_clean_up(void);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_CAL_CAL_H */
