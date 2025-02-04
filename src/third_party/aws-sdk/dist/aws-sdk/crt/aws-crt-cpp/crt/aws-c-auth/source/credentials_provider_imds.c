/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/aws_imds_client.h>
#include <aws/auth/credentials.h>
#include <aws/auth/private/credentials_utils.h>
#include <aws/common/string.h>

#if defined(_MSC_VER)
#    pragma warning(disable : 4204)
#endif /* _MSC_VER */

struct aws_credentials_provider_imds_impl {
    struct aws_imds_client *client;
};

static int s_credentials_provider_imds_get_credentials_async(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data);

static void s_on_imds_client_shutdown(void *user_data);

static void s_credentials_provider_imds_destroy(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_imds_impl *impl = provider->impl;
    if (impl == NULL) {
        return;
    }

    if (impl->client) {
        /* release IMDS client, cleanup will finish when its shutdown callback fires */
        aws_imds_client_release(impl->client);
    } else {
        /* If provider setup failed halfway through, IMDS client might not exist.
         * In this case invoke shutdown completion callback directly to finish cleanup */
        s_on_imds_client_shutdown(provider);
    }
}

static void s_on_imds_client_shutdown(void *user_data) {
    struct aws_credentials_provider *provider = user_data;
    aws_credentials_provider_invoke_shutdown_callback(provider);
    aws_mem_release(provider->allocator, provider);
}

static struct aws_credentials_provider_vtable s_aws_credentials_provider_imds_vtable = {
    .get_credentials = s_credentials_provider_imds_get_credentials_async,
    .destroy = s_credentials_provider_imds_destroy,
};

struct aws_credentials_provider *aws_credentials_provider_new_imds(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_imds_options *options) {

    if (!options->bootstrap) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "Client bootstrap is required for querying IMDS");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_credentials_provider *provider = NULL;
    struct aws_credentials_provider_imds_impl *impl = NULL;

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_credentials_provider_imds_impl));

    if (!provider) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);

    aws_credentials_provider_init_base(provider, allocator, &s_aws_credentials_provider_imds_vtable, impl);

    struct aws_imds_client_options client_options = {
        .bootstrap = options->bootstrap,
        .function_table = options->function_table,
        .imds_version = options->imds_version,
        .ec2_metadata_v1_disabled = options->ec2_metadata_v1_disabled,
        .shutdown_options =
            {
                .shutdown_callback = s_on_imds_client_shutdown,
                .shutdown_user_data = provider,
            },
    };

    impl->client = aws_imds_client_new(allocator, &client_options);
    if (!impl->client) {
        goto on_error;
    }

    provider->shutdown_options = options->shutdown_options;
    return provider;

on_error:
    aws_credentials_provider_destroy(provider);
    return NULL;
}

/*
 * Tracking structure for each outstanding async query to an imds provider
 */
struct imds_provider_user_data {
    /* immutable post-creation */
    struct aws_allocator *allocator;
    struct aws_credentials_provider *imds_provider;
    aws_on_get_credentials_callback_fn *original_callback;
    struct aws_byte_buf role;
    void *original_user_data;
};

static void s_imds_provider_user_data_destroy(struct imds_provider_user_data *user_data) {
    if (user_data == NULL) {
        return;
    }
    aws_byte_buf_clean_up(&user_data->role);
    aws_credentials_provider_release(user_data->imds_provider);
    aws_mem_release(user_data->allocator, user_data);
}

static struct imds_provider_user_data *s_imds_provider_user_data_new(
    struct aws_credentials_provider *imds_provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct imds_provider_user_data *wrapped_user_data =
        aws_mem_calloc(imds_provider->allocator, 1, sizeof(struct imds_provider_user_data));
    if (wrapped_user_data == NULL) {
        goto on_error;
    }
    if (aws_byte_buf_init(&wrapped_user_data->role, imds_provider->allocator, 100)) {
        goto on_error;
    }
    wrapped_user_data->allocator = imds_provider->allocator;
    wrapped_user_data->imds_provider = imds_provider;
    aws_credentials_provider_acquire(imds_provider);
    wrapped_user_data->original_user_data = user_data;
    wrapped_user_data->original_callback = callback;

    return wrapped_user_data;

on_error:
    s_imds_provider_user_data_destroy(wrapped_user_data);
    return NULL;
}

static void s_on_get_credentials(const struct aws_credentials *credentials, int error_code, void *user_data) {
    (void)error_code;
    struct imds_provider_user_data *wrapped_user_data = user_data;
    if (error_code == AWS_OP_SUCCESS) {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "id=%p: IMDS provider successfully retrieved credentials",
            (void *)wrapped_user_data->imds_provider);
    } else {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "id=%p: IMDS provider failed to retrieve credentials: %s",
            (void *)wrapped_user_data->imds_provider,
            aws_error_str(error_code));
    }
    wrapped_user_data->original_callback(
        (struct aws_credentials *)credentials, error_code, wrapped_user_data->original_user_data);
    s_imds_provider_user_data_destroy(wrapped_user_data);
}

static void s_on_get_role(const struct aws_byte_buf *role, int error_code, void *user_data) {
    struct imds_provider_user_data *wrapped_user_data = user_data;
    if (!role || error_code || role->len == 0) {
        goto on_error;
    }

    struct aws_byte_cursor role_cursor = aws_byte_cursor_from_buf(role);
    if (aws_byte_buf_append_dynamic(&wrapped_user_data->role, &role_cursor)) {
        goto on_error;
    }

    struct aws_credentials_provider_imds_impl *impl = wrapped_user_data->imds_provider->impl;
    if (aws_imds_client_get_credentials(
            impl->client, aws_byte_cursor_from_buf(&wrapped_user_data->role), s_on_get_credentials, user_data)) {
        goto on_error;
    }

    return;

on_error:
    AWS_LOGF_INFO(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "id=%p: IMDS provider failed to retrieve role: %s",
        (void *)wrapped_user_data->imds_provider,
        aws_error_str(error_code));
    wrapped_user_data->original_callback(
        NULL, AWS_AUTH_CREDENTIALS_PROVIDER_IMDS_SOURCE_FAILURE, wrapped_user_data->original_user_data);
    s_imds_provider_user_data_destroy(wrapped_user_data);
}

static int s_credentials_provider_imds_get_credentials_async(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER, "id=%p: IMDS provider trying to load credentials", (void *)provider);

    struct aws_credentials_provider_imds_impl *impl = provider->impl;

    struct imds_provider_user_data *wrapped_user_data = s_imds_provider_user_data_new(provider, callback, user_data);
    if (wrapped_user_data == NULL) {
        goto error;
    }

    if (aws_imds_client_get_attached_iam_role(impl->client, s_on_get_role, wrapped_user_data)) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:
    AWS_LOGF_ERROR(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "id=%p: IMDS provider failed to request credentials: %s",
        (void *)provider,
        aws_error_str(aws_last_error()));
    s_imds_provider_user_data_destroy(wrapped_user_data);
    return AWS_OP_ERR;
}
