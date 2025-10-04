/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/array_list.h>
#include <aws/common/clock.h>
#include <aws/common/hash_table.h>
#include <aws/common/logging.h>
#include <aws/http/connection.h>
#include <aws/http/connection_manager.h>
#include <aws/http/request_response.h>
#include <aws/io/channel.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>

#include <aws/http/http2_stream_manager.h>
#include <aws/http/private/http2_stream_manager_impl.h>
#include <aws/http/private/request_response_impl.h>
#include <aws/http/status_code.h>

#include <inttypes.h>

#ifdef _MSC_VER
#    pragma warning(disable : 4204) /* non-constant aggregate initializer */
#endif

/* Apple toolchains such as xcode and swiftpm define the DEBUG symbol. undef it here so we can actually use the token */
#undef DEBUG

#define STREAM_MANAGER_LOGF(level, stream_manager, text, ...)                                                          \
    AWS_LOGF_##level(AWS_LS_HTTP_STREAM_MANAGER, "id=%p: " text, (void *)(stream_manager), __VA_ARGS__)
#define STREAM_MANAGER_LOG(level, stream_manager, text) STREAM_MANAGER_LOGF(level, stream_manager, "%s", text)

/* 3 seconds */
static const size_t s_default_ping_timeout_ms = 3000;

static void s_stream_manager_start_destroy(struct aws_http2_stream_manager *stream_manager);
static void s_aws_http2_stream_manager_build_transaction_synced(struct aws_http2_stream_management_transaction *work);
static void s_aws_http2_stream_manager_execute_transaction(struct aws_http2_stream_management_transaction *work);

static struct aws_h2_sm_pending_stream_acquisition *s_new_pending_stream_acquisition(
    struct aws_allocator *allocator,
    const struct aws_http_make_request_options *options,
    aws_http2_stream_manager_on_stream_acquired_fn *callback,
    void *user_data) {
    struct aws_h2_sm_pending_stream_acquisition *pending_stream_acquisition =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_h2_sm_pending_stream_acquisition));

    /* Copy the options and keep the underlying message alive */
    pending_stream_acquisition->options = *options;
    pending_stream_acquisition->request = options->request;
    aws_http_message_acquire(pending_stream_acquisition->request);
    pending_stream_acquisition->callback = callback;
    pending_stream_acquisition->user_data = user_data;
    pending_stream_acquisition->allocator = allocator;
    return pending_stream_acquisition;
}

static void s_pending_stream_acquisition_destroy(
    struct aws_h2_sm_pending_stream_acquisition *pending_stream_acquisition) {
    if (pending_stream_acquisition == NULL) {
        return;
    }
    if (pending_stream_acquisition->request) {
        aws_http_message_release(pending_stream_acquisition->request);
    }
    aws_mem_release(pending_stream_acquisition->allocator, pending_stream_acquisition);
}

static void s_lock_synced_data(struct aws_http2_stream_manager *stream_manager) {
    int err = aws_mutex_lock(&stream_manager->synced_data.lock);
    AWS_ASSERT(!err && "lock failed");
    (void)err;
}

static void s_unlock_synced_data(struct aws_http2_stream_manager *stream_manager) {
    int err = aws_mutex_unlock(&stream_manager->synced_data.lock);
    AWS_ASSERT(!err && "unlock failed");
    (void)err;
}

static void s_sm_log_stats_synced(struct aws_http2_stream_manager *stream_manager) {
    STREAM_MANAGER_LOGF(
        TRACE,
        stream_manager,
        "Stream manager internal counts status: "
        "connection acquiring=%zu, streams opening=%zu, pending make request count=%zu, pending acquisition count=%zu, "
        "holding connections count=%zu",
        stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_CONNECTIONS_ACQUIRING],
        stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_OPEN_STREAM],
        stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_MAKE_REQUESTS],
        stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_ACQUISITION],
        stream_manager->synced_data.holding_connections_count);
}

/* The count acquire and release all needs to be invoked helding the lock */
static void s_sm_count_increase_synced(
    struct aws_http2_stream_manager *stream_manager,
    enum aws_sm_count_type count_type,
    size_t num) {
    stream_manager->synced_data.internal_refcount_stats[count_type] += num;
    for (size_t i = 0; i < num; i++) {
        aws_ref_count_acquire(&stream_manager->internal_ref_count);
    }
}

static void s_sm_count_decrease_synced(
    struct aws_http2_stream_manager *stream_manager,
    enum aws_sm_count_type count_type,
    size_t num) {
    stream_manager->synced_data.internal_refcount_stats[count_type] -= num;
    for (size_t i = 0; i < num; i++) {
        aws_ref_count_release(&stream_manager->internal_ref_count);
    }
}

static void s_aws_stream_management_transaction_init(
    struct aws_http2_stream_management_transaction *work,
    struct aws_http2_stream_manager *stream_manager) {
    AWS_ZERO_STRUCT(*work);
    aws_linked_list_init(&work->pending_make_requests);
    work->stream_manager = stream_manager;
    work->allocator = stream_manager->allocator;
    aws_ref_count_acquire(&stream_manager->internal_ref_count);
}

static void s_aws_stream_management_transaction_clean_up(struct aws_http2_stream_management_transaction *work) {
    (void)work;
    AWS_ASSERT(aws_linked_list_empty(&work->pending_make_requests));
    aws_ref_count_release(&work->stream_manager->internal_ref_count);
}

static struct aws_h2_sm_connection *s_get_best_sm_connection_from_set(struct aws_random_access_set *set) {
    /* Use the best two algorithm */
    int errored = AWS_ERROR_SUCCESS;
    struct aws_h2_sm_connection *sm_connection_a = NULL;
    errored = aws_random_access_set_random_get_ptr(set, (void **)&sm_connection_a);
    struct aws_h2_sm_connection *sm_connection_b = NULL;
    errored |= aws_random_access_set_random_get_ptr(set, (void **)&sm_connection_b);
    struct aws_h2_sm_connection *chosen_connection =
        sm_connection_a->num_streams_assigned > sm_connection_b->num_streams_assigned ? sm_connection_b
                                                                                      : sm_connection_a;
    return errored == AWS_ERROR_SUCCESS ? chosen_connection : NULL;
    (void)errored;
}

