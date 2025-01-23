/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/system_resource_util.h>

#include <sys/resource.h>

int aws_init_memory_usage_for_current_process(struct aws_memory_usage_stats *memory_usage) {
    AWS_PRECONDITION(memory_usage);

    AWS_ZERO_STRUCT(*memory_usage);
    struct rusage usage;

    if (getrusage(RUSAGE_SELF, &usage)) {
        return aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
    }

#if defined(AWS_OS_APPLE)
    /*
     * For some reason Apple switched to reporting this in bytes instead of KB
     * around MacOS 10.6.
     * Make it back to KB. Result might be slightly off due to rounding.
     */
    memory_usage->maxrss = usage.ru_maxrss / 1024;
#else
    memory_usage->maxrss = usage.ru_maxrss;
#endif
    memory_usage->page_faults = usage.ru_majflt;
    return AWS_OP_SUCCESS;
}
