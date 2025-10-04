/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/connection_monitor.h>

#include <aws/http/connection.h>
#include <aws/http/statistics.h>
#include <aws/io/channel.h>
#include <aws/io/logging.h>
#include <aws/io/statistics.h>

#include <aws/common/clock.h>

#include <inttypes.h>

static void s_process_statistics(
    struct aws_crt_statistics_handler *handler,
    struct aws_crt_statistics_sample_interval *interval,
    struct aws_array_list *stats_list,
    void *context) {

    (void)interval;

    struct aws_statistics_handler_http_connection_monitor_impl *impl = handler->impl;
    if (!aws_http_connection_monitoring_options_is_valid(&impl->options)) {
        return;
    }

    uint64_t pending_read_interval_ms = 0;
    uint64_t pending_write_interval_ms = 0;
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
    uint32_t h1_current_outgoing_stream_id = 0;
    uint32_t h1_current_incoming_stream_id = 0;

    /*
     * Pull out the data needed to perform the throughput calculation
     */
    size_t stats_count = aws_array_list_length(stats_list);
    bool h2 = false;
    bool h2_was_inactive = false;

    for (size_t i = 0; i < stats_count; ++i) {
        struct aws_crt_statistics_base *stats_base = NULL;
        if (aws_array_list_get_at(stats_list, &stats_base, i)) {
            continue;
        }

        switch (stats_base->category) {
            case AWSCRT_STAT_CAT_SOCKET: {
                struct aws_crt_statistics_socket *socket_stats = (struct aws_crt_statistics_socket *)stats_base;
                bytes_read = socket_stats->bytes_read;
                bytes_written = socket_stats->bytes_written;
                break;
            }

            case AWSCRT_STAT_CAT_HTTP1_CHANNEL: {
                AWS_ASSERT(!h2);
                struct aws_crt_statistics_http1_channel *http1_stats =
                    (struct aws_crt_statistics_http1_channel *)stats_base;
                pending_read_interval_ms = http1_stats->pending_incoming_stream_ms;
                pending_write_interval_ms = http1_stats->pending_outgoing_stream_ms;
                h1_current_outgoing_stream_id = http1_stats->current_outgoing_stream_id;
                h1_current_incoming_stream_id = http1_stats->current_incoming_stream_id;

                break;
            }

            case AWSCRT_STAT_CAT_HTTP2_CHANNEL: {
                struct aws_crt_statistics_http2_channel *h2_stats =
                    (struct aws_crt_statistics_http2_channel *)stats_base;
                pending_read_interval_ms = h2_stats->pending_incoming_stream_ms;
                pending_write_interval_ms = h2_stats->pending_outgoing_stream_ms;
                h2_was_inactive |= h2_stats->was_inactive;
                h2 = true;
                break;
            }

            default:
                break;
        }
    }

    if (impl->options.statistics_observer_fn) {
        impl->options.statistics_observer_fn(
            (size_t)(uintptr_t)(context), stats_list, impl->options.statistics_observer_user_data);
    }

    struct aws_channel *channel = context;

    uint64_t bytes_per_second = 0;
    uint64_t max_pending_io_interval_ms = 0;

    if (pending_write_interval_ms > 0) {
        double fractional_bytes_written_per_second =
            (double)bytes_written * (double)AWS_TIMESTAMP_MILLIS / (double)pending_write_interval_ms;
        if (fractional_bytes_written_per_second >= (double)UINT64_MAX) {
            bytes_per_second = UINT64_MAX;
        } else {
            bytes_per_second = (uint64_t)fractional_bytes_written_per_second;
        }
        max_pending_io_interval_ms = pending_write_interval_ms;
    }

    if (pending_read_interval_ms > 0) {
        double fractional_bytes_read_per_second =
            (double)bytes_read * (double)AWS_TIMESTAMP_MILLIS / (double)pending_read_interval_ms;
        if (fractional_bytes_read_per_second >= (double)UINT64_MAX) {
            bytes_per_second = UINT64_MAX;
        } else {
            bytes_per_second = aws_add_u64_saturating(bytes_per_second, (uint64_t)fractional_bytes_read_per_second);
        }
        if (pending_read_interval_ms > max_pending_io_interval_ms) {
            max_pending_io_interval_ms = pending_read_interval_ms;
        }
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_CHANNEL,
        "id=%p: channel throughput - %" PRIu64 " bytes per second",
        (void *)channel,
        bytes_per_second);

    /*
     * Check throughput only if the connection has active stream and no gap between.
     */
    bool check_throughput = false;
    if (h2) {
        /* For HTTP/2, check throughput only if there always has any active stream on the connection */
        check_throughput = !h2_was_inactive;
    } else {
        /* For HTTP/1, check throughput only if at least one stream exists and was observed in that role previously */
        check_throughput =
            (h1_current_incoming_stream_id != 0 && h1_current_incoming_stream_id == impl->last_incoming_stream_id) ||
            (h1_current_outgoing_stream_id != 0 && h1_current_outgoing_stream_id == impl->last_outgoing_stream_id);

        impl->last_outgoing_stream_id = h1_current_outgoing_stream_id;
        impl->last_incoming_stream_id = h1_current_incoming_stream_id;
    }
    impl->last_measured_throughput = bytes_per_second;

    if (!check_throughput) {
        AWS_LOGF_TRACE(AWS_LS_IO_CHANNEL, "id=%p: channel throughput does not need to be checked", (void *)channel);
        impl->throughput_failure_time_ms = 0;
        return;
    }

    if (bytes_per_second >= impl->options.minimum_throughput_bytes_per_second) {
        impl->throughput_failure_time_ms = 0;
        return;
    }

    impl->throughput_failure_time_ms =
        aws_add_u64_saturating(impl->throughput_failure_time_ms, max_pending_io_interval_ms);

    AWS_LOGF_INFO(
        AWS_LS_IO_CHANNEL,
        "id=%p: Channel low throughput warning.  Currently %" PRIu64 " milliseconds of consecutive failure time",
        (void *)channel,
        impl->throughput_failure_time_ms);

    uint64_t maximum_failure_time_ms = aws_timestamp_convert(
        impl->options.allowable_throughput_failure_interval_seconds, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_MILLIS, NULL);
    if (impl->throughput_failure_time_ms <= maximum_failure_time_ms) {
        return;
    }

    AWS_LOGF_INFO(
        AWS_LS_IO_CHANNEL,
        "id=%p: Channel low throughput threshold exceeded (< %" PRIu64
        " bytes per second for more than %u seconds).  Shutting down.",
        (void *)channel,
        impl->options.minimum_throughput_bytes_per_second,
        impl->options.allowable_throughput_failure_interval_seconds);

    aws_channel_shutdown(channel, AWS_ERROR_HTTP_CHANNEL_THROUGHPUT_FAILURE);
}