/* helper function for building the transaction: Try to assign connection for a pending stream acquisition */
/* *_synced should only be called with LOCK HELD or from another synced function */
static void s_sm_try_assign_connection_to_pending_stream_acquisition_synced(
    struct aws_http2_stream_manager *stream_manager,
    struct aws_h2_sm_pending_stream_acquisition *pending_stream_acquisition) {

    AWS_ASSERT(pending_stream_acquisition->sm_connection == NULL);
    int errored = 0;
    if (aws_random_access_set_get_size(&stream_manager->synced_data.ideal_available_set)) {
        /**
         * Try assigning to connection from ideal set
         */
        struct aws_h2_sm_connection *chosen_connection =
            s_get_best_sm_connection_from_set(&stream_manager->synced_data.ideal_available_set);
        AWS_ASSERT(chosen_connection);
        pending_stream_acquisition->sm_connection = chosen_connection;
        chosen_connection->num_streams_assigned++;

        STREAM_MANAGER_LOGF(
            DEBUG,
            stream_manager,
            "Picking connection:%p for acquisition:%p. Streams assigned to the connection=%" PRIu32 "",
            (void *)chosen_connection->connection,
            (void *)pending_stream_acquisition,
            chosen_connection->num_streams_assigned);
        /* Check if connection is still available or ideal, and move it if it's not */
        if (chosen_connection->num_streams_assigned >= chosen_connection->max_concurrent_streams) {
            /* It becomes not available for new streams any more, remove it from the set, but still alive (streams
             * created will track the lifetime) */
            chosen_connection->state = AWS_H2SMCST_FULL;
            errored |=
                aws_random_access_set_remove(&stream_manager->synced_data.ideal_available_set, chosen_connection);
            STREAM_MANAGER_LOGF(
                DEBUG,
                stream_manager,
                "connection:%p reaches max concurrent streams limits. "
                "Connection max limits=%" PRIu32 ". Moving it out of available connections.",
                (void *)chosen_connection->connection,
                chosen_connection->max_concurrent_streams);
        } else if (chosen_connection->num_streams_assigned >= stream_manager->ideal_concurrent_streams_per_connection) {
            /* It meets the ideal limit, but still available for new streams, move it to the nonidea-available set */
            errored |=
                aws_random_access_set_remove(&stream_manager->synced_data.ideal_available_set, chosen_connection);
            bool added = false;
            errored |= aws_random_access_set_add(
                &stream_manager->synced_data.nonideal_available_set, chosen_connection, &added);
            errored |= !added;
            chosen_connection->state = AWS_H2SMCST_NEARLY_FULL;
            STREAM_MANAGER_LOGF(
                DEBUG,
                stream_manager,
                "connection:%p reaches ideal concurrent streams limits. Ideal limits=%zu. Moving it to nonlimited set.",
                (void *)chosen_connection->connection,
                stream_manager->ideal_concurrent_streams_per_connection);
        }
    } else if (stream_manager->synced_data.holding_connections_count == stream_manager->max_connections) {
        /**
         * Try assigning to connection from nonideal available set.
         *
         * Note that we do not assign to nonideal connections until we're holding all the connections we can ever
         * possibly get. This way, we don't overfill the first connections we get our hands on.
         */

        if (aws_random_access_set_get_size(&stream_manager->synced_data.nonideal_available_set)) {
            struct aws_h2_sm_connection *chosen_connection =
                s_get_best_sm_connection_from_set(&stream_manager->synced_data.nonideal_available_set);
            AWS_ASSERT(chosen_connection);
            pending_stream_acquisition->sm_connection = chosen_connection;
            chosen_connection->num_streams_assigned++;

            STREAM_MANAGER_LOGF(
                DEBUG,
                stream_manager,
                "Picking connection:%p for acquisition:%p. Streams assigned to the connection=%" PRIu32 "",
                (void *)chosen_connection->connection,
                (void *)pending_stream_acquisition,
                chosen_connection->num_streams_assigned);

            if (chosen_connection->num_streams_assigned >= chosen_connection->max_concurrent_streams) {
                /* It becomes not available for new streams any more, remove it from the set, but still alive (streams
                 * created will track the lifetime) */
                chosen_connection->state = AWS_H2SMCST_FULL;
                errored |= aws_random_access_set_remove(
                    &stream_manager->synced_data.nonideal_available_set, chosen_connection);
                STREAM_MANAGER_LOGF(
                    DEBUG,
                    stream_manager,
                    "connection %p reaches max concurrent streams limits. "
                    "Connection max limits=%" PRIu32 ". Moving it out of available connections.",
                    (void *)chosen_connection->connection,
                    chosen_connection->max_concurrent_streams);
            }
        }
    }
    AWS_ASSERT(errored == 0 && "random access set went wrong");
    (void)errored;
}

/* NOTE: never invoke with lock held */
static void s_finish_pending_stream_acquisitions_list_helper(
    struct aws_http2_stream_manager *stream_manager,
    struct aws_linked_list *pending_stream_acquisitions,
    int error_code) {
    while (!aws_linked_list_empty(pending_stream_acquisitions)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(pending_stream_acquisitions);
        struct aws_h2_sm_pending_stream_acquisition *pending_stream_acquisition =
            AWS_CONTAINER_OF(node, struct aws_h2_sm_pending_stream_acquisition, node);
        /* Make sure no connection assigned. */
        AWS_ASSERT(pending_stream_acquisition->sm_connection == NULL);
        if (pending_stream_acquisition->callback) {
            pending_stream_acquisition->callback(NULL, error_code, pending_stream_acquisition->user_data);
        }
        STREAM_MANAGER_LOGF(
            DEBUG,
            stream_manager,
            "acquisition:%p failed with error: %d(%s)",
            (void *)pending_stream_acquisition,
            error_code,
            aws_error_str(error_code));
        s_pending_stream_acquisition_destroy(pending_stream_acquisition);
    }
}

/* This is scheduled to run on a separate event loop to finish pending acquisition asynchronously */
static void s_finish_pending_stream_acquisitions_task(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)status;
    struct aws_http2_stream_manager *stream_manager = arg;
    STREAM_MANAGER_LOG(TRACE, stream_manager, "Stream Manager final task runs");
    struct aws_http2_stream_management_transaction work;
    struct aws_linked_list pending_stream_acquisitions;
    aws_linked_list_init(&pending_stream_acquisitions);
    s_aws_stream_management_transaction_init(&work, stream_manager);
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(stream_manager);
        AWS_ASSERT(stream_manager->synced_data.state == AWS_H2SMST_DESTROYING);
        /* swap list to avoid callback with lock held. */
        aws_linked_list_swap_contents(
            &pending_stream_acquisitions, &stream_manager->synced_data.pending_stream_acquisitions);
        /* After the callbacks invoked, now we can update the count */
        s_sm_count_decrease_synced(
            stream_manager,
            AWS_SMCT_PENDING_ACQUISITION,
            stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_ACQUISITION]);
        s_aws_http2_stream_manager_build_transaction_synced(&work);
        s_unlock_synced_data(stream_manager);
    } /* END CRITICAL SECTION */
    s_finish_pending_stream_acquisitions_list_helper(
        stream_manager, &pending_stream_acquisitions, AWS_ERROR_HTTP_STREAM_MANAGER_SHUTTING_DOWN);
    aws_mem_release(stream_manager->allocator, task);
    s_aws_http2_stream_manager_execute_transaction(&work);
}

