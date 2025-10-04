#ifndef AWS_COMMON_PRIVATE_SYSTEM_INFO_PRIV_H
#define AWS_COMMON_PRIVATE_SYSTEM_INFO_PRIV_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/byte_buf.h>
#include <aws/common/ref_count.h>
#include <aws/common/string.h>
#include <aws/common/system_info.h>

struct aws_system_environment {
    struct aws_allocator *allocator;
    struct aws_ref_count ref_count;
    struct aws_byte_buf virtualization_vendor;
    struct aws_byte_buf product_name;
    enum aws_platform_os os;
    size_t cpu_count;
    size_t cpu_group_count;
    void *impl;
};

/**
 * For internal implementors. Fill in info in env that you're able to grab, such as dmi info, os version strings etc...
 * in here. The default just returns AWS_OP_SUCCESS. This is currently only implemented for linux.
 *
 * Returns AWS_OP_ERR if the implementation wasn't able to fill in required information for the platform.
 */
int aws_system_environment_load_platform_impl(struct aws_system_environment *env);

/**
 * For internal implementors. Cleans up anything allocated in aws_system_environment_load_platform_impl,
 * but does not release the memory for env.
 */
void aws_system_environment_destroy_platform_impl(struct aws_system_environment *env);

#endif // AWS_COMMON_PRIVATE_SYSTEM_INFO_PRIV_H
