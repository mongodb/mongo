#ifndef AWS_HTTP_H1_DECODER_H
#define AWS_HTTP_H1_DECODER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/http_impl.h>
#include <aws/http/private/request_response_impl.h>

struct aws_h1_decoded_header {
    /* Name of the header. If the type is `AWS_HTTP_HEADER_NAME_UNKNOWN` then `name_data` must be parsed manually. */
    enum aws_http_header_name name;

    /* Raw buffer storing the header's name. */
    struct aws_byte_cursor name_data;

    /* Raw buffer storing the header's value. */
    struct aws_byte_cursor value_data;

    /* Raw buffer storing the entire header. */
    struct aws_byte_cursor data;
};

struct aws_h1_decoder_vtable {
    /**
     * Called from `aws_h*_decode` when an http header has been received.
     * All pointers are strictly *read only*; any data that needs to persist must be copied out into user-owned memory.
     */
    int (*on_header)(const struct aws_h1_decoded_header *header, void *user_data);

    /**
     * Called from `aws_h1_decode` when a portion of the http body has been received.
     * `finished` is true if this is the last section of the http body, and false if more body data is yet to be
     * received. All pointers are strictly *read only*; any data that needs to persist must be copied out into
     * user-owned memory.
     */
    int (*on_body)(const struct aws_byte_cursor *data, bool finished, void *user_data);

    /* Only needed for requests, can be NULL for responses. */
    int (*on_request)(
        enum aws_http_method method_enum,
        const struct aws_byte_cursor *method_str,
        const struct aws_byte_cursor *uri,
        void *user_data);

    /* Only needed for responses, can be NULL for requests. */
    int (*on_response)(int status_code, void *user_data);

    int (*on_done)(void *user_data);
};

/**
 * Structure used to initialize an `aws_h1_decoder`.
 */
struct aws_h1_decoder_params {
    struct aws_allocator *alloc;
    size_t scratch_space_initial_size;
    /* Set false if decoding responses */
    bool is_decoding_requests;
    void *user_data;
    struct aws_h1_decoder_vtable vtable;
};

struct aws_h1_decoder;

AWS_EXTERN_C_BEGIN

AWS_HTTP_API struct aws_h1_decoder *aws_h1_decoder_new(struct aws_h1_decoder_params *params);
AWS_HTTP_API void aws_h1_decoder_destroy(struct aws_h1_decoder *decoder);
AWS_HTTP_API int aws_h1_decode(struct aws_h1_decoder *decoder, struct aws_byte_cursor *data);

AWS_HTTP_API void aws_h1_decoder_set_logging_id(struct aws_h1_decoder *decoder, const void *id);
AWS_HTTP_API void aws_h1_decoder_set_body_headers_ignored(struct aws_h1_decoder *decoder, bool body_headers_ignored);

/* RFC-7230 section 4.2 Message Format */
#define AWS_HTTP_TRANSFER_ENCODING_CHUNKED (1 << 0)
#define AWS_HTTP_TRANSFER_ENCODING_GZIP (1 << 1)
#define AWS_HTTP_TRANSFER_ENCODING_DEFLATE (1 << 2)
#define AWS_HTTP_TRANSFER_ENCODING_DEPRECATED_COMPRESS (1 << 3)
AWS_HTTP_API int aws_h1_decoder_get_encoding_flags(const struct aws_h1_decoder *decoder);

AWS_HTTP_API uint64_t aws_h1_decoder_get_content_length(const struct aws_h1_decoder *decoder);
AWS_HTTP_API bool aws_h1_decoder_get_body_headers_ignored(const struct aws_h1_decoder *decoder);
AWS_HTTP_API enum aws_http_header_block aws_h1_decoder_get_header_block(const struct aws_h1_decoder *decoder);

AWS_EXTERN_C_END

#endif /* AWS_HTTP_H1_DECODER_H */