/* helper function for building the transaction: how many new connections we should request */
static void s_check_new_connections_needed_synced(struct aws_http2_stream_management_transaction *work) {
    struct aws_http2_stream_manager *stream_manager = work->stream_manager;
    /* The ideal new connection we need to fit all the pending stream acquisitions */
    size_t ideal_new_connection_count =
        stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_ACQUISITION] /
        stream_manager->ideal_concurrent_streams_per_connection;
    /* Rounding up */
    if (stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_ACQUISITION] %
        stream_manager->ideal_concurrent_streams_per_connection) {
        ++ideal_new_connection_count;
    }
    /* The ideal new connections sub the number of connections we are acquiring to avoid the async acquiring */
    work->new_connections = aws_sub_size_saturating(
        ideal_new_connection_count,
        stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_CONNECTIONS_ACQUIRING]);
    /* The real number we can have is the min of how many more we can still have and how many we need */
    size_t new_connections_available =
        stream_manager->max_connections - stream_manager->synced_data.holding_connections_count -
        stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_CONNECTIONS_ACQUIRING];
    work->new_connections = aws_min_size(new_connections_available, work->new_connections);
    /* Update the number of connections we acquiring */
    s_sm_count_increase_synced(stream_manager, AWS_SMCT_CONNECTIONS_ACQUIRING, work->new_connections);
    STREAM_MANAGER_LOGF(
        DEBUG,
        stream_manager,
        "number of acquisition that waiting for connections to use=%zu. connection acquiring=%zu, connection held=%zu, "
        "max connection=%zu",
        stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_ACQUISITION],
        stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_CONNECTIONS_ACQUIRING],
        stream_manager->synced_data.holding_connections_count,
        stream_manager->max_connections);
}

/**
 * It can be invoked from:
 * - User release last refcount of stream manager
 * - User acquires stream from stream manager
 * - Connection acquired callback from connection manager
 * - Stream completed callback from HTTP
 */
/* *_synced should only be called with LOCK HELD or from another synced function */
static void s_aws_http2_stream_manager_build_transaction_synced(struct aws_http2_stream_management_transaction *work) {
    struct aws_http2_stream_manager *stream_manager = work->stream_manager;
    if (stream_manager->synced_data.state == AWS_H2SMST_READY) {

        /* Steps 1: Pending acquisitions of stream */
        while (!aws_linked_list_empty(&stream_manager->synced_data.pending_stream_acquisitions)) {
            struct aws_linked_list_node *node =
                aws_linked_list_pop_front(&stream_manager->synced_data.pending_stream_acquisitions);
            struct aws_h2_sm_pending_stream_acquisition *pending_stream_acquisition =
                AWS_CONTAINER_OF(node, struct aws_h2_sm_pending_stream_acquisition, node);
            s_sm_try_assign_connection_to_pending_stream_acquisition_synced(stream_manager, pending_stream_acquisition);
            if (pending_stream_acquisition->sm_connection == NULL) {
                /* Cannot find any connection, push it back to the front and break the loop */
                aws_linked_list_push_front(&stream_manager->synced_data.pending_stream_acquisitions, node);
                STREAM_MANAGER_LOGF(
                    DEBUG,
                    stream_manager,
                    "acquisition:%p cannot find any connection to use.",
                    (void *)pending_stream_acquisition);
                break;
            } else {
                /* found connection for the request. Move it to pending make requests and update the count */
                aws_linked_list_push_back(&work->pending_make_requests, node);
                s_sm_count_decrease_synced(stream_manager, AWS_SMCT_PENDING_ACQUISITION, 1);
                s_sm_count_increase_synced(stream_manager, AWS_SMCT_PENDING_MAKE_REQUESTS, 1);
            }
        }

        /* Step 2: Check for new connections needed */
        if (stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_ACQUISITION]) {
            s_check_new_connections_needed_synced(work);
        }

    } else {
        /* Stream manager is shutting down */
        if (stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_ACQUISITION] &&
            !stream_manager->synced_data.finish_pending_stream_acquisitions_task_scheduled) {
            /* schedule a task to finish the pending acquisitions if there doesn't have one and needed */
            stream_manager->finish_pending_stream_acquisitions_task_event_loop =
                aws_event_loop_group_get_next_loop(stream_manager->bootstrap->event_loop_group);
            struct aws_task *finish_pending_stream_acquisitions_task =
                aws_mem_calloc(stream_manager->allocator, 1, sizeof(struct aws_task));
            aws_task_init(
                finish_pending_stream_acquisitions_task,
                s_finish_pending_stream_acquisitions_task,
                stream_manager,
                "sm_finish_pending_stream_acquisitions");
            aws_event_loop_schedule_task_now(
                stream_manager->finish_pending_stream_acquisitions_task_event_loop,
                finish_pending_stream_acquisitions_task);
            stream_manager->synced_data.finish_pending_stream_acquisitions_task_scheduled = true;
        }
    }
    s_sm_log_stats_synced(stream_manager);
}

static void s_on_ping_complete(
    struct aws_http_connection *http2_connection,
    uint64_t round_trip_time_ns,
    int error_code,
    void *user_data) {

    (void)http2_connection;
    struct aws_h2_sm_connection *sm_connection = user_data;
    if (error_code) {
        goto done;
    }
    if (!sm_connection->connection) {
        goto done;
    }
    AWS_ASSERT(aws_channel_thread_is_callers_thread(aws_http_connection_get_channel(sm_connection->connection)));
    STREAM_MANAGER_LOGF(
        TRACE,
        sm_connection->stream_manager,
        "PING ACK received for connection: %p. Round trip time in ns is: %" PRIu64 ".",
        (void *)sm_connection->connection,
        round_trip_time_ns);
    sm_connection->thread_data.ping_received = true;

done:
    /* Release refcount held for ping complete */
    aws_ref_count_release(&sm_connection->ref_count);
}

static void s_connection_ping_timeout_task(struct aws_channel_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    (void)status;
    struct aws_h2_sm_connection *sm_connection = arg;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }
    if (!sm_connection->connection) {
        /* The connection has been released before timeout happens, just release the refcount */
        goto done;
    }
    AWS_ASSERT(aws_channel_thread_is_callers_thread(aws_http_connection_get_channel(sm_connection->connection)));
    if (!sm_connection->thread_data.ping_received) {
        /* Timeout happened */
        STREAM_MANAGER_LOGF(
            ERROR,
            sm_connection->stream_manager,
            "ping timeout detected for connection: %p, closing connection.",
            (void *)sm_connection->connection);

        aws_http_connection_close(sm_connection->connection);
    } else {
        struct aws_channel *channel = aws_http_connection_get_channel(sm_connection->connection);
        /* acquire a refcount for next set of tasks to run */
        aws_ref_count_acquire(&sm_connection->ref_count);
        aws_channel_schedule_task_future(
            channel, &sm_connection->ping_task, sm_connection->thread_data.next_ping_task_time);
    }
done:
    /* Release refcount for current set of tasks */
    aws_ref_count_release(&sm_connection->ref_count);
}

static void s_connection_ping_task(struct aws_channel_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    (void)status;
    struct aws_h2_sm_connection *sm_connection = arg;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        aws_ref_count_release(&sm_connection->ref_count);
        return;
    }
    if (!sm_connection->connection) {
        /* The connection has been released before ping task, just release the refcount */
        aws_ref_count_release(&sm_connection->ref_count);
        return;
    }
    AWS_ASSERT(aws_channel_thread_is_callers_thread(aws_http_connection_get_channel(sm_connection->connection)));

    STREAM_MANAGER_LOGF(
        TRACE, sm_connection->stream_manager, "Sending PING for connection: %p.", (void *)sm_connection->connection);
    aws_http2_connection_ping(sm_connection->connection, NULL, s_on_ping_complete, sm_connection);
    /* Acquire refcount for PING complete to be invoked. */
    aws_ref_count_acquire(&sm_connection->ref_count);
    sm_connection->thread_data.ping_received = false;

    /* schedule timeout task */
    struct aws_channel *channel = aws_http_connection_get_channel(sm_connection->connection);
    uint64_t current_time = 0;
    aws_channel_current_clock_time(channel, &current_time);
    sm_connection->thread_data.next_ping_task_time =
        current_time + sm_connection->stream_manager->connection_ping_period_ns;
    uint64_t timeout_time = current_time + sm_connection->stream_manager->connection_ping_timeout_ns;
    aws_channel_task_init(
        &sm_connection->ping_timeout_task,
        s_connection_ping_timeout_task,
        sm_connection,
        "Stream manager connection ping timeout task");
    /* keep the refcount for timeout task to run */
    aws_channel_schedule_task_future(channel, &sm_connection->ping_timeout_task, timeout_time);
}

