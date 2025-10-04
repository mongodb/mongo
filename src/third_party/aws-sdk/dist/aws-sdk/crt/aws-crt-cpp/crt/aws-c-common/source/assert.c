/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>

#include <aws/common/system_info.h>

#include <stdio.h>
#include <stdlib.h>

void aws_fatal_assert(const char *cond_str, const char *file, int line) {
    aws_debug_break();
    fprintf(stderr, "Fatal error condition occurred in %s:%d: %s\nExiting Application\n", file, line, cond_str);
    aws_backtrace_print(stderr, NULL);
    abort();
}
