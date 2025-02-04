#ifndef AWS_COMMON_ENVIRONMENT_H
#define AWS_COMMON_ENVIRONMENT_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_string;

/*
 * Simple shims to the appropriate platform calls for environment variable manipulation.
 *
 * Not thread safe to use set/unset unsynced with get.  Set/unset only used in unit tests.
 */
AWS_EXTERN_C_BEGIN

/*
 * Get the value of an environment variable.  If the variable is not set, the output string will be set to NULL.
 * Not thread-safe
 */
AWS_COMMON_API
int aws_get_environment_value(
    struct aws_allocator *allocator,
    const struct aws_string *variable_name,
    struct aws_string **value_out);

/*
 * Set the value of an environment variable.  On Windows, setting a variable to the empty string will actually unset it.
 * Not thread-safe
 */
AWS_COMMON_API
int aws_set_environment_value(const struct aws_string *variable_name, const struct aws_string *value);

/*
 * Unset an environment variable.
 * Not thread-safe
 */
AWS_COMMON_API
int aws_unset_environment_value(const struct aws_string *variable_name);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_ENVIRONMENT_H */
