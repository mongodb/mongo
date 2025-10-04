/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/signable.h>
#include <aws/common/string.h>
#include <aws/http/request_response.h>

struct aws_signable_trailing_headers_impl {
    struct aws_http_headers *trailing_headers;
    struct aws_array_list headers;
    struct aws_string *previous_signature;
};

static int s_aws_signable_trailing_headers_get_property(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_byte_cursor *out_value) {

    struct aws_signable_trailing_headers_impl *impl = signable->impl;

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

static int s_aws_signable_trailing_headers_get_property_list(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_array_list **out_list) {
    (void)signable;
    (void)name;
    (void)out_list;

    struct aws_signable_trailing_headers_impl *impl = signable->impl;

    *out_list = NULL;

    if (aws_string_eq(name, g_aws_http_headers_property_list_name)) {
        *out_list = &impl->headers;
    } else {
        return aws_raise_error(AWS_ERROR_UNSUPPORTED_OPERATION);
    }

    return AWS_OP_SUCCESS;
}

static int s_aws_signable_trailing_headers_get_payload_stream(
    const struct aws_signable *signable,
    struct aws_input_stream **out_input_stream) {
    (void)signable;
    *out_input_stream = NULL;

    return AWS_OP_SUCCESS;
}

static void s_aws_signable_trailing_headers_destroy(struct aws_signable *signable) {
    if (signable == NULL) {
        return;
    }

    struct aws_signable_trailing_headers_impl *impl = signable->impl;
    if (impl == NULL) {
        return;
    }

    aws_http_headers_release(impl->trailing_headers);
    aws_string_destroy(impl->previous_signature);
    aws_array_list_clean_up(&impl->headers);
    aws_mem_release(signable->allocator, signable);
}

static struct aws_signable_vtable s_signable_trailing_headers_vtable = {
    .get_property = s_aws_signable_trailing_headers_get_property,
    .get_property_list = s_aws_signable_trailing_headers_get_property_list,
    .get_payload_stream = s_aws_signable_trailing_headers_get_payload_stream,
    .destroy = s_aws_signable_trailing_headers_destroy,
};

struct aws_signable *aws_signable_new_trailing_headers(
    struct aws_allocator *allocator,
    struct aws_http_headers *trailing_headers,
    struct aws_byte_cursor previous_signature) {

    struct aws_signable *signable = NULL;
    struct aws_signable_trailing_headers_impl *impl = NULL;
    aws_mem_acquire_many(
        allocator, 2, &signable, sizeof(struct aws_signable), &impl, sizeof(struct aws_signable_trailing_headers_impl));

    AWS_ZERO_STRUCT(*signable);
    AWS_ZERO_STRUCT(*impl);

    /* Keep the headers alive. We're referencing the underlying strings. */
    aws_http_headers_acquire(trailing_headers);
    impl->trailing_headers = trailing_headers;
    signable->allocator = allocator;
    signable->vtable = &s_signable_trailing_headers_vtable;
    signable->impl = impl;

    /*
     * Convert headers list to aws_signable_property_list_pair arraylist since they're not different types.
     */
    size_t header_count = aws_http_headers_count(trailing_headers);
    if (aws_array_list_init_dynamic(
            &impl->headers, allocator, header_count, sizeof(struct aws_signable_property_list_pair))) {
        goto on_error;
    }

    for (size_t i = 0; i < header_count; ++i) {
        struct aws_http_header header;
        aws_http_headers_get_index(trailing_headers, i, &header);

        struct aws_signable_property_list_pair property = {.name = header.name, .value = header.value};
        aws_array_list_push_back(&impl->headers, &property);
    }

    impl->previous_signature = aws_string_new_from_array(allocator, previous_signature.ptr, previous_signature.len);
    if (impl->previous_signature == NULL) {
        goto on_error;
    }

    return signable;

on_error:

    aws_signable_destroy(signable);

    return NULL;
}
