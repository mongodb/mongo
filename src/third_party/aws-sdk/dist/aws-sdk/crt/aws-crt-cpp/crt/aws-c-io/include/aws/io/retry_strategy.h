#ifndef AWS_IO_CLIENT_RETRY_STRATEGY_H
#define AWS_IO_CLIENT_RETRY_STRATEGY_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/exports.h>

#include <aws/common/atomics.h>
#include <aws/common/byte_buf.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_retry_strategy;
struct aws_retry_token;
struct aws_event_loop_group;

/**
 * Invoked upon the acquisition, or failure to acquire a retry token. This function will always be invoked if and only
 * if aws_retry_strategy_acquire_retry_token() returns AWS_OP_SUCCESS. It will never be invoked synchronously from
 * aws_retry_strategy_acquire_retry_token(). Token will always be NULL if error_code is non-zero, and vice-versa. If
 * token is non-null, it will have a reference count of 1, and you must call aws_retry_token_release() on it later. See
 * the comments for aws_retry_strategy_on_retry_ready_fn for more info.
 */
typedef void(aws_retry_strategy_on_retry_token_acquired_fn)(
    struct aws_retry_strategy *retry_strategy,
    int error_code,
    struct aws_retry_token *token,
    void *user_data);

/**
 * Invoked after a successful call to aws_retry_strategy_schedule_retry(). This function will always be invoked if and
 * only if aws_retry_strategy_schedule_retry() returns AWS_OP_SUCCESS. It will never be invoked synchronously from
 * aws_retry_strategy_schedule_retry(). After attempting the operation, either call aws_retry_strategy_schedule_retry()
 * with an aws_retry_error_type or call aws_retry_token_record_success() and then release the token via.
 * aws_retry_token_release().
 */
typedef void(aws_retry_strategy_on_retry_ready_fn)(struct aws_retry_token *token, int error_code, void *user_data);

/**
 * Optional function to supply your own generate random implementation
 */
typedef uint64_t(aws_generate_random_fn)(void *user_data);

enum aws_retry_error_type {
    /** This is a connection level error such as a socket timeout, socket connect error, tls negotiation timeout etc...
     * Typically these should never be applied for non-idempotent request types since in this scenario, it's impossible
     * to know whether the operation had a side effect on the server. */
    AWS_RETRY_ERROR_TYPE_TRANSIENT,
    /** This is an error where the server explicitly told the client to back off, such as a 429 or 503 Http error. */
    AWS_RETRY_ERROR_TYPE_THROTTLING,
    /** This is a server error that isn't explicitly throttling but is considered by the client
     * to be something that should be retried. */
    AWS_RETRY_ERROR_TYPE_SERVER_ERROR,
    /** Doesn't count against any budgets. This could be something like a 401 challenge in Http. */
    AWS_RETRY_ERROR_TYPE_CLIENT_ERROR,
};

struct aws_retry_strategy_vtable {
    void (*destroy)(struct aws_retry_strategy *retry_strategy);
    int (*acquire_token)(
        struct aws_retry_strategy *retry_strategy,
        const struct aws_byte_cursor *partition_id,
        aws_retry_strategy_on_retry_token_acquired_fn *on_acquired,
        void *user_data,
        uint64_t timeout_ms);
    int (*schedule_retry)(
        struct aws_retry_token *token,
        enum aws_retry_error_type error_type,
        aws_retry_strategy_on_retry_ready_fn *retry_ready,
        void *user_data);
    int (*record_success)(struct aws_retry_token *token);
    void (*release_token)(struct aws_retry_token *token);
};

struct aws_retry_strategy {
    struct aws_allocator *allocator;
    struct aws_retry_strategy_vtable *vtable;
    struct aws_atomic_var ref_count;
    void *impl;
};

struct aws_retry_token {
    struct aws_allocator *allocator;
    struct aws_retry_strategy *retry_strategy;
    struct aws_atomic_var ref_count;
    void *impl;
};

/**
 * Jitter mode for exponential backoff.
 *
 * For a great writeup on these options see:
 * https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/
 */
enum aws_exponential_backoff_jitter_mode {
    /* Uses AWS_EXPONENTIAL_BACKOFF_JITTER_FULL */
    AWS_EXPONENTIAL_BACKOFF_JITTER_DEFAULT,
    AWS_EXPONENTIAL_BACKOFF_JITTER_NONE,
    AWS_EXPONENTIAL_BACKOFF_JITTER_FULL,
    AWS_EXPONENTIAL_BACKOFF_JITTER_DECORRELATED,
};

/**
 * Options for exponential backoff retry strategy. el_group must be set, any other option, if set to 0 will signify
 * "use defaults"
 */
struct aws_exponential_backoff_retry_options {
    /* Event loop group to use for scheduling tasks. */
    struct aws_event_loop_group *el_group;
    /* Max retries to allow. The default value is 10 */
    size_t max_retries;
    /* Scaling factor to add for the backoff. Default is 500ms */
    uint32_t backoff_scale_factor_ms;
    /* Max retry backoff in seconds. Default is 20 seconds */
    uint32_t max_backoff_secs;
    /** Jitter mode to use, see comments for aws_exponential_backoff_jitter_mode.
     * Default is AWS_EXPONENTIAL_BACKOFF_JITTER_DEFAULT */
    enum aws_exponential_backoff_jitter_mode jitter_mode;

    /** Deprecated. Use generate_random_impl instead
     * By default this will be set to use aws_device_random. If you want something else, set it here.
     * */
    uint64_t (*generate_random)(void);

