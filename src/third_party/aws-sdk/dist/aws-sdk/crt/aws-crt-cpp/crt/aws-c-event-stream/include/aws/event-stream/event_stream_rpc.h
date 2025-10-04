#ifndef AWS_EVENT_STREAM_RPC_H
#define AWS_EVENT_STREAM_RPC_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/event-stream/event_stream.h>

AWS_PUSH_SANE_WARNING_LEVEL

/**
 * :message-type header name
 */
extern AWS_EVENT_STREAM_API const struct aws_byte_cursor aws_event_stream_rpc_message_type_name;
/**
 * :message-flags header name
 */
extern AWS_EVENT_STREAM_API const struct aws_byte_cursor aws_event_stream_rpc_message_flags_name;
/**
 * :stream-id header name
 */
extern AWS_EVENT_STREAM_API const struct aws_byte_cursor aws_event_stream_rpc_stream_id_name;
/**
 * operation header name.
 */
extern AWS_EVENT_STREAM_API const struct aws_byte_cursor aws_event_stream_rpc_operation_name;

enum aws_event_stream_rpc_message_type {
    AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_APPLICATION_MESSAGE,
    AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_APPLICATION_ERROR,
    AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_PING,
    AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_PING_RESPONSE,
    AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_CONNECT,
    AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_CONNECT_ACK,
    AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_PROTOCOL_ERROR,
    AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_INTERNAL_ERROR,

    AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_COUNT,
};

enum aws_event_stream_rpc_message_flag {
    AWS_EVENT_STREAM_RPC_MESSAGE_FLAG_CONNECTION_ACCEPTED = 1,
    AWS_EVENT_STREAM_RPC_MESSAGE_FLAG_TERMINATE_STREAM = 2,
};

struct aws_event_stream_rpc_message_args {
    /** array of headers for an event-stream message. */
    struct aws_event_stream_header_value_pair *headers;
    /** number of headers in the headers array.
     * headers are copied in aws_event_stream_rpc_*_send_message()
     * so you can free the memory immediately after calling it if you need to.*/
    size_t headers_count;
    /** payload buffer for the message, payload is copied in aws_event_stream_rpc_*_send_message()
     * so you can free the memory immediately after calling it if you need to. */
    struct aws_byte_buf *payload;
    /** message type for the message. This will be added to the headers array
     * and the ":message-type" header should not be included in headers */
    enum aws_event_stream_rpc_message_type message_type;
    /** message flags for the message. This will be added to the headers array
     * and the ":message-flags" header should not be included in headers */
    uint32_t message_flags;
};
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_EVENT_STREAM_RPC_SERVER_H */
