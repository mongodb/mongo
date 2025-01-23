/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/private/credentials_utils.h>

struct aws_credentials_provider_delegate_impl {
    aws_credentials_provider_delegate_get_credentials_fn *get_credentials;
    void *user_data;
};

static int s_credentials_provider_delegate_get_credentials(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *callback_user_data) {

    struct aws_credentials_provider_delegate_impl *impl = provider->impl;
    return impl->get_credentials(impl->user_data, callback, callback_user_data);
}

static void s_credentials_provider_delegate_destroy(struct aws_credentials_provider *provider) {
    aws_credentials_provider_invoke_shutdown_callback(provider);
    aws_mem_release(provider->allocator, provider);
}

static struct aws_credentials_provider_vtable s_credentials_provider_delegate_vtable = {
    .get_credentials = s_credentials_provider_delegate_get_credentials,
    .destroy = s_credentials_provider_delegate_destroy,
};

struct aws_credentials_provider *aws_credentials_provider_new_delegate(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_delegate_options *options) {

    AWS_ASSERT(options);
    AWS_ASSERT(options->get_credentials);

    struct aws_credentials_provider *provider = NULL;
    struct aws_credentials_provider_delegate_impl *impl = NULL;

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_credentials_provider_delegate_impl));

    if (!provider) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);

    aws_credentials_provider_init_base(provider, allocator, &s_credentials_provider_delegate_vtable, impl);
    provider->shutdown_options = options->shutdown_options;

    impl->get_credentials = options->get_credentials;
    impl->user_data = options->delegate_user_data;

    return provider;
}