    /*
     * By default this will be set to use aws_device_random. If you want something else, set it here.
     */
    aws_generate_random_fn *generate_random_impl;
    /**
     * Optional user data for the generate random generate_random_impl.
     */
    void *generate_random_user_data;

    /**
     * Optional shutdown callback that gets invoked, with appropriate user data,
     * when the resources used by the retry_strategy are no longer in use.
     */
    const struct aws_shutdown_callback_options *shutdown_options;
};

struct aws_no_retry_options {
    /**
     * Optional shutdown callback that gets invoked, with appropriate user data,
     * when the resources used by the retry_strategy are no longer in use.
     */
    const struct aws_shutdown_callback_options *shutdown_options;
};

struct aws_standard_retry_options {
    struct aws_exponential_backoff_retry_options backoff_retry_options;
    /** capacity for partitions. Defaults to 500 */
    size_t initial_bucket_capacity;
};

AWS_EXTERN_C_BEGIN
/**
 * Acquire a reference count on retry_strategy.
 */
AWS_IO_API void aws_retry_strategy_acquire(struct aws_retry_strategy *retry_strategy);
/**
 * Releases a reference count on retry_strategy.
 */
AWS_IO_API void aws_retry_strategy_release(struct aws_retry_strategy *retry_strategy);
/**
 * Attempts to acquire a retry token for use with retries. On success, on_acquired will be invoked when a token is
 * available, or an error will be returned if the timeout expires. partition_id identifies operations that should be
 * grouped together. This allows for more sophisticated strategies such as AIMD and circuit breaker patterns. Pass NULL
 * to use the global partition.
 */
AWS_IO_API int aws_retry_strategy_acquire_retry_token(
    struct aws_retry_strategy *retry_strategy,
    const struct aws_byte_cursor *partition_id,
    aws_retry_strategy_on_retry_token_acquired_fn *on_acquired,
    void *user_data,
    uint64_t timeout_ms);

/**
 * Schedules a retry based on the backoff and token based strategies. retry_ready is invoked when the retry is either
 * ready for execution or if it has been canceled due to application shutdown.
 *
 * This function can return an error to reject the retry attempt if, for example, a circuit breaker has opened. If this
 * occurs users should fail their calls back to their callers.
 *
 * error_type is used for book keeping. See the comments above for aws_retry_error_type.
 */
AWS_IO_API int aws_retry_strategy_schedule_retry(
    struct aws_retry_token *token,
    enum aws_retry_error_type error_type,
    aws_retry_strategy_on_retry_ready_fn *retry_ready,
    void *user_data);
/**
 * Records a successful retry. This is used for making future decisions to open up token buckets, AIMD breakers etc...
 * some strategies such as exponential backoff will ignore this, but you should always call it after a successful
 * operation or your system will never recover during an outage.
 */
AWS_IO_API int aws_retry_token_record_success(struct aws_retry_token *token);

/**
 * Increments reference count for token. This should be called any time you seat the token to a pointer you own.
 */
AWS_IO_API void aws_retry_token_acquire(struct aws_retry_token *token);

/**
 * Releases the reference count for token. This should always be invoked after either calling
 * aws_retry_strategy_schedule_retry() and failing, or after calling aws_retry_token_record_success().
 */
AWS_IO_API void aws_retry_token_release(struct aws_retry_token *token);
/**
 * Creates a retry strategy using exponential backoff. This strategy does not perform any bookkeeping on error types and
 * success. There is no circuit breaker functionality in here. See the comments above for
 * aws_exponential_backoff_retry_options.
 */
AWS_IO_API struct aws_retry_strategy *aws_retry_strategy_new_exponential_backoff(
    struct aws_allocator *allocator,
    const struct aws_exponential_backoff_retry_options *config);

/**
 * This is a retry implementation that cuts off traffic if it's
 * detected that an endpoint partition is having availability
 * problems. This is necessary to keep from making outages worse
 * by scheduling work that's unlikely to succeed yet increases
 * load on an already ailing system.
 *
 * We do this by creating a bucket for each partition. A partition
 * is an arbitrary specifier. It can be anything: a region, a service,
 * a combination of region and service, a literal dns name.... doesn't matter.
 *
 * Each bucket has a budget for maximum allowed retries. Different types of events
 * carry different weights. Things that indicate an unhealthy partition such as
 * transient errors (timeouts, unhealthy connection etc...) cost more.
 * A retry for any other reason (service sending a 5xx response code) cost a bit less.
 * When a retry is attempted this capacity is leased out to the retry. On success it is
 * released back to the capacity pool. On failure, it remains leased.
 * Operations that succeed without a retry slowly restore the capacity pool.
 *
 * If a partition runs out of capacity it is assumed unhealthy and retries will be blocked
 * until capacity returns to the pool. To prevent a partition from staying unhealthy after
 * an outage has recovered, new requests that succeed without a retry will increase the capacity
 * slowly ( a new request gets a payback lease of 1, but the lease is never actually deducted from the capacity pool).
 */
AWS_IO_API struct aws_retry_strategy *aws_retry_strategy_new_standard(
    struct aws_allocator *allocator,
    const struct aws_standard_retry_options *config);

/**
 * This retry strategy is used to disable retries. Passed config can be null.
 * Calling `aws_retry_strategy_acquire_retry_token` will raise error `AWS_IO_RETRY_PERMISSION_DENIED`.
 * Calling any function apart from the `aws_retry_strategy_acquire_retry_token` and `aws_retry_strategy_release` will
 * result in a fatal error.
 */
AWS_IO_API struct aws_retry_strategy *aws_retry_strategy_new_no_retry(
    struct aws_allocator *allocator,
    const struct aws_no_retry_options *config);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_CLIENT_RETRY_STRATEGY_H */
