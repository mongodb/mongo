#ifndef AWS_EVENT_STREAM_RPC_PRIV_H
#define AWS_EVENT_STREAM_RPC_PRIV_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/event-stream/event_stream_rpc.h>

enum aws_event_stream_connection_handshake_state {
    CONNECTION_HANDSHAKE_STATE_INITIALIZED = 0u,
    CONNECTION_HANDSHAKE_STATE_CONNECT_PROCESSED = 1u,
    CONNECTION_HANDSHAKE_STATE_CONNECT_ACK_PROCESSED = 2u,
};

int aws_event_stream_rpc_extract_message_metadata(
    const struct aws_array_list *message_headers,
    int32_t *stream_id,
    int32_t *message_type,
    int32_t *message_flags,
    struct aws_byte_buf *operation_name);

uint64_t aws_event_stream_rpc_hash_streamid(const void *to_hash);
bool aws_event_stream_rpc_streamid_eq(const void *a, const void *b);

static const struct aws_byte_cursor s_json_content_type_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(":content-type");
static const struct aws_byte_cursor s_json_content_type_value =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("application/json");

static const struct aws_byte_cursor s_invalid_stream_id_error =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("{ \"message\": \"non-zero stream-id field is only allowed for messages of "
                                          "type APPLICATION_MESSAGE. The stream id max value is INT32_MAX.\" }");

static const struct aws_byte_cursor s_invalid_client_stream_id_error =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("{ \"message\": \"stream-id values must be monotonically incrementing. A "
                                          "stream-id arrived that was lower than the last seen stream-id.\" }");

static const struct aws_byte_cursor s_invalid_new_client_stream_id_error =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("{ \"message\": \"stream-id values must be monotonically incrementing. A new "
                                          "stream-id arrived that was incremented by more than 1.\" }");

static const struct aws_byte_cursor s_invalid_message_type_error =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("{ \"message\": \"an invalid value for message-type field was received.\" }");

static const struct aws_byte_cursor s_invalid_message_error = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(
    "{ \"message\": \"A message was received with missing required fields. Check that your client is sending at least, "
    ":message-type, :message-flags, and :stream-id\" }");

static const struct aws_byte_cursor s_internal_error = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(
    "{ \"message\": \"An error occurred on the peer endpoint. This is not likely caused by your endpoint.\" }");

static const struct aws_byte_cursor s_connect_not_completed_error = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(
    "{ \"message\": \"A CONNECT message must be received, and the CONNECT_ACK must be sent in response, before any "
    "other message-types can be sent on this connection. In addition, only one CONNECT message is allowed on a "
    "connection.\" }");

#endif /* #define AWS_EVENT_STREAM_RPC_PRIV_H */
