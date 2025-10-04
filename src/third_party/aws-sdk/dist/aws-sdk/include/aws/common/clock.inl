#ifndef AWS_COMMON_CLOCK_INL
#define AWS_COMMON_CLOCK_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/clock.h>
#include <aws/common/common.h>
#include <aws/common/math.h>

AWS_EXTERN_C_BEGIN

/**
 * Converts 'timestamp' from unit 'convert_from' to unit 'convert_to', if the units are the same then 'timestamp' is
 * returned. If 'remainder' is NOT NULL, it will be set to the remainder if convert_from is a more precise unit than
 * convert_to (but only if the old frequency is a multiple of the new one). If conversion would lead to integer
 * overflow, the timestamp returned will be the highest possible time that is representable, i.e. UINT64_MAX.
 */
AWS_STATIC_IMPL uint64_t
    aws_timestamp_convert_u64(uint64_t ticks, uint64_t old_frequency, uint64_t new_frequency, uint64_t *remainder) {

    AWS_FATAL_ASSERT(old_frequency > 0 && new_frequency > 0);

    /*
     * The remainder, as defined in the contract of the original version of this function, only makes mathematical
     * sense when the old frequency is a positive multiple of the new frequency.  The new convert function needs to be
     * backwards compatible with the old version's remainder while being a lot more accurate with its conversions
     * in order to handle extreme edge cases of large numbers.
     */
    if (remainder != NULL) {
        *remainder = 0;
        /* only calculate remainder when going from a higher to lower frequency */
        if (new_frequency < old_frequency) {
            uint64_t frequency_remainder = old_frequency % new_frequency;
            /* only calculate remainder when the old frequency is evenly divisible by the new one */
            if (frequency_remainder == 0) {
                uint64_t frequency_ratio = old_frequency / new_frequency;
                *remainder = ticks % frequency_ratio;
            }
        }
    }

    /*
     * Now do the actual conversion.
     */
    uint64_t old_seconds_elapsed = ticks / old_frequency;
    uint64_t old_remainder = ticks - old_seconds_elapsed * old_frequency;

    uint64_t new_ticks_whole_part = aws_mul_u64_saturating(old_seconds_elapsed, new_frequency);

    /*
     * This could be done in one of three ways:
     *
     * (1) (old_remainder / old_frequency) * new_frequency - this would be completely wrong since we know that
     * (old_remainder / old_frequency) < 1 = 0
     *
     * (2) old_remainder * (new_frequency / old_frequency) - this only gives a good solution when new_frequency is
     * a multiple of old_frequency
     *
     * (3) (old_remainder * new_frequency) / old_frequency - this is how we do it below, the primary concern is if
     * the initial multiplication can overflow.  For that to be the case, we would need to be using old and new
     * frequencies in the billions.  This does not appear to be the case in any current machine's hardware counters.
     *
     * Ignoring arbitrary frequencies, even a nanosecond to nanosecond conversion would not overflow either.
     *
     * If this did become an issue, we would potentially need to use intrinsics/platform support for 128 bit math.
     *
     * For review consideration:
     *   (1) should we special case frequencies being a multiple of the other?
     *   (2) should we special case frequencies being the same?  A ns-to-ns conversion does the full math and
     *   approaches overflow (but cannot actually do so).
     */
    uint64_t new_ticks_remainder_part = aws_mul_u64_saturating(old_remainder, new_frequency) / old_frequency;

    return aws_add_u64_saturating(new_ticks_whole_part, new_ticks_remainder_part);
}

AWS_STATIC_IMPL uint64_t aws_timestamp_convert(
    uint64_t timestamp,
    enum aws_timestamp_unit convert_from,
    enum aws_timestamp_unit convert_to,
    uint64_t *remainder) {

    return aws_timestamp_convert_u64(timestamp, convert_from, convert_to, remainder);
}

AWS_EXTERN_C_END

#endif /* AWS_COMMON_CLOCK_INL */
