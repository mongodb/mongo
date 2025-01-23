#ifndef AWS_HTTP_HTTP_MONITOR_H
#define AWS_HTTP_HTTP_MONITOR_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/connection.h>
#include <aws/http/http.h>

struct aws_allocator;
struct aws_crt_statistics_handler;

/*
 * Needed by tests
 */
struct aws_statistics_handler_http_connection_monitor_impl {
    struct aws_http_connection_monitoring_options options;

    uint64_t throughput_failure_time_ms;
    uint32_t last_incoming_stream_id;
    uint32_t last_outgoing_stream_id;
    uint64_t last_measured_throughput;
};

AWS_EXTERN_C_BEGIN

/**
 * Creates a new http connection monitor that regularly checks the connection's throughput and shuts the connection
 * down if the a minimum threshold is not met for a configurable number of seconds.
 */
AWS_HTTP_API
struct aws_crt_statistics_handler *aws_crt_statistics_handler_new_http_connection_monitor(
    struct aws_allocator *allocator,
    struct aws_http_connection_monitoring_options *options);

/**
 * Validates monitoring options to ensure they are sensible
 */
AWS_HTTP_API
bool aws_http_connection_monitoring_options_is_valid(const struct aws_http_connection_monitoring_options *options);

AWS_EXTERN_C_END

#endif /* AWS_HTTP_HTTP_MONITOR_H */
