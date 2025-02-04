#ifndef AWS_IO_TRACING_H
#define AWS_IO_TRACING_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/external/ittnotify.h>
#include <aws/io/io.h>

extern __itt_domain *io_tracing_domain;
extern __itt_string_handle *tracing_input_stream_read;
extern __itt_string_handle *tracing_event_loop_run_tasks;
extern __itt_string_handle *tracing_event_loop_event;
extern __itt_string_handle *tracing_event_loop_events;

AWS_EXTERN_C_BEGIN

void aws_io_tracing_init(void);

AWS_EXTERN_C_END
#endif /* AWS_IO_TRACING_H */
