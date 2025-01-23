/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/credentials.h>

#include <aws/auth/private/credentials_utils.h>
#include <aws/common/clock.h>
#include <aws/common/mutex.h>
#include <aws/common/time.h>

#include <inttypes.h>

/*

 ToDo: credentials expiration environment overrides

AWS_STATIC_STRING_FROM_LITERAL(s_credential_expiration_env_var, "AWS_CREDENTIAL_EXPIRATION");

*/

#define REFRESH_CREDENTIALS_EARLY_DURATION_SECONDS 60 * 5 /* 5 minutes */

struct aws_credentials_provider_cached {
    struct aws_credentials_provider *source;
    struct aws_credentials *cached_credentials;
    struct aws_mutex lock;
    uint64_t refresh_interval_in_ns;
    uint64_t next_refresh_time;
    aws_io_clock_fn *high_res_clock_fn;
    aws_io_clock_fn *system_clock_fn;
    struct aws_linked_list pending_queries;
};

static void s_aws_credentials_query_list_notify_and_clean_up(
    struct aws_linked_list *query_list,
    struct aws_allocator *allocator,
    struct aws_credentials *credentials,
    int error_code) {

    while (!aws_linked_list_empty(query_list)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(query_list);
        struct aws_credentials_query *query = AWS_CONTAINER_OF(node, struct aws_credentials_query, node);
        query->callback(credentials, error_code, query->user_data);
        aws_credentials_query_clean_up(query);
        aws_mem_release(allocator, query);
    }
}

static void s_swap_cached_credentials(
    struct aws_credentials_provider *provider,
    struct aws_credentials *new_credentials) {
    struct aws_credentials_provider_cached *cached_provider = provider->impl;

    aws_credentials_release(cached_provider->cached_credentials);
    cached_provider->cached_credentials = new_credentials;
    if (cached_provider->cached_credentials != NULL) {
        aws_credentials_acquire(cached_provider->cached_credentials);

        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Cached credentials provider succesfully sourced credentials on refresh",
            (void *)provider);
    } else {
        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Cached credentials provider was unable to source credentials on refresh",
            (void *)provider);
    }
}

static void s_cached_credentials_provider_get_credentials_async_callback(
    struct aws_credentials *credentials,
    int error_code,
    void *user_data) {

    struct aws_credentials_provider *provider = user_data;
    struct aws_credentials_provider_cached *impl = provider->impl;

    aws_mutex_lock(&impl->lock);

    /*
     * Move pending queries so that we can do notifications outside the lock
     */
    struct aws_linked_list pending_queries;
    aws_linked_list_init(&pending_queries);

    aws_linked_list_swap_contents(&pending_queries, &impl->pending_queries);

    uint64_t next_refresh_time_in_ns = UINT64_MAX;

    uint64_t high_res_now = 0;
    if (!impl->high_res_clock_fn(&high_res_now)) {
        if (impl->refresh_interval_in_ns > 0) {
            next_refresh_time_in_ns = high_res_now + impl->refresh_interval_in_ns;
        }

        uint64_t credentials_expiration_timepoint_seconds = UINT64_MAX;
        if (credentials != NULL) {
            credentials_expiration_timepoint_seconds = aws_credentials_get_expiration_timepoint_seconds(credentials);
        }

        /*
         * If the sourced credentials have an explicit expiration time, we should always use that time
         * rather than the much cruder, mechanical refresh setting on the caching wrapper.
         */
        if (credentials_expiration_timepoint_seconds < UINT64_MAX) {
            uint64_t system_now = 0;
            if (!impl->system_clock_fn(&system_now)) {

                uint64_t system_now_seconds =
                    aws_timestamp_convert(system_now, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_SECS, NULL);
                if (credentials_expiration_timepoint_seconds >=
                    system_now_seconds + REFRESH_CREDENTIALS_EARLY_DURATION_SECONDS) {
                    next_refresh_time_in_ns = high_res_now;
                    next_refresh_time_in_ns += aws_timestamp_convert(
                        credentials_expiration_timepoint_seconds - system_now_seconds -
                            REFRESH_CREDENTIALS_EARLY_DURATION_SECONDS,
                        AWS_TIMESTAMP_SECS,
                        AWS_TIMESTAMP_NANOS,
                        NULL);
                } else {
                    next_refresh_time_in_ns = high_res_now;
                }
            }
        }
    }

    impl->next_refresh_time = next_refresh_time_in_ns;

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p) Cached credentials provider next refresh time set to %" PRIu64,
        (void *)provider,
        impl->next_refresh_time);

    s_swap_cached_credentials(provider, credentials);

    aws_mutex_unlock(&impl->lock);

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p) Cached credentials provider notifying pending queries of new credentials",
        (void *)provider);

    s_aws_credentials_query_list_notify_and_clean_up(&pending_queries, provider->allocator, credentials, error_code);
}

