/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/signable.h>
#include <aws/common/string.h>
#include <aws/http/request_response.h>

/*
 * This is a simple aws_signable wrapper implementation for the aws_http_message struct
 */
struct aws_signable_http_request_impl {
    struct aws_http_message *request;
    struct aws_array_list headers;
};

static int s_aws_signable_http_request_get_property(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_byte_cursor *out_value) {

    struct aws_signable_http_request_impl *impl = signable->impl;

    AWS_ZERO_STRUCT(*out_value);

    /*
     * uri and method can be queried directly from the wrapper request
     */
    if (aws_string_eq(name, g_aws_http_uri_property_name)) {
        aws_http_message_get_request_path(impl->request, out_value);
    } else if (aws_string_eq(name, g_aws_http_method_property_name)) {
        aws_http_message_get_request_method(impl->request, out_value);
    } else {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    return AWS_OP_SUCCESS;
}

static int s_aws_signable_http_request_get_property_list(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_array_list **out_list) {

    struct aws_signable_http_request_impl *impl = signable->impl;

    *out_list = NULL;

    if (aws_string_eq(name, g_aws_http_headers_property_list_name)) {
        *out_list = &impl->headers;
    } else {
        return aws_raise_error(AWS_ERROR_UNSUPPORTED_OPERATION);
    }

    return AWS_OP_SUCCESS;
}

static int s_aws_signable_http_request_get_payload_stream(
    const struct aws_signable *signable,
    struct aws_input_stream **out_input_stream) {

    struct aws_signable_http_request_impl *impl = signable->impl;
    *out_input_stream = aws_http_message_get_body_stream(impl->request);

    return AWS_OP_SUCCESS;
}

static void s_aws_signable_http_request_destroy(struct aws_signable *signable) {
    if (signable == NULL) {
        return;
    }

    struct aws_signable_http_request_impl *impl = signable->impl;
    if (impl == NULL) {
        return;
    }

    aws_http_message_release(impl->request);
    aws_array_list_clean_up(&impl->headers);
    aws_mem_release(signable->allocator, signable);
}

static struct aws_signable_vtable s_signable_http_request_vtable = {
    .get_property = s_aws_signable_http_request_get_property,
    .get_property_list = s_aws_signable_http_request_get_property_list,
    .get_payload_stream = s_aws_signable_http_request_get_payload_stream,
    .destroy = s_aws_signable_http_request_destroy,
};

struct aws_signable *aws_signable_new_http_request(struct aws_allocator *allocator, struct aws_http_message *request) {

    struct aws_signable *signable = NULL;
    struct aws_signable_http_request_impl *impl = NULL;
    aws_mem_acquire_many(
        allocator, 2, &signable, sizeof(struct aws_signable), &impl, sizeof(struct aws_signable_http_request_impl));

    AWS_ZERO_STRUCT(*signable);
    AWS_ZERO_STRUCT(*impl);

    signable->allocator = allocator;
    signable->vtable = &s_signable_http_request_vtable;
    signable->impl = impl;

    /*
     * Copy the headers since they're not different types
     */
    size_t header_count = aws_http_message_get_header_count(request);
    if (aws_array_list_init_dynamic(
            &impl->headers, allocator, header_count, sizeof(struct aws_signable_property_list_pair))) {
        goto on_error;
    }

    for (size_t i = 0; i < header_count; ++i) {
        struct aws_http_header header;
        aws_http_message_get_header(request, &header, i);

        struct aws_signable_property_list_pair property = {.name = header.name, .value = header.value};
        aws_array_list_push_back(&impl->headers, &property);
    }

    impl->request = aws_http_message_acquire(request);

    return signable;

on_error:

    aws_signable_destroy(signable);

    return NULL;
}
