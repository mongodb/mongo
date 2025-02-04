/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_SDKUTILS_PARTITIONS_H
#define AWS_SDKUTILS_PARTITIONS_H

#include <aws/common/byte_buf.h>
#include <aws/sdkutils/sdkutils.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_partitions_config;

AWS_EXTERN_C_BEGIN

AWS_SDKUTILS_API struct aws_byte_cursor aws_partitions_get_supported_version(void);

/*
 * Create new partitions config from a json string.
 * In cases of failure NULL is returned and last error is set.
 */
AWS_SDKUTILS_API struct aws_partitions_config *aws_partitions_config_new_from_string(
    struct aws_allocator *allocator,
    struct aws_byte_cursor json);

/*
 * Increment ref count
 */
AWS_SDKUTILS_API struct aws_partitions_config *aws_partitions_config_acquire(struct aws_partitions_config *partitions);

/*
 * Decrement ref count
 */
AWS_SDKUTILS_API struct aws_partitions_config *aws_partitions_config_release(struct aws_partitions_config *partitions);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_SDKUTILS_PARTITIONS_H */
