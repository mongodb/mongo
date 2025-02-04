#ifndef AWS_AUTH_TOKEN_PROVIDERS_PRIVATE_H
#define AWS_AUTH_TOKEN_PROVIDERS_PRIVATE_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/auth.h>
#include <aws/auth/credentials.h>

/**
 * Configuration options for a provider that sources sso token information from the aws profile (by default
 * ~/.aws/config) and token from ~/.aws/sso/cache/<sha1 of start url>.json.
 */
struct aws_token_provider_sso_profile_options {
    struct aws_credentials_provider_shutdown_options shutdown_options;

    /*
     * Override of what profile to use to source credentials from ('default' by default)
     */
    struct aws_byte_cursor profile_name_override;

    /*
     * Override path to the profile config file (~/.aws/config by default)
     */
    struct aws_byte_cursor config_file_name_override;

    /**
     * (Optional)
     * Use a cached config profile collection. You can also pass a merged collection.
     * config_file_name_override will be ignored if this option is provided.
     */
    struct aws_profile_collection *config_file_cached;

    /* For mocking, leave NULL otherwise */
    aws_io_clock_fn *system_clock_fn;
};

/**
 * Configuration options for a provider that sources sso token information from the aws profile (by default
 * ~/.aws/config) and token from ~/.aws/sso/cache/<sha1 of session name>.json.
 */
struct aws_token_provider_sso_session_options {
    struct aws_credentials_provider_shutdown_options shutdown_options;

    /*
     * Override of what profile to use to source credentials from ('default' by default)
     */
    struct aws_byte_cursor profile_name_override;

    /*
     * Override path to the profile config file (~/.aws/config by default)
     */
    struct aws_byte_cursor config_file_name_override;

    /**
     * (Optional)
     * Use a cached config profile collection. You can also pass a merged collection.
     * config_file_name_override will be ignored if this option is provided.
     */
    struct aws_profile_collection *config_file_cached;

    /*
     * Connection bootstrap to use for any network connections made
     */
    struct aws_client_bootstrap *bootstrap;

    /*
     * Client TLS context to use for any network connections made.
     */
    struct aws_tls_ctx *tls_ctx;

    /* For mocking, leave NULL otherwise */
    aws_io_clock_fn *system_clock_fn;
};

AWS_EXTERN_C_BEGIN

/**
 * Creates a provider that sources sso token based credentials from key-value profiles loaded from the aws
 * config("~/.aws/config" by default) and ~/.aws/sso/cache/<sha1 of start url>.json
 * This is the legacy way which doesn't support refreshing credentials.
 *
 * @param allocator memory allocator to use for all memory allocation
 * @param options provider-specific configuration options
 *
 * @return the newly-constructed credentials provider, or NULL if an error occurred.
 */
AWS_AUTH_API
struct aws_credentials_provider *aws_token_provider_new_sso_profile(
    struct aws_allocator *allocator,
    const struct aws_token_provider_sso_profile_options *options);

/**
 * Creates a provider that sources sso token based credentials from key-value profiles loaded from the aws
 * config("~/.aws/config" by default) and ~/.aws/sso/cache/<sha1 of session name>.json
 * Note: Token refresh is not currently supported
 *
 * @param allocator memory allocator to use for all memory allocation
 * @param options provider-specific configuration options
 *
 * @return the newly-constructed credentials provider, or NULL if an error occurred.
 */
AWS_AUTH_API
struct aws_credentials_provider *aws_token_provider_new_sso_session(
    struct aws_allocator *allocator,
    const struct aws_token_provider_sso_session_options *options);

AWS_EXTERN_C_END

#endif /* AWS_AUTH_TOKEN_PROVIDERS_PRIVATE_H */
