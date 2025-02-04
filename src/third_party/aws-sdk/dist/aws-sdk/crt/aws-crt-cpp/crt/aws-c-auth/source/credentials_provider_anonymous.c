/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/credentials.h>
#include <aws/auth/private/credentials_utils.h>

static int s_anonymous_credentials_provider_get_credentials_async(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_credentials *credentials = provider->impl;

    AWS_LOGF_INFO(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p) Anonymous credentials provider successfully sourced credentials",
        (void *)provider);
    callback(credentials, AWS_ERROR_SUCCESS, user_data);

    return AWS_OP_SUCCESS;
}

static void s_anonymous_credentials_provider_destroy(struct aws_credentials_provider *provider) {
    struct aws_credentials *credentials = provider->impl;

    aws_credentials_release(credentials);
    aws_credentials_provider_invoke_shutdown_callback(provider);
    aws_mem_release(provider->allocator, provider);
}

static struct aws_credentials_provider_vtable s_aws_credentials_provider_anonymous_vtable = {
    .get_credentials = s_anonymous_credentials_provider_get_credentials_async,
    .destroy = s_anonymous_credentials_provider_destroy,
};

struct aws_credentials_provider *aws_credentials_provider_new_anonymous(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_shutdown_options *shutdown_options) {

    struct aws_credentials_provider *provider = aws_mem_calloc(allocator, 1, sizeof(struct aws_credentials_provider));

    struct aws_credentials *credentials = aws_credentials_new_anonymous(allocator);
    if (credentials == NULL) {
        goto on_new_credentials_failure;
    }

    aws_credentials_provider_init_base(provider, allocator, &s_aws_credentials_provider_anonymous_vtable, credentials);

    if (shutdown_options) {
        provider->shutdown_options = *shutdown_options;
    }

    return provider;

on_new_credentials_failure:

    aws_mem_release(allocator, provider);

    return NULL;
}
