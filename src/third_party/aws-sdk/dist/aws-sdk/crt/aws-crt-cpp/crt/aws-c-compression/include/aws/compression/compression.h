#ifndef AWS_COMPRESSION_COMPRESSION_H
#define AWS_COMPRESSION_COMPRESSION_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/compression/exports.h>

#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

#define AWS_C_COMPRESSION_PACKAGE_ID 3

enum aws_compression_error {
    AWS_ERROR_COMPRESSION_UNKNOWN_SYMBOL = AWS_ERROR_ENUM_BEGIN_RANGE(AWS_C_COMPRESSION_PACKAGE_ID),

    AWS_ERROR_END_COMPRESSION_RANGE = AWS_ERROR_ENUM_END_RANGE(AWS_C_COMPRESSION_PACKAGE_ID)
};

AWS_EXTERN_C_BEGIN

/**
 * Initializes internal datastructures used by aws-c-compression.
 * Must be called before using any functionality in aws-c-compression.
 */
AWS_COMPRESSION_API
void aws_compression_library_init(struct aws_allocator *alloc);

/**
 * Clean up internal datastructures used by aws-c-compression.
 * Must not be called until application is done using functionality in aws-c-compression.
 */
AWS_COMPRESSION_API
void aws_compression_library_clean_up(void);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMPRESSION_COMPRESSION_H */
