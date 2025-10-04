/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/credentials.h>

#include <aws/auth/private/credentials_utils.h>
#include <aws/common/clock.h>
#include <aws/common/environment.h>
#include <aws/common/logging.h>
#include <aws/common/string.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/io/uri.h>

#define DEFAULT_CREDENTIAL_PROVIDER_REFRESH_MS (15 * 60 * 1000)

#if defined(_MSC_VER)
#    pragma warning(disable : 4204)
/*
 * For designated initialization: .providers = providers,
 * of aws_credentials_provider_chain_options in function
 * aws_credentials_provider_new_chain_default
 */
#    pragma warning(disable : 4221)
#endif /* _MSC_VER */

AWS_STATIC_STRING_FROM_LITERAL(s_ec2_creds_env_disable, "AWS_EC2_METADATA_DISABLED");

/**
 * ECS and IMDS credentials providers are mutually exclusive,
 * ECS has higher priority
 */
static struct aws_credentials_provider *s_aws_credentials_provider_new_ecs_or_imds(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_shutdown_options *shutdown_options,
    struct aws_client_bootstrap *bootstrap,
    struct aws_tls_ctx *tls_ctx) {

    /* Try to create the ECS provider. This will fail if its environment variables aren't set */
    struct aws_credentials_provider_ecs_environment_options ecs_options = {
        .shutdown_options = *shutdown_options,
        .bootstrap = bootstrap,
        .tls_ctx = tls_ctx,
    };
    struct aws_credentials_provider *ecs_provider =
        aws_credentials_provider_new_ecs_from_environment(allocator, &ecs_options);
    if (ecs_provider != NULL) {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "default chain: ECS credentials provider will be used to retrieve credentials");
        return ecs_provider;
    }

    /* Can we do IMDS? */
    struct aws_string *ec2_imds_disable = NULL;
    aws_get_environment_value(allocator, s_ec2_creds_env_disable, &ec2_imds_disable);
    bool imds_enabled = ec2_imds_disable == NULL || aws_string_eq_c_str_ignore_case(ec2_imds_disable, "false");
    aws_string_destroy(ec2_imds_disable);
    if (imds_enabled) {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "default chain: IMDS credentials provider will be used to retrieve credentials");
        struct aws_credentials_provider_imds_options imds_options = {
            .shutdown_options = *shutdown_options,
            .bootstrap = bootstrap,
        };
        return aws_credentials_provider_new_imds(allocator, &imds_options);
    }

    AWS_LOGF_INFO(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER, "default chain: neither ECS nor IMDS will be used to retrieve credentials");
    return NULL;
}

struct default_chain_callback_data {
    struct aws_allocator *allocator;
    struct aws_credentials_provider *default_chain_provider;
    aws_on_get_credentials_callback_fn *original_callback;
    void *original_user_data;
};

static struct default_chain_callback_data *s_create_callback_data(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn *callback,
    void *user_data) {
    struct default_chain_callback_data *callback_data =
        aws_mem_calloc(provider->allocator, 1, sizeof(struct default_chain_callback_data));
    if (callback_data == NULL) {
        return NULL;
    }
    callback_data->allocator = provider->allocator;
    callback_data->default_chain_provider = provider;
    callback_data->original_callback = callback;
    callback_data->original_user_data = user_data;

    aws_credentials_provider_acquire(provider);

    return callback_data;
}

static void s_destroy_callback_data(struct default_chain_callback_data *callback_data) {
    aws_credentials_provider_release(callback_data->default_chain_provider);
    aws_mem_release(callback_data->allocator, callback_data);
}

struct aws_credentials_provider_default_chain_impl {
    struct aws_atomic_var shutdowns_remaining;
    struct aws_credentials_provider *cached_provider;
};

static void s_aws_provider_default_chain_callback(
    struct aws_credentials *credentials,
    int error_code,
    void *user_data) {
    struct default_chain_callback_data *callback_data = user_data;
    struct aws_credentials_provider *provider = callback_data->default_chain_provider;

    if (credentials != NULL) {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Default chain credentials provider successfully sourced credentials",
            (void *)provider);
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Default chain credentials provider failed to source credentials with error %d(%s)",
            (void *)provider,
            error_code,
            aws_error_debug_str(error_code));
    }

    callback_data->original_callback(credentials, error_code, callback_data->original_user_data);
    s_destroy_callback_data(callback_data);
}

