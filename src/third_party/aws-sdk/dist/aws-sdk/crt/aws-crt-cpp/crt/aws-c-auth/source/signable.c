/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/signable.h>

#include <aws/common/string.h>

void aws_signable_destroy(struct aws_signable *signable) {
    if (signable == NULL) {
        return;
    }

    if (signable->vtable != NULL) {
        signable->vtable->destroy(signable);
    }
}

int aws_signable_get_property(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_byte_cursor *out_value) {

    AWS_ASSERT(signable && signable->vtable && signable->vtable->get_property);

    return signable->vtable->get_property(signable, name, out_value);
}

int aws_signable_get_property_list(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_array_list **out_property_list) {

    AWS_ASSERT(signable && signable->vtable && signable->vtable->get_property_list);

    return signable->vtable->get_property_list(signable, name, out_property_list);
}

int aws_signable_get_payload_stream(const struct aws_signable *signable, struct aws_input_stream **out_input_stream) {

    AWS_ASSERT(signable && signable->vtable && signable->vtable->get_payload_stream);

    return signable->vtable->get_payload_stream(signable, out_input_stream);
}

AWS_STRING_FROM_LITERAL(g_aws_http_headers_property_list_name, "headers");
AWS_STRING_FROM_LITERAL(g_aws_http_query_params_property_list_name, "params");
AWS_STRING_FROM_LITERAL(g_aws_http_method_property_name, "method");
AWS_STRING_FROM_LITERAL(g_aws_http_uri_property_name, "uri");
AWS_STRING_FROM_LITERAL(g_aws_signature_property_name, "signature");
AWS_STRING_FROM_LITERAL(g_aws_previous_signature_property_name, "previous-signature");
AWS_STRING_FROM_LITERAL(g_aws_canonical_request_property_name, "canonical-request");

/*
 * This is a simple aws_signable wrapper implementation for AWS's canonical representation of an http request
 */
struct aws_signable_canonical_request_impl {
    struct aws_string *canonical_request;
};

static int s_aws_signable_canonical_request_get_property(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_byte_cursor *out_value) {

    struct aws_signable_canonical_request_impl *impl = signable->impl;

    AWS_ZERO_STRUCT(*out_value);

    /*
     * uri and method can be queried directly from the wrapper request
     */
    if (aws_string_eq(name, g_aws_canonical_request_property_name)) {
        *out_value = aws_byte_cursor_from_string(impl->canonical_request);
    } else {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    return AWS_OP_SUCCESS;
}

static int s_aws_signable_canonical_request_get_property_list(
    const struct aws_signable *signable,
    const struct aws_string *name,
    struct aws_array_list **out_list) {
    (void)signable;
    (void)name;
    (void)out_list;

    *out_list = NULL;

    return aws_raise_error(AWS_ERROR_UNSUPPORTED_OPERATION);
}

static int s_aws_signable_canonical_request_get_payload_stream(
    const struct aws_signable *signable,
    struct aws_input_stream **out_input_stream) {
    (void)signable;

    *out_input_stream = NULL;

    return AWS_OP_SUCCESS;
}

static void s_aws_signable_canonical_request_destroy(struct aws_signable *signable) {
    if (signable == NULL) {
        return;
    }

    struct aws_signable_canonical_request_impl *impl = signable->impl;
    if (impl == NULL) {
        return;
    }

    aws_string_destroy(impl->canonical_request);

    aws_mem_release(signable->allocator, signable);
}

static struct aws_signable_vtable s_signable_canonical_request_vtable = {
    .get_property = s_aws_signable_canonical_request_get_property,
    .get_property_list = s_aws_signable_canonical_request_get_property_list,
    .get_payload_stream = s_aws_signable_canonical_request_get_payload_stream,
    .destroy = s_aws_signable_canonical_request_destroy,
};

struct aws_signable *aws_signable_new_canonical_request(
    struct aws_allocator *allocator,
    struct aws_byte_cursor canonical_request) {

    struct aws_signable *signable = NULL;
    struct aws_signable_canonical_request_impl *impl = NULL;
    aws_mem_acquire_many(
        allocator,
        2,
        &signable,
        sizeof(struct aws_signable),
        &impl,
        sizeof(struct aws_signable_canonical_request_impl));

    if (signable == NULL || impl == NULL) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*signable);
    AWS_ZERO_STRUCT(*impl);

    signable->allocator = allocator;
    signable->vtable = &s_signable_canonical_request_vtable;
    signable->impl = impl;

    impl->canonical_request = aws_string_new_from_array(allocator, canonical_request.ptr, canonical_request.len);
    if (impl->canonical_request == NULL) {
        goto on_error;
    }

    return signable;

on_error:

    aws_signable_destroy(signable);

    return NULL;
}
