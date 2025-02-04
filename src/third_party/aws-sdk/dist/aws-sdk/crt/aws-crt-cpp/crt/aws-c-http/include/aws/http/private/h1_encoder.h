#ifndef AWS_HTTP_H1_ENCODER_H
#define AWS_HTTP_H1_ENCODER_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/http_impl.h>
#include <aws/http/private/request_response_impl.h>

struct aws_h1_chunk {
    struct aws_allocator *allocator;
    struct aws_input_stream *data;
    uint64_t data_size;
    aws_http1_stream_write_chunk_complete_fn *on_complete;
    void *user_data;
    struct aws_linked_list_node node;
    /* Buffer containing pre-encoded start line: chunk-size [chunk-ext] CRLF */
    struct aws_byte_buf chunk_line;
};

struct aws_h1_trailer {
    struct aws_allocator *allocator;
    struct aws_byte_buf trailer_data;
};

/**
 * Message to be submitted to encoder.
 * Contains data necessary for encoder to write an outgoing request or response.
 */
struct aws_h1_encoder_message {
    /* Upon creation, the "head" (everything preceding body) is buffered here. */
    struct aws_byte_buf outgoing_head_buf;
    /* Single stream used for unchunked body */
    struct aws_input_stream *body;

    /* Pointer to list of `struct aws_h1_chunk`, used for chunked encoding.
     * List is owned by aws_h1_stream.
     * Encoder completes/frees/pops front chunk when it's done sending.
     * If list goes empty, encoder waits for more chunks to arrive.
     * A chunk with data_size=0 means "final chunk" */
    struct aws_linked_list *pending_chunk_list;

    /* Pointer to chunked_trailer, used for chunked_trailer. */
    struct aws_h1_trailer *trailer;

    /* If non-zero, length of unchunked body to send */
    uint64_t content_length;
    bool has_connection_close_header;
    bool has_chunked_encoding_header;
};

enum aws_h1_encoder_state {
    AWS_H1_ENCODER_STATE_INIT,
    AWS_H1_ENCODER_STATE_HEAD,
    AWS_H1_ENCODER_STATE_UNCHUNKED_BODY,
    AWS_H1_ENCODER_STATE_CHUNK_NEXT,
    AWS_H1_ENCODER_STATE_CHUNK_LINE,
    AWS_H1_ENCODER_STATE_CHUNK_BODY,
    AWS_H1_ENCODER_STATE_CHUNK_END,
    AWS_H1_ENCODER_STATE_CHUNK_TRAILER,
    AWS_H1_ENCODER_STATE_DONE,
};

struct aws_h1_encoder {
    struct aws_allocator *allocator;

    enum aws_h1_encoder_state state;
    /* Current message being encoded */
    struct aws_h1_encoder_message *message;
    /* Used by some states to track progress. Reset to 0 whenever state changes */
    uint64_t progress_bytes;
    /* Current chunk */
    struct aws_h1_chunk *current_chunk;
    /* Number of chunks sent, just used for logging */
    size_t chunk_count;
    /* Encoder logs with this stream ptr as the ID, and passes this ptr to the chunk_complete callback */
    struct aws_http_stream *current_stream;
};

struct aws_h1_chunk *aws_h1_chunk_new(struct aws_allocator *allocator, const struct aws_http1_chunk_options *options);
struct aws_h1_trailer *aws_h1_trailer_new(
    struct aws_allocator *allocator,
    const struct aws_http_headers *trailing_headers);

void aws_h1_trailer_destroy(struct aws_h1_trailer *trailer);

/* Just destroy the chunk (don't fire callback) */
void aws_h1_chunk_destroy(struct aws_h1_chunk *chunk);

/* Destroy chunk and fire its completion callback */
void aws_h1_chunk_complete_and_destroy(struct aws_h1_chunk *chunk, struct aws_http_stream *http_stream, int error_code);

int aws_chunk_line_from_options(struct aws_http1_chunk_options *options, struct aws_byte_buf *chunk_line);

AWS_EXTERN_C_BEGIN

/* Validate request and cache any info the encoder will need later in the "encoder message". */
AWS_HTTP_API
int aws_h1_encoder_message_init_from_request(
    struct aws_h1_encoder_message *message,
    struct aws_allocator *allocator,
    const struct aws_http_message *request,
    struct aws_linked_list *pending_chunk_list);

int aws_h1_encoder_message_init_from_response(
    struct aws_h1_encoder_message *message,
    struct aws_allocator *allocator,
    const struct aws_http_message *response,
    bool body_headers_ignored,
    struct aws_linked_list *pending_chunk_list);

AWS_HTTP_API
void aws_h1_encoder_message_clean_up(struct aws_h1_encoder_message *message);

AWS_HTTP_API
void aws_h1_encoder_init(struct aws_h1_encoder *encoder, struct aws_allocator *allocator);

AWS_HTTP_API
void aws_h1_encoder_clean_up(struct aws_h1_encoder *encoder);

AWS_HTTP_API
int aws_h1_encoder_start_message(
    struct aws_h1_encoder *encoder,
    struct aws_h1_encoder_message *message,
    struct aws_http_stream *stream);

AWS_HTTP_API
int aws_h1_encoder_process(struct aws_h1_encoder *encoder, struct aws_byte_buf *out_buf);

AWS_HTTP_API
bool aws_h1_encoder_is_message_in_progress(const struct aws_h1_encoder *encoder);

/* Return true if the encoder is stuck waiting for more chunks to be added to the current message */
AWS_HTTP_API
bool aws_h1_encoder_is_waiting_for_chunks(const struct aws_h1_encoder *encoder);

AWS_EXTERN_C_END

#endif /* AWS_HTTP_H1_ENCODER_H */
