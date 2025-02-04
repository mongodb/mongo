/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_RATE_LIMITERS_H
#define AWS_RATE_LIMITERS_H

#include <aws/mqtt/mqtt.h>

#include <aws/io/io.h>

struct aws_rate_limiter_token_bucket_options {
    /* Clock function override.  If left null, the high resolution clock will be used */
    aws_io_clock_fn *clock_fn;

    /* How many tokens regenerate per second? */
    uint64_t tokens_per_second;

    /* Initial amount of tokens the limiter will start with */
    uint64_t initial_token_count;

    /*
     * Maximum amount of tokens the limiter can hold.  Regenerated tokens that exceed this maximum are
     * discarded
     */
    uint64_t maximum_token_count;
};

/**
 * A token-bucket based rate limiter.
 *
 * Has an unusually complex implementation due to implementer-desired constraints:
 *
 *   (1) Model regeneration as an integral rate per second.  This is for ease-of-use.  A regeneration interval would
 *      be a much simpler implementation, but not as intuitive (or accurate for non-integral rates).
 *   (2) Integer math only.  Not comfortable falling back on doubles and not having a good understanding of the
 *      accuracy issues, over time, that doing so would create.
 *   (3) Minimize as much as possible the dangers of multiplication saturation and integer division round-down.
 *   (4) No integer division round-off "error" accumulation allowed.  Arguments could be made that it might be small
 *      enough to never make a difference but I'd rather not even have the argument at all.
 *   (5) A perfectly accurate how-long-must-I-wait query.  Not just a safe over-estimate.
 */
struct aws_rate_limiter_token_bucket {
    uint64_t last_service_time;
    uint64_t current_token_count;

    uint64_t fractional_nanos;
    uint64_t fractional_nano_tokens;

    struct aws_rate_limiter_token_bucket_options config;
};

AWS_EXTERN_C_BEGIN

/**
 * Initializes a token-bucket-based rate limiter
 *
 * @param limiter rate limiter to intiialize
 * @param options configuration values for the token bucket rate limiter
 * @return AWS_OP_SUCCESS/AWS_OP_ERR
 */
AWS_MQTT_API int aws_rate_limiter_token_bucket_init(
    struct aws_rate_limiter_token_bucket *limiter,
    const struct aws_rate_limiter_token_bucket_options *options);

/**
 * Resets a token-bucket-based rate limiter
 *
 * @param limiter rate limiter to reset
 */
AWS_MQTT_API void aws_rate_limiter_token_bucket_reset(struct aws_rate_limiter_token_bucket *limiter);

/**
 * Queries if the token bucket has a number of tokens currently available
 *
 * @param limiter token bucket rate limiter to query, non-const because token count is lazily updated
 * @param token_count how many tokens to check for
 * @return true if that many tokens are available, false otherwise
 */
AWS_MQTT_API bool aws_rate_limiter_token_bucket_can_take_tokens(
    struct aws_rate_limiter_token_bucket *limiter,
    uint64_t token_count);

/**
 * Takes a number of tokens from the token bucket rate limiter
 *
 * @param limiter token bucket rate limiter to take from
 * @param token_count how many tokens to take
 * @return AWS_OP_SUCCESS if there were that many tokens available, AWS_OP_ERR otherwise
 */
AWS_MQTT_API int aws_rate_limiter_token_bucket_take_tokens(
    struct aws_rate_limiter_token_bucket *limiter,
    uint64_t token_count);

/**
 * Queries a token-bucket-based rate limiter for how long, in nanoseconds, until a specified amount of tokens will
 * be available.
 *
 * @param limiter token-bucket-based rate limiter to query
 * @param token_count how many tokens need to be avilable
 * @return how long the caller must wait, in nanoseconds, before that many tokens are available
 */
AWS_MQTT_API uint64_t aws_rate_limiter_token_bucket_compute_wait_for_tokens(
    struct aws_rate_limiter_token_bucket *limiter,
    uint64_t token_count);

AWS_EXTERN_C_END

#endif /* AWS_RATE_LIMITERS_H */
