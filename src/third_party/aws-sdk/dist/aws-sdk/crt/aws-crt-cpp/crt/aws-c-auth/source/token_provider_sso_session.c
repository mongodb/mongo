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
 * sso-session token provider implementation
 */
struct aws_token_provider_sso_session_impl {
    struct aws_string *sso_token_file_path;
    aws_io_clock_fn *system_clock_fn;
};

static int s_token_provider_sso_session_get_token(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_token_provider_sso_session_impl *impl = provider->impl;
    struct aws_sso_token *sso_token = NULL;
    struct aws_credentials *credentials = NULL;
    int result = AWS_OP_ERR;

    sso_token = aws_sso_token_new_from_file(provider->allocator, impl->sso_token_file_path);
    if (!sso_token) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "(id=%p) failed to get sso token from file.", (void *)provider);
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

    /* TODO: Refresh token if it is within refresh window and refreshable */

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

static void s_token_provider_sso_session_destroy(struct aws_credentials_provider *provider) {
    struct aws_token_provider_sso_session_impl *impl = provider->impl;
    if (impl == NULL) {
        return;
    }

    aws_string_destroy(impl->sso_token_file_path);

    aws_credentials_provider_invoke_shutdown_callback(provider);
    aws_mem_release(provider->allocator, provider);
}

static struct aws_credentials_provider_vtable s_aws_token_provider_sso_session_vtable = {
    .get_credentials = s_token_provider_sso_session_get_token,
    .destroy = s_token_provider_sso_session_destroy,
};

AWS_STRING_FROM_LITERAL(s_sso_session_name, "sso_session");
AWS_STRING_FROM_LITERAL(s_sso_region_name, "sso_region");
AWS_STRING_FROM_LITERAL(s_sso_start_url_name, "sso_start_url");

/**
 * Parses the config file to validate and construct a token path. A valid profile with sso session is as follow
 * [profile sso-profile]
 *   sso_session = dev
 *   sso_account_id = 012345678901
 *   sso_role_name = SampleRole
 *
 * [sso-session dev]
 *   sso_region = us-east-1
 *   sso_start_url = https://d-abc123.awsapps.com/start
 */
static struct aws_string *s_verify_config_and_construct_sso_token_path(
    struct aws_allocator *allocator,
    const struct aws_token_provider_sso_session_options *options) {
    struct aws_profile_collection *config_collection = NULL;
    struct aws_string *profile_name = NULL;
    struct aws_string *sso_token_path = NULL;

    profile_name = aws_get_profile_name(allocator, &options->profile_name_override);
    if (!profile_name) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "sso-session: token provider failed to resolve profile name");
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
        goto cleanup;
    }

    const struct aws_profile *profile = aws_profile_collection_get_profile(config_collection, profile_name);

    if (!profile) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "sso-session: token provider could not load"
            " a profile at %s.",
            aws_string_c_str(profile_name));
        goto cleanup;
    }

    const struct aws_profile_property *sso_session_property = aws_profile_get_property(profile, s_sso_session_name);
    if (!sso_session_property) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "token-provider-sso-session: token provider could not find an sso-session at profile %s",
            aws_string_c_str(profile_name));
        aws_raise_error(AWS_AUTH_SSO_TOKEN_PROVIDER_SOURCE_FAILURE);
        goto cleanup;
    }
    const struct aws_string *sso_session_name = aws_profile_property_get_value(sso_session_property);

    /* parse sso_session */
    const struct aws_profile *session_profile =
        aws_profile_collection_get_section(config_collection, AWS_PROFILE_SECTION_TYPE_SSO_SESSION, sso_session_name);
    if (!session_profile) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "token-provider-sso-session: failed to find an sso-session");
        aws_raise_error(AWS_AUTH_SSO_TOKEN_PROVIDER_SOURCE_FAILURE);
        goto cleanup;
    }

    const struct aws_profile_property *sso_region_property =
        aws_profile_get_property(session_profile, s_sso_region_name);
    const struct aws_profile_property *sso_start_url_property =
        aws_profile_get_property(session_profile, s_sso_start_url_name);

    if (!sso_region_property) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "token-provider-sso-session: failed to find sso_region in sso-session");
        aws_raise_error(AWS_AUTH_SSO_TOKEN_PROVIDER_SOURCE_FAILURE);
        goto cleanup;
    }

    if (!sso_start_url_property) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "token-provider-sso-session: failed to find sso_start_url in sso-session");
        aws_raise_error(AWS_AUTH_SSO_TOKEN_PROVIDER_SOURCE_FAILURE);
        goto cleanup;
    }

    /* Verify sso_region & start_url are the same in profile section if they exist */
    const struct aws_string *sso_region = aws_profile_property_get_value(sso_region_property);
    const struct aws_string *sso_start_url = aws_profile_property_get_value(sso_start_url_property);

    const struct aws_profile_property *profile_sso_region_property =
        aws_profile_get_property(profile, s_sso_region_name);
    const struct aws_profile_property *profile_sso_start_url_property =
        aws_profile_get_property(profile, s_sso_start_url_name);

    if (profile_sso_region_property &&
        !aws_string_eq(sso_region, aws_profile_property_get_value(profile_sso_region_property))) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "token-provider-sso-session: profile & sso-session have different value for sso_region");
        aws_raise_error(AWS_AUTH_SSO_TOKEN_PROVIDER_SOURCE_FAILURE);
        goto cleanup;
    }

    if (profile_sso_start_url_property &&
        !aws_string_eq(sso_start_url, aws_profile_property_get_value(profile_sso_start_url_property))) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "token-provider-sso-session: profile & sso-session have different value for sso_start_url");
        aws_raise_error(AWS_AUTH_SSO_TOKEN_PROVIDER_SOURCE_FAILURE);
        goto cleanup;
    }

    sso_token_path = aws_construct_sso_token_path(allocator, sso_session_name);

cleanup:
    aws_string_destroy(profile_name);
    aws_profile_collection_release(config_collection);
    return sso_token_path;
}

struct aws_credentials_provider *aws_token_provider_new_sso_session(
    struct aws_allocator *allocator,
    const struct aws_token_provider_sso_session_options *options) {

    /* Currently, they are not used but they will be required when we implement the refresh token functionality. */
    AWS_ASSERT(options->bootstrap);
    AWS_ASSERT(options->tls_ctx);

    struct aws_string *token_path = s_verify_config_and_construct_sso_token_path(allocator, options);
    if (!token_path) {
        return NULL;
    }
    struct aws_credentials_provider *provider = NULL;
    struct aws_token_provider_sso_session_impl *impl = NULL;

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_token_provider_sso_session_impl));
    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);
    aws_credentials_provider_init_base(provider, allocator, &s_aws_token_provider_sso_session_vtable, impl);
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