static void s_sm_connection_destroy(void *user_data) {
    struct aws_h2_sm_connection *sm_connection = user_data;
    aws_mem_release(sm_connection->allocator, sm_connection);
}

static struct aws_h2_sm_connection *s_sm_connection_new(
    struct aws_http2_stream_manager *stream_manager,
    struct aws_http_connection *connection) {
    struct aws_h2_sm_connection *sm_connection =
        aws_mem_calloc(stream_manager->allocator, 1, sizeof(struct aws_h2_sm_connection));
    sm_connection->allocator = stream_manager->allocator;
    /* Max concurrent stream reached, we need to update the max for the sm_connection */
    struct aws_http2_setting out_settings[AWS_HTTP2_SETTINGS_COUNT];
    /* The setting id equals to the index plus one. */
    aws_http2_connection_get_remote_settings(connection, out_settings);
    uint32_t remote_max_con_streams = out_settings[AWS_HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS - 1].value;
    sm_connection->max_concurrent_streams =
        aws_min_u32((uint32_t)stream_manager->max_concurrent_streams_per_connection, remote_max_con_streams);
    sm_connection->connection = connection;
    sm_connection->stream_manager = stream_manager;
    sm_connection->state = AWS_H2SMCST_IDEAL;
    aws_ref_count_init(&sm_connection->ref_count, sm_connection, s_sm_connection_destroy);
    if (stream_manager->connection_ping_period_ns) {
        struct aws_channel *channel = aws_http_connection_get_channel(connection);
        uint64_t schedule_time = 0;
        aws_channel_current_clock_time(channel, &schedule_time);
        schedule_time += stream_manager->connection_ping_period_ns;
        aws_channel_task_init(
            &sm_connection->ping_task, s_connection_ping_task, sm_connection, "Stream manager connection ping task");
        /* Keep a refcount on sm_connection for the task to run. */
        aws_ref_count_acquire(&sm_connection->ref_count);
        aws_channel_schedule_task_future(channel, &sm_connection->ping_task, schedule_time);
    }
    return sm_connection;
}

static void s_sm_connection_release_connection(struct aws_h2_sm_connection *sm_connection) {
    AWS_ASSERT(sm_connection->num_streams_assigned == 0);
    if (sm_connection->connection) {
        /* Should only be invoked from the connection thread. */
        AWS_ASSERT(aws_channel_thread_is_callers_thread(aws_http_connection_get_channel(sm_connection->connection)));
        int error = aws_http_connection_manager_release_connection(
            sm_connection->stream_manager->connection_manager, sm_connection->connection);
        AWS_ASSERT(!error);
        (void)error;
        sm_connection->connection = NULL;
    }
    aws_ref_count_release(&sm_connection->ref_count);
}

static void s_sm_on_connection_acquired_failed_synced(
    struct aws_http2_stream_manager *stream_manager,
    struct aws_linked_list *stream_acquisitions_to_fail) {

    /* Once we failed to acquire a connection, we fail the stream acquisitions that cannot fit into the remaining
     * acquiring connections. */
    size_t num_can_fit = aws_mul_size_saturating(
        stream_manager->ideal_concurrent_streams_per_connection,
        stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_CONNECTIONS_ACQUIRING]);
    size_t num_to_fail = aws_sub_size_saturating(
        stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_ACQUISITION], num_can_fit);
    /* Get a list to fail instead of fail them with in the lock. */
    for (size_t i = 0; i < num_to_fail; i++) {
        struct aws_linked_list_node *node =
            aws_linked_list_pop_front(&stream_manager->synced_data.pending_stream_acquisitions);
        aws_linked_list_push_back(stream_acquisitions_to_fail, node);
    }
    s_sm_count_decrease_synced(stream_manager, AWS_SMCT_PENDING_ACQUISITION, num_to_fail);
}

static void s_sm_on_connection_acquired(struct aws_http_connection *connection, int error_code, void *user_data) {
    struct aws_http2_stream_manager *stream_manager = user_data;
    struct aws_http2_stream_management_transaction work;
    STREAM_MANAGER_LOGF(TRACE, stream_manager, "connection=%p acquired from connection manager", (void *)connection);
    int re_error = 0;
    int stream_fail_error_code = AWS_ERROR_SUCCESS;
    bool should_release_connection = false;
    struct aws_linked_list stream_acquisitions_to_fail;
    aws_linked_list_init(&stream_acquisitions_to_fail);
    s_aws_stream_management_transaction_init(&work, stream_manager);
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(stream_manager);
        s_sm_count_decrease_synced(stream_manager, AWS_SMCT_CONNECTIONS_ACQUIRING, 1);
        if (error_code || !connection) {
            STREAM_MANAGER_LOGF(
                ERROR,
                stream_manager,
                "connection acquired from connection manager failed, with error: %d(%s)",
                error_code,
                aws_error_str(error_code));
            s_sm_on_connection_acquired_failed_synced(stream_manager, &stream_acquisitions_to_fail);
            stream_fail_error_code = AWS_ERROR_HTTP_STREAM_MANAGER_CONNECTION_ACQUIRE_FAILURE;
        } else if (aws_http_connection_get_version(connection) != AWS_HTTP_VERSION_2) {
            STREAM_MANAGER_LOGF(
                ERROR,
                stream_manager,
                "Unexpected HTTP version acquired, release the connection=%p acquired immediately",
                (void *)connection);
            should_release_connection = true;
            s_sm_on_connection_acquired_failed_synced(stream_manager, &stream_acquisitions_to_fail);
            stream_fail_error_code = AWS_ERROR_HTTP_STREAM_MANAGER_UNEXPECTED_HTTP_VERSION;
        } else if (stream_manager->synced_data.state != AWS_H2SMST_READY) {
            STREAM_MANAGER_LOGF(
                DEBUG,
                stream_manager,
                "shutting down, release the connection=%p acquired immediately",
                (void *)connection);
            /* Release the acquired connection */
            should_release_connection = true;
        } else if (stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_ACQUISITION] == 0) {
            STREAM_MANAGER_LOGF(
                DEBUG,
                stream_manager,
                "No pending acquisition, release the connection=%p acquired immediately",
                (void *)connection);
            /* Release the acquired connection */
            should_release_connection = true;
        } else {
            struct aws_h2_sm_connection *sm_connection = s_sm_connection_new(stream_manager, connection);
            bool added = false;
            re_error |=
                aws_random_access_set_add(&stream_manager->synced_data.ideal_available_set, sm_connection, &added);
            re_error |= !added;
            ++stream_manager->synced_data.holding_connections_count;
        }
        s_aws_http2_stream_manager_build_transaction_synced(&work);
        s_unlock_synced_data(stream_manager);
    } /* END CRITICAL SECTION */

    if (should_release_connection) {
        STREAM_MANAGER_LOGF(DEBUG, stream_manager, "Releasing connection: %p", (void *)connection);
        re_error |= aws_http_connection_manager_release_connection(stream_manager->connection_manager, connection);
    }

    AWS_ASSERT(!re_error && "connection acquired callback fails with programming errors");
    (void)re_error;

    /* Fail acquisitions if any */
    s_finish_pending_stream_acquisitions_list_helper(
        stream_manager, &stream_acquisitions_to_fail, stream_fail_error_code);
    s_aws_http2_stream_manager_execute_transaction(&work);
}

