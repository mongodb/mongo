/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/event-stream/event_stream_rpc.h>

#include <inttypes.h>

const struct aws_byte_cursor aws_event_stream_rpc_message_type_name =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(":message-type");
const struct aws_byte_cursor aws_event_stream_rpc_message_flags_name =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(":message-flags");
const struct aws_byte_cursor aws_event_stream_rpc_stream_id_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(":stream-id");
const struct aws_byte_cursor aws_event_stream_rpc_operation_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("operation");

/* just a convenience function for fetching message metadata from the event stream headers on a single iteration. */
int aws_event_stream_rpc_extract_message_metadata(
    const struct aws_array_list *message_headers,
    int32_t *stream_id,
    int32_t *message_type,
    int32_t *message_flags,
    struct aws_byte_buf *operation_name) {
    size_t length = aws_array_list_length(message_headers);
    bool message_type_found = 0;
    bool message_flags_found = 0;
    bool stream_id_found = 0;
    bool operation_name_found = 0;

    AWS_LOGF_TRACE(
        AWS_LS_EVENT_STREAM_GENERAL, "processing message headers for rpc protocol. %zu headers to process.", length);

    for (size_t i = 0; i < length; ++i) {
        struct aws_event_stream_header_value_pair *header = NULL;
        aws_array_list_get_at_ptr(message_headers, (void **)&header, i);
        struct aws_byte_buf name_buf = aws_event_stream_header_name(header);
        AWS_LOGF_DEBUG(AWS_LS_EVENT_STREAM_GENERAL, "processing header name " PRInSTR, AWS_BYTE_BUF_PRI(name_buf));

        /* check type first since that's cheaper than a string compare */
        if (header->header_value_type == AWS_EVENT_STREAM_HEADER_INT32) {

            struct aws_byte_buf stream_id_field = aws_byte_buf_from_array(
                aws_event_stream_rpc_stream_id_name.ptr, aws_event_stream_rpc_stream_id_name.len);
            if (aws_byte_buf_eq_ignore_case(&name_buf, &stream_id_field)) {
                *stream_id = aws_event_stream_header_value_as_int32(header);
                AWS_LOGF_DEBUG(AWS_LS_EVENT_STREAM_GENERAL, "stream id header value %" PRId32, *stream_id);
                stream_id_found += 1;
                goto found;
            }

            struct aws_byte_buf message_type_field = aws_byte_buf_from_array(
                aws_event_stream_rpc_message_type_name.ptr, aws_event_stream_rpc_message_type_name.len);
            if (aws_byte_buf_eq_ignore_case(&name_buf, &message_type_field)) {
                *message_type = aws_event_stream_header_value_as_int32(header);
                AWS_LOGF_DEBUG(AWS_LS_EVENT_STREAM_GENERAL, "message type header value %" PRId32, *message_type);
                message_type_found += 1;
                goto found;
            }

            struct aws_byte_buf message_flags_field = aws_byte_buf_from_array(
                aws_event_stream_rpc_message_flags_name.ptr, aws_event_stream_rpc_message_flags_name.len);
            if (aws_byte_buf_eq_ignore_case(&name_buf, &message_flags_field)) {
                *message_flags = aws_event_stream_header_value_as_int32(header);
                AWS_LOGF_DEBUG(AWS_LS_EVENT_STREAM_GENERAL, "message flags header value %" PRId32, *message_flags);
                message_flags_found += 1;
                goto found;
            }
        }

        if (header->header_value_type == AWS_EVENT_STREAM_HEADER_STRING) {
            struct aws_byte_buf operation_field = aws_byte_buf_from_array(
                aws_event_stream_rpc_operation_name.ptr, aws_event_stream_rpc_operation_name.len);

            if (aws_byte_buf_eq_ignore_case(&name_buf, &operation_field)) {
                *operation_name = aws_event_stream_header_value_as_string(header);
                AWS_LOGF_DEBUG(
                    AWS_LS_EVENT_STREAM_GENERAL,
                    "operation name header value" PRInSTR,
                    AWS_BYTE_BUF_PRI(*operation_name));
                operation_name_found += 1;
                goto found;
            }
        }

        continue;

    found:
        if (message_flags_found && message_type_found && stream_id_found && operation_name_found) {
            return AWS_OP_SUCCESS;
        }
    }

    return message_flags_found && message_type_found && stream_id_found
               ? AWS_OP_SUCCESS
               : aws_raise_error(AWS_ERROR_EVENT_STREAM_RPC_PROTOCOL_ERROR);
}

static const uint32_t s_bit_scrambling_magic = 0x45d9f3bU;
static const uint32_t s_bit_shift_magic = 16U;

/* this is a repurposed hash function based on the technique in splitmix64. The magic number was a result of numerical
 * analysis on maximum bit entropy. */
uint64_t aws_event_stream_rpc_hash_streamid(const void *to_hash) {
    uint32_t int_to_hash = *(const uint32_t *)to_hash;
    uint32_t hash = ((int_to_hash >> s_bit_shift_magic) ^ int_to_hash) * s_bit_scrambling_magic;
    hash = ((hash >> s_bit_shift_magic) ^ hash) * s_bit_scrambling_magic;
    hash = (hash >> s_bit_shift_magic) ^ hash;
    return (uint64_t)hash;
}

bool aws_event_stream_rpc_streamid_eq(const void *a, const void *b) {
    return *(const uint32_t *)a == *(const uint32_t *)b;
}
