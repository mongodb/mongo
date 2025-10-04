/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/private/tracing.h>

__itt_domain *io_tracing_domain;
__itt_string_handle *tracing_input_stream_read;
__itt_string_handle *tracing_event_loop_run_tasks;
__itt_string_handle *tracing_event_loop_event;
__itt_string_handle *tracing_event_loop_events;

void aws_io_tracing_init(void) {
    io_tracing_domain = __itt_domain_create("aws.c.io");
    tracing_input_stream_read = __itt_string_handle_create("Read:InputStream");
    tracing_event_loop_run_tasks = __itt_string_handle_create("RunTasks:EventLoop");
    tracing_event_loop_event = __itt_string_handle_create("IOEvent:EventLoop");
    tracing_event_loop_events = __itt_string_handle_create("IOEvents:EventLoop");
}
