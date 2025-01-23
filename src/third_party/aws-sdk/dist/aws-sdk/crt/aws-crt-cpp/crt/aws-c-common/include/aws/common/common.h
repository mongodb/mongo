#ifndef AWS_COMMON_COMMON_H
#define AWS_COMMON_COMMON_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/config.h>
#include <aws/common/exports.h>

#include <aws/common/allocator.h>
#include <aws/common/assert.h>
#include <aws/common/error.h>
#include <aws/common/macros.h>
#include <aws/common/platform.h>
#include <aws/common/predicates.h>
#include <aws/common/stdbool.h>
#include <aws/common/stdint.h>
#include <aws/common/zero.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h> /* for abort() */
#include <string.h>

AWS_PUSH_SANE_WARNING_LEVEL
AWS_EXTERN_C_BEGIN

/**
 * Initializes internal data structures used by aws-c-common.
 * Must be called before using any functionality in aws-c-common.
 */
AWS_COMMON_API
void aws_common_library_init(struct aws_allocator *allocator);

/**
 * Shuts down the internal data structures used by aws-c-common.
 */
AWS_COMMON_API
void aws_common_library_clean_up(void);

AWS_COMMON_API
void aws_common_fatal_assert_library_initialized(void);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_COMMON_H */
