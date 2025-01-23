#ifndef AWS_COMMON_TIME_H
#define AWS_COMMON_TIME_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/common.h>

#include <time.h>

AWS_PUSH_SANE_WARNING_LEVEL
AWS_EXTERN_C_BEGIN

/**
 * Cross platform friendly version of timegm
 */
AWS_COMMON_API time_t aws_timegm(struct tm *const t);

/**
 * Cross platform friendly version of localtime_r
 */
AWS_COMMON_API void aws_localtime(time_t time, struct tm *t);

/**
 * Cross platform friendly version of gmtime_r
 */
AWS_COMMON_API void aws_gmtime(time_t time, struct tm *t);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_TIME_H */
