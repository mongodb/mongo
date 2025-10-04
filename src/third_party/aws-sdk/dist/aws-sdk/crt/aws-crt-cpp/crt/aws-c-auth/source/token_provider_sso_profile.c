/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/credentials.h>

#include <aws/auth/private/aws_profile.h>
#include <aws/auth/private/credentials_utils.h>
#include <aws/auth/private/sso_token_providers.h>
#include <aws/auth/private/sso_token_utils.h>
#include <aws/common/clock.h>

#ifdef _MSC_VER
/* allow non-constant declared initializers. */
#    pragma warning(disable : 4204)
#endif

/*
 * sso-token profile provider implementation
 */
struct aws_token_provider_profile_impl {
    struct aws_string *sso_token_file_path;

    aws_io_clock_fn *system_clock_fn;
};

static int s_token_provider_profile_get_token(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {
    struct aws_token_provider_profile_impl *impl = provider->impl;

    struct aws_sso_token *sso_token = NULL;
    struct aws_credentials *credentials = NULL;
    int result = AWS_OP_ERR;
    sso_token = aws_sso_token_new_from_file(provider->allocator, impl->sso_token_file_path);
    if (!sso_token) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "(id=%p) failed to get sso token from file", (void *)provider);
        goto done;
    }

    /* check token expiration. */
    uint64_t now_ns = UINT64_MAX;
    if (impl->system_clock_fn(&now_ns) != AWS_OP_SUCCESS) {
        goto done;
    }

    if (aws_date_time_as_nanos(&sso_token->expiration) <= now_ns) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "(id=%p) cached sso token is expired.", (void *)provider);
        aws_raise_error(AWS_AUTH_SSO_TOKEN_EXPIRED);
        goto done;
    }

    credentials = aws_credentials_new_token(
        provider->allocator,
        aws_byte_cursor_from_string(sso_token->access_token),
        (uint64_t)aws_date_time_as_epoch_secs(&sso_token->expiration));
    if (!credentials) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "(id=%p) Unable to construct credentials.", (void *)provider);
        goto done;
    }

    callback(credentials, AWS_OP_SUCCESS, user_data);
    result = AWS_OP_SUCCESS;

done:
    aws_sso_token_destroy(sso_token);
    aws_credentials_release(credentials);
    return result;
}

static void s_token_provider_profile_destroy(struct aws_credentials_provider *provider) {
    struct aws_token_provider_profile_impl *impl = provider->impl;
    if (impl == NULL) {
        return;
    }

    aws_string_destroy(impl->sso_token_file_path);

    aws_credentials_provider_invoke_shutdown_callback(provider);
    aws_mem_release(provider->allocator, provider);
}

static struct aws_credentials_provider_vtable s_aws_token_provider_profile_vtable = {
    .get_credentials = s_token_provider_profile_get_token,
    .destroy = s_token_provider_profile_destroy,
};

AWS_STRING_FROM_LITERAL(s_profile_sso_start_url_name, "sso_start_url");

static struct aws_string *s_construct_profile_sso_token_path(
    struct aws_allocator *allocator,
    const struct aws_token_provider_sso_profile_options *options) {

    struct aws_profile_collection *config_collection = NULL;
    struct aws_string *profile_name = NULL;
    struct aws_string *sso_token_path = NULL;

    profile_name = aws_get_profile_name(allocator, &options->profile_name_override);
    if (!profile_name) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "token-provider-sso-profile: failed to resolve profile name");
        aws_raise_error(AWS_AUTH_SSO_TOKEN_PROVIDER_SOURCE_FAILURE);
        goto cleanup;
    }
    if (options->config_file_cached) {
        /* Use cached config file */
        config_collection = aws_profile_collection_acquire(options->config_file_cached);
    } else {
        /* load config file */
        config_collection = aws_load_profile_collection_from_config_file(allocator, options->config_file_name_override);
    }

    if (!config_collection) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "token-provider-sso-profile: could not load or parse"
            " a config file.");
        aws_raise_error(AWS_AUTH_SSO_TOKEN_PROVIDER_SOURCE_FAILURE);
        goto cleanup;
    }

    const struct aws_profile *profile = aws_profile_collection_get_profile(config_collection, profile_name);

    if (!profile) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "token-provider-sso-profile: could not load"
            " a profile at %s.",
            aws_string_c_str(profile_name));
        aws_raise_error(AWS_AUTH_SSO_TOKEN_PROVIDER_SOURCE_FAILURE);
        goto cleanup;
    }

    const struct aws_profile_property *sso_start_url_property =
        aws_profile_get_property(profile, s_profile_sso_start_url_name);

    if (!sso_start_url_property) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "token-provider-sso-profile: failed to find sso_start_url");
        aws_raise_error(AWS_AUTH_SSO_TOKEN_PROVIDER_SOURCE_FAILURE);
        goto cleanup;
    }

    sso_token_path = aws_construct_sso_token_path(allocator, aws_profile_property_get_value(sso_start_url_property));
    if (!sso_token_path) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "token-provider-sso-profile: failed to construct token path");
        goto cleanup;
    }

cleanup:
    aws_string_destroy(profile_name);
    aws_profile_collection_release(config_collection);
    return sso_token_path;
}

struct aws_credentials_provider *aws_token_provider_new_sso_profile(
    struct aws_allocator *allocator,
    const struct aws_token_provider_sso_profile_options *options) {
    struct aws_string *token_path = s_construct_profile_sso_token_path(allocator, options);
    if (!token_path) {
        return NULL;
    }
    struct aws_credentials_provider *provider = NULL;
    struct aws_token_provider_profile_impl *impl = NULL;

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_token_provider_profile_impl));
    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);
    aws_credentials_provider_init_base(provider, allocator, &s_aws_token_provider_profile_vtable, impl);
    impl->sso_token_file_path = aws_string_new_from_string(allocator, token_path);
    provider->shutdown_options = options->shutdown_options;
    if (options->system_clock_fn) {
        impl->system_clock_fn = options->system_clock_fn;
    } else {
        impl->system_clock_fn = aws_sys_clock_get_ticks;
    }

    aws_string_destroy(token_path);
    return provider;
}