static int s_credentials_provider_default_chain_get_credentials_async(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_credentials_provider_default_chain_impl *impl = provider->impl;

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p) Credentials provider chain get credentials dispatch",
        (void *)provider);

    struct default_chain_callback_data *callback_data = s_create_callback_data(provider, callback, user_data);
    if (callback_data == NULL) {
        return AWS_OP_ERR;
    }

    int result = aws_credentials_provider_get_credentials(
        impl->cached_provider, s_aws_provider_default_chain_callback, callback_data);
    if (result != AWS_OP_SUCCESS) {
        s_destroy_callback_data(callback_data);
    }

    return result;
}

static void s_on_sub_provider_shutdown_completed(void *user_data) {
    struct aws_credentials_provider *provider = user_data;
    struct aws_credentials_provider_default_chain_impl *impl = provider->impl;

    size_t remaining = aws_atomic_fetch_sub(&impl->shutdowns_remaining, 1);
    if (remaining != 1) {
        return;
    }

    /* Invoke our own shutdown callback */
    aws_credentials_provider_invoke_shutdown_callback(provider);

    aws_mem_release(provider->allocator, provider);
}

static void s_credentials_provider_default_chain_destroy(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_default_chain_impl *impl = provider->impl;
    if (impl == NULL) {
        return;
    }

    aws_credentials_provider_release(impl->cached_provider);

    s_on_sub_provider_shutdown_completed(provider);
}

static struct aws_credentials_provider_vtable s_aws_credentials_provider_default_chain_vtable = {
    .get_credentials = s_credentials_provider_default_chain_get_credentials_async,
    .destroy = s_credentials_provider_default_chain_destroy,
};

/*
 * Default provider chain implementation
 */
struct aws_credentials_provider *aws_credentials_provider_new_chain_default(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_chain_default_options *options) {

    struct aws_credentials_provider *provider = NULL;
    struct aws_credentials_provider_default_chain_impl *impl = NULL;

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_credentials_provider_default_chain_impl));

    if (!provider) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);

    aws_credentials_provider_init_base(provider, allocator, &s_aws_credentials_provider_default_chain_vtable, impl);
    provider->shutdown_options = options->shutdown_options;

    /* 1 shutdown call from the provider's destroy itself */
    aws_atomic_init_int(&impl->shutdowns_remaining, 1);

    struct aws_credentials_provider_shutdown_options sub_provider_shutdown_options;
    AWS_ZERO_STRUCT(sub_provider_shutdown_options);
    sub_provider_shutdown_options.shutdown_callback = s_on_sub_provider_shutdown_completed;
    sub_provider_shutdown_options.shutdown_user_data = provider;

    struct aws_tls_ctx *tls_ctx = NULL;
    struct aws_credentials_provider *environment_provider = NULL;
    struct aws_credentials_provider *profile_provider = NULL;
    struct aws_credentials_provider *sts_provider = NULL;
    struct aws_credentials_provider *ecs_or_imds_provider = NULL;
    struct aws_credentials_provider *chain_provider = NULL;
    struct aws_credentials_provider *cached_provider = NULL;

    if (options->tls_ctx) {
        tls_ctx = aws_tls_ctx_acquire(options->tls_ctx);
    } else {
#ifdef BYO_CRYPTO
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "TLS context must be provided to credentials provider.");
        goto on_error;
#else
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): TLS context not provided, initializing a new one for credentials provider.",
            (void *)provider);
        struct aws_tls_ctx_options tls_options;
        aws_tls_ctx_options_init_default_client(&tls_options, allocator);
        tls_ctx = aws_tls_client_ctx_new(allocator, &tls_options);
        aws_tls_ctx_options_clean_up(&tls_options);
        if (!tls_ctx) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p): failed to create a TLS context with error %s",
                (void *)provider,
                aws_error_debug_str(aws_last_error()));
            goto on_error;
        }