static int s_on_incoming_headers(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    const struct aws_http_header *header_array,
    size_t num_headers,
    void *user_data) {
    struct aws_h2_sm_pending_stream_acquisition *pending_stream_acquisition = user_data;
    struct aws_h2_sm_connection *sm_connection = pending_stream_acquisition->sm_connection;
    struct aws_http2_stream_manager *stream_manager = sm_connection->stream_manager;

    if (pending_stream_acquisition->options.on_response_headers) {
        return pending_stream_acquisition->options.on_response_headers(
            stream, header_block, header_array, num_headers, pending_stream_acquisition->options.user_data);
    }
    if (stream_manager->close_connection_on_server_error) {
        /* Check status code if stream completed successfully. */
        int status_code = 0;
        aws_http_stream_get_incoming_response_status(stream, &status_code);
        AWS_ASSERT(status_code != 0); /* The get status should not fail */
        switch (status_code) {
            case AWS_HTTP_STATUS_CODE_500_INTERNAL_SERVER_ERROR:
            case AWS_HTTP_STATUS_CODE_502_BAD_GATEWAY:
            case AWS_HTTP_STATUS_CODE_503_SERVICE_UNAVAILABLE:
            case AWS_HTTP_STATUS_CODE_504_GATEWAY_TIMEOUT:
                /* For those error code if the retry happens, it should not use the same connection. */
                if (!sm_connection->thread_data.stopped_new_requests) {
                    STREAM_MANAGER_LOGF(
                        DEBUG,
                        stream_manager,
                        "no longer using connection: %p due to receiving %d server error status code for stream: %p",
                        (void *)sm_connection->connection,
                        status_code,
                        (void *)stream);
                    aws_http_connection_stop_new_requests(sm_connection->connection);
                    sm_connection->thread_data.stopped_new_requests = true;
                }
                break;
            default:
                break;
        }
    }
    return AWS_OP_SUCCESS;
}

static int s_on_incoming_header_block_done(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    void *user_data) {
    struct aws_h2_sm_pending_stream_acquisition *pending_stream_acquisition = user_data;
    if (pending_stream_acquisition->options.on_response_header_block_done) {
        return pending_stream_acquisition->options.on_response_header_block_done(
            stream, header_block, pending_stream_acquisition->options.user_data);
    }
    return AWS_OP_SUCCESS;
}

static int s_on_incoming_body(struct aws_http_stream *stream, const struct aws_byte_cursor *data, void *user_data) {
    struct aws_h2_sm_pending_stream_acquisition *pending_stream_acquisition = user_data;
    if (pending_stream_acquisition->options.on_response_body) {
        return pending_stream_acquisition->options.on_response_body(
            stream, data, pending_stream_acquisition->options.user_data);
    }
    return AWS_OP_SUCCESS;
}

/* Helper invoked when underlying connections is still available and the num stream assigned has been updated */
static void s_update_sm_connection_set_on_stream_finishes_synced(
    struct aws_h2_sm_connection *sm_connection,
    struct aws_http2_stream_manager *stream_manager) {

    int re_error = 0;
    size_t cur_num = sm_connection->num_streams_assigned;
    size_t ideal_num = stream_manager->ideal_concurrent_streams_per_connection;
    size_t max_num = sm_connection->max_concurrent_streams;
    /**
     * TODO: When the MAX_CONCURRENT_STREAMS from other side changed after the initial settings. We need to:
     * - figure out where I am
     * - figure out where I should be
     * - if they're different, remove from where I am, put where should be
     */
    if (sm_connection->state == AWS_H2SMCST_NEARLY_FULL && cur_num < ideal_num) {
        /* this connection is back from soft limited to ideal */
        bool exist = false;
        (void)exist;
        AWS_ASSERT(
            aws_random_access_set_exist(&stream_manager->synced_data.nonideal_available_set, sm_connection, &exist) ==
                AWS_OP_SUCCESS &&
            exist);
        re_error |= aws_random_access_set_remove(&stream_manager->synced_data.nonideal_available_set, sm_connection);
        bool added = false;
        re_error |= aws_random_access_set_add(&stream_manager->synced_data.ideal_available_set, sm_connection, &added);
        re_error |= !added;
        sm_connection->state = AWS_H2SMCST_IDEAL;
    } else if (sm_connection->state == AWS_H2SMCST_FULL && cur_num < max_num) {
        /* this connection is back from full */
        STREAM_MANAGER_LOGF(
            DEBUG,
            stream_manager,
            "connection:%p back to available, assigned stream=%zu, max concurrent streams=%" PRIu32 "",
            (void *)sm_connection->connection,
            cur_num,
            sm_connection->max_concurrent_streams);
        bool added = false;
        if (cur_num >= ideal_num) {
            sm_connection->state = AWS_H2SMCST_NEARLY_FULL;
            STREAM_MANAGER_LOGF(
                TRACE, stream_manager, "connection:%p added to soft limited set", (void *)sm_connection->connection);
            re_error |=
                aws_random_access_set_add(&stream_manager->synced_data.nonideal_available_set, sm_connection, &added);
        } else {
            sm_connection->state = AWS_H2SMCST_IDEAL;
            STREAM_MANAGER_LOGF(
                TRACE, stream_manager, "connection:%p added to ideal set", (void *)sm_connection->connection);
            re_error |=
                aws_random_access_set_add(&stream_manager->synced_data.ideal_available_set, sm_connection, &added);
        }
        re_error |= !added;
    }
    AWS_ASSERT(re_error == AWS_OP_SUCCESS);
    (void)re_error;
}

