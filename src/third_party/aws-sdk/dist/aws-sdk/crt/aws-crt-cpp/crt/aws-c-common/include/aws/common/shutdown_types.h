#ifndef AWS_COMMON_SHUTDOWN_TYPES_H
#define AWS_COMMON_SHUTDOWN_TYPES_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

typedef void(aws_simple_completion_callback)(void *);

/**
 * Configuration for a callback to invoke when something has been completely
 * cleaned up.  Primarily used in async cleanup control flows.
 */
struct aws_shutdown_callback_options {

    /**
     * Function to invoke when the associated object is fully destroyed.
     */
    aws_simple_completion_callback *shutdown_callback_fn;

    /**
     * User data to invoke the shutdown callback with.
     */
    void *shutdown_callback_user_data;
};

AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_SHUTDOWN_TYPES_H */
