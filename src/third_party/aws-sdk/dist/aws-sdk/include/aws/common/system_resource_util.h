#ifndef AWS_COMMON_SYSTEM_RESOURCE_UTIL_H
#define AWS_COMMON_SYSTEM_RESOURCE_UTIL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

AWS_EXTERN_C_BEGIN

struct aws_memory_usage_stats {
    size_t maxrss;      /* max resident set size in kilobytes since program start */
    size_t page_faults; /* num of page faults since program start */

    size_t _reserved[8];
};

/*
 * Get memory usage for current process.
 * Raises AWS_ERROR_SYS_CALL_FAILURE on failure.
 */
AWS_COMMON_API int aws_init_memory_usage_for_current_process(struct aws_memory_usage_stats *memory_usage);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_SYSTEM_RESOURCE_UTIL_H */