static void s_sm_connection_on_scheduled_stream_finishes(
    struct aws_h2_sm_connection *sm_connection,
    struct aws_http2_stream_manager *stream_manager) {
    /* Reach the max current will still allow new requests, but the new stream will complete with error */
    bool connection_available = aws_http_connection_new_requests_allowed(sm_connection->connection);
    struct aws_http2_stream_management_transaction work;
    s_aws_stream_management_transaction_init(&work, stream_manager);
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(stream_manager);
        s_sm_count_decrease_synced(stream_manager, AWS_SMCT_OPEN_STREAM, 1);
        --sm_connection->num_streams_assigned;
        if (!connection_available) {
            /* It might be removed already, but, it's fine */
            aws_random_access_set_remove(&stream_manager->synced_data.ideal_available_set, sm_connection);
            aws_random_access_set_remove(&stream_manager->synced_data.nonideal_available_set, sm_connection);
        } else {
            s_update_sm_connection_set_on_stream_finishes_synced(sm_connection, stream_manager);
        }
        s_aws_http2_stream_manager_build_transaction_synced(&work);
        /* After we build transaction, if the sm_connection still have zero assigned stream, we can kill the
         * sm_connection */
        if (sm_connection->num_streams_assigned == 0) {
            /* It might be removed already, but, it's fine */
            aws_random_access_set_remove(&stream_manager->synced_data.ideal_available_set, sm_connection);
            work.sm_connection_to_release = sm_connection;
            --stream_manager->synced_data.holding_connections_count;
            /* After we release one connection back, we should check if we need more connections */
            if (stream_manager->synced_data.state == AWS_H2SMST_READY &&
                stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_ACQUISITION]) {
                s_check_new_connections_needed_synced(&work);
            }
        }
        s_unlock_synced_data(stream_manager);
    } /* END CRITICAL SECTION */
    s_aws_http2_stream_manager_execute_transaction(&work);
}

static void s_on_stream_complete(struct aws_http_stream *stream, int error_code, void *user_data) {
    struct aws_h2_sm_pending_stream_acquisition *pending_stream_acquisition = user_data;
    struct aws_h2_sm_connection *sm_connection = pending_stream_acquisition->sm_connection;
    struct aws_http2_stream_manager *stream_manager = sm_connection->stream_manager;
    if (pending_stream_acquisition->options.on_complete) {
        pending_stream_acquisition->options.on_complete(
            stream, error_code, pending_stream_acquisition->options.user_data);
    }
    s_sm_connection_on_scheduled_stream_finishes(sm_connection, stream_manager);
}

static void s_on_stream_destroy(void *user_data) {
    struct aws_h2_sm_pending_stream_acquisition *pending_stream_acquisition = user_data;
    if (pending_stream_acquisition->options.on_destroy) {
        pending_stream_acquisition->options.on_destroy(pending_stream_acquisition->options.user_data);
    }
    s_pending_stream_acquisition_destroy(pending_stream_acquisition);
}

/* Scheduled to happen from connection's thread */
static void s_make_request_task(struct aws_channel_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    struct aws_h2_sm_pending_stream_acquisition *pending_stream_acquisition = arg;
    struct aws_h2_sm_connection *sm_connection = pending_stream_acquisition->sm_connection;
    struct aws_http2_stream_manager *stream_manager = sm_connection->stream_manager;
    int error_code = AWS_ERROR_SUCCESS;

    STREAM_MANAGER_LOGF(
        TRACE,
        stream_manager,
        "Make request task running for acquisition:%p from connection:%p thread",
        (void *)pending_stream_acquisition,
        (void *)sm_connection->connection);
    bool is_shutting_down = false;
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(stream_manager);
        is_shutting_down = stream_manager->synced_data.state != AWS_H2SMST_READY;
        s_sm_count_decrease_synced(stream_manager, AWS_SMCT_PENDING_MAKE_REQUESTS, 1);
        /* The stream has not open yet, but we increase the count here, if anything fails, the count will be decreased
         */
        s_sm_count_increase_synced(stream_manager, AWS_SMCT_OPEN_STREAM, 1);
        AWS_ASSERT(
            sm_connection->max_concurrent_streams >= sm_connection->num_streams_assigned &&
            "The max concurrent streams exceed");
        s_unlock_synced_data(stream_manager);
    } /* END CRITICAL SECTION */
    /* this is a channel task. If it is canceled, that means the channel shutdown. In that case, that's equivalent
     * to a closed connection. */
    if (status != AWS_TASK_STATUS_RUN_READY) {
        STREAM_MANAGER_LOGF(
            ERROR,
            stream_manager,
            "acquisition:%p failed as the task is cancelled.",
            (void *)pending_stream_acquisition);
        error_code = AWS_ERROR_HTTP_CONNECTION_CLOSED;
        goto error;
    }
    if (is_shutting_down) {
        STREAM_MANAGER_LOGF(
            ERROR,
            stream_manager,
            "acquisition:%p failed as stream manager is shutting down before task runs.",
            (void *)pending_stream_acquisition);
        error_code = AWS_ERROR_HTTP_STREAM_MANAGER_SHUTTING_DOWN;
        goto error;
    }
    struct aws_http_make_request_options request_options = {
        .self_size = sizeof(request_options),
        .request = pending_stream_acquisition->request,
        .on_response_headers = s_on_incoming_headers,
        .on_response_header_block_done = s_on_incoming_header_block_done,
        .on_response_body = s_on_incoming_body,
        .on_complete = s_on_stream_complete,
        .on_destroy = s_on_stream_destroy,
        .user_data = pending_stream_acquisition,
        .http2_use_manual_data_writes = pending_stream_acquisition->options.http2_use_manual_data_writes,
    };
    /* TODO: we could put the pending acquisition back to the list if the connection is not available for new request.
     */

    struct aws_http_stream *stream = aws_http_connection_make_request(sm_connection->connection, &request_options);
    if (!stream) {
        error_code = aws_last_error();
        STREAM_MANAGER_LOGF(
            ERROR,
            stream_manager,
            "acquisition:%p failed as HTTP level make request failed with error: %d(%s).",
            (void *)pending_stream_acquisition,
            error_code,
            aws_error_str(error_code));
        goto error;
    }
    /* Since we're in the connection's thread, this should be safe, there won't be any other callbacks to the user */
    if (aws_http_stream_activate(stream)) {
        /* Activate failed, the on_completed callback will NOT be invoked from HTTP, but we already told user about
         * the stream. Invoke the user completed callback here */
        error_code = aws_last_error();
        STREAM_MANAGER_LOGF(
            ERROR,
            stream_manager,
            "acquisition:%p failed as stream activate failed with error: %d(%s).",
            (void *)pending_stream_acquisition,
            error_code,
            aws_error_str(error_code));
        goto error;
    }
    if (pending_stream_acquisition->callback) {
        pending_stream_acquisition->callback(stream, 0, pending_stream_acquisition->user_data);
    }

    /* Happy case, the complete callback will be invoked, and we clean things up at the callback, but we can release the
     * request now */
    aws_http_message_release(pending_stream_acquisition->request);
    pending_stream_acquisition->request = NULL;
    return;
error:
    if (pending_stream_acquisition->callback) {
        pending_stream_acquisition->callback(NULL, error_code, pending_stream_acquisition->user_data);
    }
    s_pending_stream_acquisition_destroy(pending_stream_acquisition);
    /* task should happen after destroy, as the task can trigger the whole stream manager to be destroyed */
    s_sm_connection_on_scheduled_stream_finishes(sm_connection, stream_manager);
}