static int s_cached_credentials_provider_get_credentials_async(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_credentials_provider_cached *impl = provider->impl;

    uint64_t current_time = 0;
    impl->high_res_clock_fn(&current_time);

    bool should_submit_query = false;
    bool perform_callback = false;
    struct aws_credentials *credentials = NULL;

    aws_mutex_lock(&impl->lock);

    if (impl->cached_credentials != NULL && current_time < impl->next_refresh_time) {
        perform_callback = true;
        credentials = impl->cached_credentials;
        aws_credentials_acquire(credentials);
    } else {
        struct aws_credentials_query *query =
            aws_mem_acquire(provider->allocator, sizeof(struct aws_credentials_query));
        if (query != NULL) {
            aws_credentials_query_init(query, provider, callback, user_data);
            should_submit_query = aws_linked_list_empty(&impl->pending_queries);
            aws_linked_list_push_back(&impl->pending_queries, &query->node);
        } else {
            perform_callback = true;
        }
    }

    aws_mutex_unlock(&impl->lock);

    if (should_submit_query) {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Cached credentials provider has expired credentials.  Requerying.",
            (void *)provider);

        aws_credentials_provider_get_credentials(
            impl->source, s_cached_credentials_provider_get_credentials_async_callback, provider);

    } else if (!perform_callback) {
        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Cached credentials provider has expired credentials.  Waiting on existing query.",
            (void *)provider);
    }

    if (perform_callback) {
        if (credentials != NULL) {
            AWS_LOGF_DEBUG(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) Cached credentials provider successfully sourced from cache",
                (void *)provider);
        } else {
            AWS_LOGF_DEBUG(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) Cached credentials provider failed to source credentials while skipping requery",
                (void *)provider);
        }
        callback(credentials, (credentials != NULL) ? AWS_ERROR_SUCCESS : aws_last_error(), user_data);
        aws_credentials_release(credentials);
    }

    return AWS_OP_SUCCESS;
}

static void s_cached_credentials_provider_destroy(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_cached *impl = provider->impl;
    if (impl == NULL) {
        return;
    }

    aws_credentials_provider_release(impl->source);

    /* Invoke our own shutdown callback */
    aws_credentials_provider_invoke_shutdown_callback(provider);

    if (impl->cached_credentials != NULL) {
        aws_credentials_release(impl->cached_credentials);
    }

    aws_mutex_clean_up(&impl->lock);

    aws_mem_release(provider->allocator, provider);
}

static struct aws_credentials_provider_vtable s_aws_credentials_provider_cached_vtable = {
    .get_credentials = s_cached_credentials_provider_get_credentials_async,
    .destroy = s_cached_credentials_provider_destroy,
};

struct aws_credentials_provider *aws_credentials_provider_new_cached(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_cached_options *options) {

    AWS_ASSERT(options->source != NULL);

    struct aws_credentials_provider *provider = NULL;
    struct aws_credentials_provider_cached *impl = NULL;

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_credentials_provider_cached));

    if (!provider) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);

    aws_credentials_provider_init_base(provider, allocator, &s_aws_credentials_provider_cached_vtable, impl);

    if (aws_mutex_init(&impl->lock)) {
        goto on_error;
    }

    aws_linked_list_init(&impl->pending_queries);

    impl->source = options->source;
    aws_credentials_provider_acquire(impl->source);

    if (options->refresh_time_in_milliseconds > 0) {
        impl->refresh_interval_in_ns = aws_timestamp_convert(
            options->refresh_time_in_milliseconds, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL);
    } else {
        /*
         * TODO: query AWS_CREDENTIAL_EXPIRATION for a refresh override
         *
         * This must be an ISO 8601 time interval which we don't have a parser for yet (one could be cobbled
         * together from the existing timestamp parser).  Does not seem important enough to get bogged down in atm.
         * Punting for now.
         */
        impl->refresh_interval_in_ns = 0;
    }

    if (options->high_res_clock_fn != NULL) {
        impl->high_res_clock_fn = options->high_res_clock_fn;
    } else {
        impl->high_res_clock_fn = &aws_high_res_clock_get_ticks;
    }

    if (options->system_clock_fn != NULL) {
        impl->system_clock_fn = options->system_clock_fn;
    } else {
        impl->system_clock_fn = &aws_sys_clock_get_ticks;
    }

    provider->shutdown_options = options->shutdown_options;

    return provider;

on_error:

    aws_credentials_provider_destroy(provider);

    return NULL;
}
