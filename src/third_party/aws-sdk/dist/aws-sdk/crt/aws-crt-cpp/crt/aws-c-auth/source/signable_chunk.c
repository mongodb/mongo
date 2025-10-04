/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/signable.h>
#include <aws/common/string.h>
#include <aws/io/stream.h>

/*
 * This is a simple aws_signable wrapper implementation for an s3 chunk
 */
struct aws_signable_chunk_impl {
    struct aws_input_stream *chunk_data;
    struct aws_string *previous_signature;
};

static int s_aws_signable_chunk_get_property(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_byte_cursor *out_value) {

    struct aws_signable_chunk_impl *impl = signable->impl;

    AWS_ZERO_STRUCT(*out_value);

    /*
     * uri and method can be queried directly from the wrapper request
     */
    if (aws_string_eq(name, g_aws_previous_signature_property_name)) {
        *out_value = aws_byte_cursor_from_string(impl->previous_signature);
    } else {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    return AWS_OP_SUCCESS;
}

static int s_aws_signable_chunk_get_property_list(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_array_list **out_list) {
    (void)signable;
    (void)name;
    (void)out_list;

    return aws_raise_error(AWS_ERROR_UNSUPPORTED_OPERATION);
}

static int s_aws_signable_chunk_get_payload_stream(
    const struct aws_signable *signable,
    struct aws_input_stream **out_input_stream) {

    struct aws_signable_chunk_impl *impl = signable->impl;
    *out_input_stream = impl->chunk_data;

    return AWS_OP_SUCCESS;
}

static void s_aws_signable_chunk_destroy(struct aws_signable *signable) {
    if (signable == NULL) {
        return;
    }

    struct aws_signable_chunk_impl *impl = signable->impl;
    if (impl == NULL) {
        return;
    }
    aws_input_stream_release(impl->chunk_data);
    aws_string_destroy(impl->previous_signature);

    aws_mem_release(signable->allocator, signable);
}

static struct aws_signable_vtable s_signable_chunk_vtable = {
    .get_property = s_aws_signable_chunk_get_property,
    .get_property_list = s_aws_signable_chunk_get_property_list,
    .get_payload_stream = s_aws_signable_chunk_get_payload_stream,
    .destroy = s_aws_signable_chunk_destroy,
};

struct aws_signable *aws_signable_new_chunk(
    struct aws_allocator *allocator,
    struct aws_input_stream *chunk_data,
    struct aws_byte_cursor previous_signature) {

    struct aws_signable *signable = NULL;
    struct aws_signable_chunk_impl *impl = NULL;
    aws_mem_acquire_many(
        allocator, 2, &signable, sizeof(struct aws_signable), &impl, sizeof(struct aws_signable_chunk_impl));

    if (signable == NULL || impl == NULL) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*signable);
    AWS_ZERO_STRUCT(*impl);

    signable->allocator = allocator;
    signable->vtable = &s_signable_chunk_vtable;
    signable->impl = impl;

    impl->chunk_data = aws_input_stream_acquire(chunk_data);
    impl->previous_signature = aws_string_new_from_array(allocator, previous_signature.ptr, previous_signature.len);
    if (impl->previous_signature == NULL) {
        goto on_error;
    }

    return signable;

on_error:

    aws_signable_destroy(signable);

    return NULL;
}