/* NEVER invoke with lock held */
static void s_aws_http2_stream_manager_execute_transaction(struct aws_http2_stream_management_transaction *work) {

    struct aws_http2_stream_manager *stream_manager = work->stream_manager;

    /* Step1: Release connection */
    if (work->sm_connection_to_release) {
        AWS_ASSERT(work->sm_connection_to_release->num_streams_assigned == 0);
        STREAM_MANAGER_LOGF(
            DEBUG,
            stream_manager,
            "Release connection:%p back to connection manager as no outstanding streams",
            (void *)work->sm_connection_to_release->connection);
        s_sm_connection_release_connection(work->sm_connection_to_release);
    }

    /* Step2: Make request. The work should know what connection for the request to be made. */
    while (!aws_linked_list_empty(&work->pending_make_requests)) {
        /* The completions can also fail as the connection can be unavailable after the decision made. We just fail
         * the acquisition */
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&work->pending_make_requests);
        struct aws_h2_sm_pending_stream_acquisition *pending_stream_acquisition =
            AWS_CONTAINER_OF(node, struct aws_h2_sm_pending_stream_acquisition, node);

        AWS_ASSERT(
            pending_stream_acquisition->sm_connection &&
            "Stream manager internal bug: connection is not decided before execute transaction");

        STREAM_MANAGER_LOGF(
            TRACE,
            stream_manager,
            "acquisition:%p is scheduled to be made request from connection:%p thread",
            (void *)pending_stream_acquisition,
            (void *)pending_stream_acquisition->sm_connection->connection);
        /**
         * schedule a task from the connection's event loop to make request, so that:
         * - We can activate the stream for user and then invoked the callback
         * - The callback will happen asynced even the stream failed to be created
         * - We can make sure we will not break the settings
         */
        struct aws_channel *channel =
            aws_http_connection_get_channel(pending_stream_acquisition->sm_connection->connection);
        aws_channel_task_init(
            &pending_stream_acquisition->make_request_task,
            s_make_request_task,
            pending_stream_acquisition,
            "Stream manager make request task");
        aws_channel_schedule_task_now(channel, &pending_stream_acquisition->make_request_task);
    }

    /* Step 3: Acquire connections if needed */
    if (work->new_connections) {
        STREAM_MANAGER_LOGF(DEBUG, stream_manager, "acquiring %zu new connections", work->new_connections);
    }
    for (size_t i = 0; i < work->new_connections; ++i) {
        aws_http_connection_manager_acquire_connection(
            stream_manager->connection_manager, s_sm_on_connection_acquired, stream_manager);
    }

    /*
     * Step 4: Clean up work.  Do this here rather than at the end of every caller. Destroy the manager if necessary
     */
    s_aws_stream_management_transaction_clean_up(work);
}

void s_stream_manager_destroy_final(struct aws_http2_stream_manager *stream_manager) {
    if (!stream_manager) {
        return;
    }

    STREAM_MANAGER_LOG(TRACE, stream_manager, "Stream Manager finishes destroying self");
    /* Connection manager has already been cleaned up */
    AWS_FATAL_ASSERT(stream_manager->connection_manager == NULL);
    AWS_FATAL_ASSERT(aws_linked_list_empty(&stream_manager->synced_data.pending_stream_acquisitions));
    aws_mutex_clean_up(&stream_manager->synced_data.lock);
    aws_random_access_set_clean_up(&stream_manager->synced_data.ideal_available_set);
    aws_random_access_set_clean_up(&stream_manager->synced_data.nonideal_available_set);
    aws_client_bootstrap_release(stream_manager->bootstrap);

    if (stream_manager->shutdown_complete_callback) {
        stream_manager->shutdown_complete_callback(stream_manager->shutdown_complete_user_data);
    }
    aws_mem_release(stream_manager->allocator, stream_manager);
}

void s_stream_manager_on_cm_shutdown_complete(void *user_data) {
    struct aws_http2_stream_manager *stream_manager = (struct aws_http2_stream_manager *)user_data;
    STREAM_MANAGER_LOGF(
        TRACE,
        stream_manager,
        "Underlying connection manager (ip=%p) finished shutdown, stream manager can finish destroying now",
        (void *)stream_manager->connection_manager);
    stream_manager->connection_manager = NULL;
    s_stream_manager_destroy_final(stream_manager);
}

static void s_stream_manager_start_destroy(struct aws_http2_stream_manager *stream_manager) {
    STREAM_MANAGER_LOG(TRACE, stream_manager, "Stream Manager reaches the condition to destroy, start to destroy");
    /* If there is no outstanding streams, the connections set should be empty. */
    AWS_ASSERT(aws_random_access_set_get_size(&stream_manager->synced_data.ideal_available_set) == 0);
    AWS_ASSERT(aws_random_access_set_get_size(&stream_manager->synced_data.nonideal_available_set) == 0);
    AWS_ASSERT(stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_CONNECTIONS_ACQUIRING] == 0);
    AWS_ASSERT(stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_OPEN_STREAM] == 0);
    AWS_ASSERT(stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_MAKE_REQUESTS] == 0);
    AWS_ASSERT(stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_ACQUISITION] == 0);
    AWS_ASSERT(stream_manager->connection_manager);
    struct aws_http_connection_manager *cm = stream_manager->connection_manager;
    stream_manager->connection_manager = NULL;
    aws_http_connection_manager_release(cm);
}

void s_stream_manager_on_zero_external_ref(struct aws_http2_stream_manager *stream_manager) {
    STREAM_MANAGER_LOG(
        TRACE,
        stream_manager,
        "Last refcount released, manager stop accepting new stream request and will start to clean up when not "
        "outstanding tasks remaining.");
    struct aws_http2_stream_management_transaction work;
    s_aws_stream_management_transaction_init(&work, stream_manager);
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(stream_manager);
        stream_manager->synced_data.state = AWS_H2SMST_DESTROYING;
        s_aws_http2_stream_manager_build_transaction_synced(&work);
        /* Release the internal ref count as no external usage anymore */
        aws_ref_count_release(&stream_manager->internal_ref_count);
        s_unlock_synced_data(stream_manager);
    } /* END CRITICAL SECTION */
    s_aws_http2_stream_manager_execute_transaction(&work);
}