static void s_destroy(struct aws_crt_statistics_handler *handler) {
    if (handler == NULL) {
        return;
    }

    aws_mem_release(handler->allocator, handler);
}

static uint64_t s_get_report_interval_ms(struct aws_crt_statistics_handler *handler) {
    (void)handler;

    return 1000;
}

static struct aws_crt_statistics_handler_vtable s_http_connection_monitor_vtable = {
    .process_statistics = s_process_statistics,
    .destroy = s_destroy,
    .get_report_interval_ms = s_get_report_interval_ms,
};

struct aws_crt_statistics_handler *aws_crt_statistics_handler_new_http_connection_monitor(
    struct aws_allocator *allocator,
    struct aws_http_connection_monitoring_options *options) {
    struct aws_crt_statistics_handler *handler = NULL;
    struct aws_statistics_handler_http_connection_monitor_impl *impl = NULL;

    if (!aws_mem_acquire_many(
            allocator,
            2,
            &handler,
            sizeof(struct aws_crt_statistics_handler),
            &impl,
            sizeof(struct aws_statistics_handler_http_connection_monitor_impl))) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*handler);
    AWS_ZERO_STRUCT(*impl);
    impl->options = *options;

    handler->vtable = &s_http_connection_monitor_vtable;
    handler->allocator = allocator;
    handler->impl = impl;

    return handler;
}

bool aws_http_connection_monitoring_options_is_valid(const struct aws_http_connection_monitoring_options *options) {
    if (options == NULL) {
        return false;
    }

    return options->allowable_throughput_failure_interval_seconds > 0 &&
           options->minimum_throughput_bytes_per_second > 0;
}
