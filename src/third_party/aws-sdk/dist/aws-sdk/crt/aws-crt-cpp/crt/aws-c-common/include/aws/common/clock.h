#ifndef AWS_COMMON_CLOCK_H
#define AWS_COMMON_CLOCK_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>
#include <aws/common/math.h>

AWS_PUSH_SANE_WARNING_LEVEL

enum aws_timestamp_unit {
    AWS_TIMESTAMP_SECS = 1,
    AWS_TIMESTAMP_MILLIS = 1000,
    AWS_TIMESTAMP_MICROS = 1000000,
    AWS_TIMESTAMP_NANOS = 1000000000,
};

AWS_EXTERN_C_BEGIN

/**
 * Converts 'timestamp' from unit 'convert_from' to unit 'convert_to', if the units are the same then 'timestamp' is
 * returned. If 'remainder' is NOT NULL, it will be set to the remainder if convert_from is a more precise unit than
 * convert_to. To avoid unnecessary branching, 'remainder' is not zero initialized in this function, be sure to set it
 * to 0 first if you care about that kind of thing. If conversion would lead to integer overflow, the timestamp
 * returned will be the highest possible time that is representable, i.e. UINT64_MAX.
 */
AWS_STATIC_IMPL uint64_t aws_timestamp_convert(
    uint64_t timestamp,
    enum aws_timestamp_unit convert_from,
    enum aws_timestamp_unit convert_to,
    uint64_t *remainder);

/**
 * More general form of aws_timestamp_convert that takes arbitrary frequencies rather than the timestamp enum.
 */
AWS_STATIC_IMPL uint64_t
    aws_timestamp_convert_u64(uint64_t ticks, uint64_t old_frequency, uint64_t new_frequency, uint64_t *remainder);

/**
 * Get ticks in nanoseconds (usually 100 nanosecond precision) on the high resolution clock (most-likely TSC). This
 * clock has no bearing on the actual system time. On success, timestamp will be set.
 */
AWS_COMMON_API
int aws_high_res_clock_get_ticks(uint64_t *timestamp);

/**
 * Get ticks in nanoseconds (usually 100 nanosecond precision) on the system clock. Reflects actual system time via
 * nanoseconds since unix epoch. Use with care since an inaccurately set clock will probably cause bugs. On success,
 * timestamp will be set.
 */
AWS_COMMON_API
int aws_sys_clock_get_ticks(uint64_t *timestamp);

AWS_EXTERN_C_END

#ifndef AWS_NO_STATIC_IMPL
#    include <aws/common/clock.inl>
#endif /* AWS_NO_STATIC_IMPL */

AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_CLOCK_H */
