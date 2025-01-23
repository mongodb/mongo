#ifndef AWS_EVENT_STREAM_RPC_TEST_HELPER_H
#define AWS_EVENT_STREAM_RPC_TEST_HELPER_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/event-stream/event_stream_rpc_server.h>

#ifndef AWS_UNSTABLE_TESTING_API
#    error The functions in this header file are for testing purposes only!
#endif

AWS_EXTERN_C_BEGIN

/** This is for testing edge cases around stream id exhaustion. Don't ever include this file outside of a unit test. */
AWS_EVENT_STREAM_API void aws_event_stream_rpc_server_override_last_stream_id(
    struct aws_event_stream_rpc_server_connection *connection,
    int32_t value);

AWS_EXTERN_C_END

#endif /* AWS_EVENT_STREAM_RPC_TEST_HELPER_H */