struct aws_http2_stream_manager *aws_http2_stream_manager_new(
    struct aws_allocator *allocator,
    const struct aws_http2_stream_manager_options *options) {

    AWS_PRECONDITION(allocator);
    /* The other options are validated by the aws_http_connection_manager_new */
    if (!options->http2_prior_knowledge && !options->tls_connection_options) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION_MANAGER,
            "Invalid options - Prior knowledge must be used for cleartext HTTP/2 connections."
            " Upgrade from HTTP/1.1 is not supported.");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }
    struct aws_http2_stream_manager *stream_manager =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http2_stream_manager));
    stream_manager->allocator = allocator;
    aws_linked_list_init(&stream_manager->synced_data.pending_stream_acquisitions);

    if (aws_mutex_init(&stream_manager->synced_data.lock)) {
        goto on_error;
    }
    if (aws_random_access_set_init(
            &stream_manager->synced_data.ideal_available_set,
            allocator,
            aws_hash_ptr,
            aws_ptr_eq,
            NULL /* destroy function */,
            2)) {
        goto on_error;
    }
    if (aws_random_access_set_init(
            &stream_manager->synced_data.nonideal_available_set,
            allocator,
            aws_hash_ptr,
            aws_ptr_eq,
            NULL /* destroy function */,
            2)) {
        goto on_error;
    }
    aws_ref_count_init(
        &stream_manager->external_ref_count,
        stream_manager,
        (aws_simple_completion_callback *)s_stream_manager_on_zero_external_ref);
    aws_ref_count_init(
        &stream_manager->internal_ref_count,
        stream_manager,
        (aws_simple_completion_callback *)s_stream_manager_start_destroy);

    if (options->connection_ping_period_ms) {
        stream_manager->connection_ping_period_ns =
            aws_timestamp_convert(options->connection_ping_period_ms, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL);
        size_t connection_ping_timeout_ms =
            options->connection_ping_timeout_ms ? options->connection_ping_timeout_ms : s_default_ping_timeout_ms;
        stream_manager->connection_ping_timeout_ns =
            aws_timestamp_convert(connection_ping_timeout_ms, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL);
        if (stream_manager->connection_ping_period_ns < stream_manager->connection_ping_timeout_ns) {
            STREAM_MANAGER_LOGF(
                WARN,
                stream_manager,
                "connection_ping_period_ms: %zu is shorter than connection_ping_timeout_ms: %zu. Clapping "
                "connection_ping_timeout_ms to %zu",
                options->connection_ping_period_ms,
                connection_ping_timeout_ms,
                options->connection_ping_period_ms);
            stream_manager->connection_ping_timeout_ns = stream_manager->connection_ping_period_ns;
        }
    }

    stream_manager->bootstrap = aws_client_bootstrap_acquire(options->bootstrap);
    struct aws_http_connection_manager_options cm_options = {
        .bootstrap = options->bootstrap,
        .socket_options = options->socket_options,
        .tls_connection_options = options->tls_connection_options,
        .http2_prior_knowledge = options->http2_prior_knowledge,
        .host = options->host,
        .port = options->port,
        .enable_read_back_pressure = options->enable_read_back_pressure,
        .monitoring_options = options->monitoring_options,
        .proxy_options = options->proxy_options,
        .proxy_ev_settings = options->proxy_ev_settings,
        .max_connections = options->max_connections,
        .shutdown_complete_user_data = stream_manager,
        .shutdown_complete_callback = s_stream_manager_on_cm_shutdown_complete,
        .initial_settings_array = options->initial_settings_array,
        .num_initial_settings = options->num_initial_settings,
        .max_closed_streams = options->max_closed_streams,
        .http2_conn_manual_window_management = options->conn_manual_window_management,
    };
    /* aws_http_connection_manager_new needs to be the last thing that can fail */
    stream_manager->connection_manager = aws_http_connection_manager_new(allocator, &cm_options);
    if (!stream_manager->connection_manager) {
        goto on_error;
    }
    /* Nothing can fail after here */
    stream_manager->synced_data.state = AWS_H2SMST_READY;
    stream_manager->shutdown_complete_callback = options->shutdown_complete_callback;
    stream_manager->shutdown_complete_user_data = options->shutdown_complete_user_data;
    stream_manager->ideal_concurrent_streams_per_connection = options->ideal_concurrent_streams_per_connection
                                                                  ? options->ideal_concurrent_streams_per_connection
                                                                  : UINT32_MAX;
    stream_manager->max_concurrent_streams_per_connection =
        options->max_concurrent_streams_per_connection ? options->max_concurrent_streams_per_connection : UINT32_MAX;
    stream_manager->max_connections = options->max_connections;
    stream_manager->close_connection_on_server_error = options->close_connection_on_server_error;

    return stream_manager;
on_error:
    s_stream_manager_destroy_final(stream_manager);
    return NULL;
}

struct aws_http2_stream_manager *aws_http2_stream_manager_acquire(struct aws_http2_stream_manager *stream_manager) {
    if (stream_manager) {
        aws_ref_count_acquire(&stream_manager->external_ref_count);
    }
    return stream_manager;
}

struct aws_http2_stream_manager *aws_http2_stream_manager_release(struct aws_http2_stream_manager *stream_manager) {
    if (stream_manager) {
        aws_ref_count_release(&stream_manager->external_ref_count);
    }
    return NULL;
}

void aws_http2_stream_manager_acquire_stream(
    struct aws_http2_stream_manager *stream_manager,
    const struct aws_http2_stream_manager_acquire_stream_options *acquire_stream_option) {
    AWS_PRECONDITION(stream_manager);
    AWS_PRECONDITION(acquire_stream_option);
    AWS_PRECONDITION(acquire_stream_option->callback);
    AWS_PRECONDITION(acquire_stream_option->options);
    struct aws_http2_stream_management_transaction work;
    struct aws_h2_sm_pending_stream_acquisition *pending_stream_acquisition = s_new_pending_stream_acquisition(
        stream_manager->allocator,
        acquire_stream_option->options,
        acquire_stream_option->callback,
        acquire_stream_option->user_data);
    STREAM_MANAGER_LOGF(
        TRACE, stream_manager, "Stream Manager creates acquisition:%p for user", (void *)pending_stream_acquisition);
    s_aws_stream_management_transaction_init(&work, stream_manager);
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(stream_manager);
        /* it's use after free crime */
        AWS_FATAL_ASSERT(stream_manager->synced_data.state != AWS_H2SMST_DESTROYING);
        aws_linked_list_push_back(
            &stream_manager->synced_data.pending_stream_acquisitions, &pending_stream_acquisition->node);
        s_sm_count_increase_synced(stream_manager, AWS_SMCT_PENDING_ACQUISITION, 1);
        s_aws_http2_stream_manager_build_transaction_synced(&work);
        s_unlock_synced_data(stream_manager);
    } /* END CRITICAL SECTION */
    s_aws_http2_stream_manager_execute_transaction(&work);
}

static size_t s_get_available_streams_num_from_connection_set(const struct aws_random_access_set *set) {
    size_t all_available_streams_num = 0;
    size_t ideal_connection_num = aws_random_access_set_get_size(set);
    for (size_t i = 0; i < ideal_connection_num; i++) {
        struct aws_h2_sm_connection *sm_connection = NULL;
        AWS_FATAL_ASSERT(aws_random_access_set_random_get_ptr_index(set, (void **)&sm_connection, i) == AWS_OP_SUCCESS);
        uint32_t available_streams = sm_connection->max_concurrent_streams - sm_connection->num_streams_assigned;
        all_available_streams_num += (size_t)available_streams;
    }
    return all_available_streams_num;
}

void aws_http2_stream_manager_fetch_metrics(
    const struct aws_http2_stream_manager *stream_manager,
    struct aws_http_manager_metrics *out_metrics) {
    AWS_PRECONDITION(stream_manager);
    AWS_PRECONDITION(out_metrics);
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data((struct aws_http2_stream_manager *)(void *)stream_manager);
        size_t all_available_streams_num = 0;
        all_available_streams_num +=
            s_get_available_streams_num_from_connection_set(&stream_manager->synced_data.ideal_available_set);
        all_available_streams_num +=
            s_get_available_streams_num_from_connection_set(&stream_manager->synced_data.nonideal_available_set);
        out_metrics->pending_concurrency_acquires =
            stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_PENDING_ACQUISITION];
        out_metrics->available_concurrency = all_available_streams_num;
        out_metrics->leased_concurrency = stream_manager->synced_data.internal_refcount_stats[AWS_SMCT_OPEN_STREAM];
        s_unlock_synced_data((struct aws_http2_stream_manager *)(void *)stream_manager);
    } /* END CRITICAL SECTION */
}
