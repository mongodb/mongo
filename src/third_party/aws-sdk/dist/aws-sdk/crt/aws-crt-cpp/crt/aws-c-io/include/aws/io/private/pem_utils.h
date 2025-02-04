#ifndef AWS_IO_PEM_UTILS_H
#define AWS_IO_PEM_UTILS_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/io.h>

AWS_EXTERN_C_BEGIN

/**
 * Cleanup Function that parses the full PEM Chain object once and strip the comments out for the pem parser not
 * handling the comments. The passed in pem will be cleaned up.
 *
 * - Garbage characters in-between PEM objects (characters before the first BEGIN or after an END and before the next
 * BEGIN) are removed
 *
 * - AWS_ERROR_INVALID_ARGUMENT will be raised if the file contains no PEM encoded data.
 */
AWS_IO_API
int aws_sanitize_pem(struct aws_byte_buf *pem, struct aws_allocator *allocator);

AWS_EXTERN_C_END
#endif /* AWS_IO_PEM_UTILS_H */