#endif /* BYO_CRYPTO */
    }

    enum { providers_size = 4 };
    struct aws_credentials_provider *providers[providers_size];
    AWS_ZERO_ARRAY(providers);
    size_t index = 0;

    /* Providers that touch fast local resources... */
    if (!options->skip_environment_credentials_provider) {
        struct aws_credentials_provider_environment_options environment_options;
        AWS_ZERO_STRUCT(environment_options);
        environment_provider = aws_credentials_provider_new_environment(allocator, &environment_options);
        if (environment_provider == NULL) {
            goto on_error;
        }
        providers[index++] = environment_provider;
    }

    /* Providers that will make a network call only if the relevant configuration is present... */

    struct aws_credentials_provider_profile_options profile_options;
    AWS_ZERO_STRUCT(profile_options);
    profile_options.bootstrap = options->bootstrap;
    profile_options.tls_ctx = tls_ctx;
    profile_options.shutdown_options = sub_provider_shutdown_options;
    profile_options.profile_collection_cached = options->profile_collection_cached;
    profile_options.profile_name_override = options->profile_name_override;
    profile_provider = aws_credentials_provider_new_profile(allocator, &profile_options);
    if (profile_provider != NULL) {
        providers[index++] = profile_provider;
        /* 1 shutdown call from the profile provider's shutdown */
        aws_atomic_fetch_add(&impl->shutdowns_remaining, 1);
    }

    struct aws_credentials_provider_sts_web_identity_options sts_options;
    AWS_ZERO_STRUCT(sts_options);
    sts_options.bootstrap = options->bootstrap;
    sts_options.tls_ctx = tls_ctx;
    sts_options.shutdown_options = sub_provider_shutdown_options;
    sts_options.config_profile_collection_cached = options->profile_collection_cached;
    sts_options.profile_name_override = options->profile_name_override;
    sts_provider = aws_credentials_provider_new_sts_web_identity(allocator, &sts_options);
    if (sts_provider != NULL) {
        providers[index++] = sts_provider;
        /* 1 shutdown call from the web identity provider's shutdown */
        aws_atomic_fetch_add(&impl->shutdowns_remaining, 1);
    }

    /* Providers that will always make a network call unless explicitly disabled... */

    ecs_or_imds_provider = s_aws_credentials_provider_new_ecs_or_imds(
        allocator, &sub_provider_shutdown_options, options->bootstrap, tls_ctx);
    if (ecs_or_imds_provider != NULL) {
        providers[index++] = ecs_or_imds_provider;
        /* 1 shutdown call from the imds or ecs provider's shutdown */
        aws_atomic_fetch_add(&impl->shutdowns_remaining, 1);
    }

    AWS_FATAL_ASSERT(index <= providers_size);

    struct aws_credentials_provider_chain_options chain_options = {
        .provider_count = index,
        .providers = providers,
    };

    chain_provider = aws_credentials_provider_new_chain(allocator, &chain_options);
    if (chain_provider == NULL) {
        goto on_error;
    }

    /*
     * Transfer ownership
     */
    aws_credentials_provider_release(environment_provider);
    aws_credentials_provider_release(profile_provider);
    aws_credentials_provider_release(sts_provider);
    aws_credentials_provider_release(ecs_or_imds_provider);

    struct aws_credentials_provider_cached_options cached_options = {
        .source = chain_provider,
        .refresh_time_in_milliseconds = DEFAULT_CREDENTIAL_PROVIDER_REFRESH_MS,
    };

    cached_provider = aws_credentials_provider_new_cached(allocator, &cached_options);
    if (cached_provider == NULL) {
        goto on_error;
    }

    /*
     * Transfer ownership
     */
    aws_credentials_provider_release(chain_provider);

    impl->cached_provider = cached_provider;

    /* Subproviders have their own reference to the tls_ctx now */
    aws_tls_ctx_release(tls_ctx);

    return provider;

on_error:

    /*
     * Have to be a bit more careful than normal with this clean up pattern since the chain/cache will
     * recursively destroy the other providers via ref release.
     *
     * Technically, the cached_provider can never be non-null here, but let's handle it anyways
     * in case someone does something weird in the future.
     */
    if (cached_provider) {
        aws_credentials_provider_release(cached_provider);
    } else if (chain_provider) {
        aws_credentials_provider_release(chain_provider);
    } else {
        aws_credentials_provider_release(ecs_or_imds_provider);
        aws_credentials_provider_release(profile_provider);
        aws_credentials_provider_release(sts_provider);
        aws_credentials_provider_release(environment_provider);
    }

    aws_tls_ctx_release(tls_ctx);

    aws_mem_release(allocator, provider);

    return NULL;
}
