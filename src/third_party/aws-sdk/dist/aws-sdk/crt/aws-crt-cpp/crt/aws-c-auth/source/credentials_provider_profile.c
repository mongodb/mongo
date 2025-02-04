/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/credentials.h>

#include <aws/auth/private/aws_profile.h>
#include <aws/auth/private/credentials_utils.h>
#include <aws/common/hash_table.h>
#include <aws/common/process.h>
#include <aws/common/string.h>
#include <aws/io/tls_channel_handler.h>

#ifdef _MSC_VER
/* allow non-constant declared initializers. */
#    pragma warning(disable : 4204)
#endif

/*
 * Profile provider implementation
 */

AWS_STRING_FROM_LITERAL(s_role_arn_name, "role_arn");
AWS_STRING_FROM_LITERAL(s_role_session_name_name, "role_session_name");
AWS_STRING_FROM_LITERAL(s_external_id_name, "external_id");
AWS_STRING_FROM_LITERAL(s_credential_source_name, "credential_source");
AWS_STRING_FROM_LITERAL(s_source_profile_name, "source_profile");
AWS_STRING_FROM_LITERAL(s_access_key_id_profile_var, "aws_access_key_id");
AWS_STRING_FROM_LITERAL(s_secret_access_key_profile_var, "aws_secret_access_key");
AWS_STATIC_STRING_FROM_LITERAL(s_credentials_process, "credential_process");

static struct aws_byte_cursor s_default_session_name_pfx =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("aws-common-runtime-profile-config");
static struct aws_byte_cursor s_ec2_imds_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Ec2InstanceMetadata");
static struct aws_byte_cursor s_environment_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Environment");
static struct aws_byte_cursor s_ecs_credentials_provider_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("EcsContainer");

#define MAX_SESSION_NAME_LEN ((size_t)64)

struct aws_credentials_provider_profile_file_impl {
    struct aws_string *config_file_path;
    struct aws_string *credentials_file_path;
    struct aws_string *profile_name;
    struct aws_profile_collection *profile_collection_cached;
};

static int s_profile_file_credentials_provider_get_credentials_async(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_credentials_provider_profile_file_impl *impl = provider->impl;
    struct aws_credentials *credentials = NULL;
    struct aws_profile_collection *merged_profiles = NULL;

    if (impl->profile_collection_cached) {
        /* Use cached profile collection */
        merged_profiles = aws_profile_collection_acquire(impl->profile_collection_cached);
    } else {
        /*
         * Parse config file from file, if it exists
         */
        struct aws_profile_collection *config_profiles =
            aws_profile_collection_new_from_file(provider->allocator, impl->config_file_path, AWS_PST_CONFIG);

        if (config_profiles != NULL) {
            AWS_LOGF_DEBUG(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) Profile credentials provider successfully built config profile collection from file at (%s)",
                (void *)provider,
                aws_string_c_str(impl->config_file_path));
        } else {
            AWS_LOGF_DEBUG(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) Profile credentials provider failed to build config profile collection from file at (%s)",
                (void *)provider,
                aws_string_c_str(impl->config_file_path));
        }

        /*
         * Parse credentials file, if it exists
         */
        struct aws_profile_collection *credentials_profiles =
            aws_profile_collection_new_from_file(provider->allocator, impl->credentials_file_path, AWS_PST_CREDENTIALS);

        if (credentials_profiles != NULL) {
            AWS_LOGF_DEBUG(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) Profile credentials provider successfully built credentials profile collection from file at "
                "(%s)",
                (void *)provider,
                aws_string_c_str(impl->credentials_file_path));
        } else {
            AWS_LOGF_DEBUG(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) Profile credentials provider failed to build credentials profile collection from file at (%s)",
                (void *)provider,
                aws_string_c_str(impl->credentials_file_path));
        }

        /*
         * Merge the (up to) two sources into a single unified profile
         */
        merged_profiles =
            aws_profile_collection_new_from_merge(provider->allocator, config_profiles, credentials_profiles);

        aws_profile_collection_release(config_profiles);
        aws_profile_collection_release(credentials_profiles);
    }

    if (merged_profiles != NULL) {
        const struct aws_profile *profile = aws_profile_collection_get_profile(merged_profiles, impl->profile_name);
        if (profile != NULL) {
            AWS_LOGF_INFO(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) Profile credentials provider attempting to pull credentials from profile \"%s\"",
                (void *)provider,
                aws_string_c_str(impl->profile_name));
            credentials = aws_credentials_new_from_profile(provider->allocator, profile);
        } else {
            AWS_LOGF_INFO(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) Profile credentials provider could not find a profile named \"%s\"",
                (void *)provider,
                aws_string_c_str(impl->profile_name));
        }
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Profile credentials provider failed to merge config and credentials profile collections",
            (void *)provider);
    }

    int error_code = AWS_ERROR_SUCCESS;
    if (credentials == NULL) {
        error_code = aws_last_error();
        if (error_code == AWS_ERROR_SUCCESS) {
            error_code = AWS_AUTH_CREDENTIALS_PROVIDER_PROFILE_SOURCE_FAILURE;
        }
    }

    if (error_code == AWS_ERROR_SUCCESS) {
        AWS_LOGF_INFO(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "Loaded credentials from profile provider");
    } else {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Failed to load credentials from profile provider: %s",
            aws_error_str(error_code));
    }

    callback(credentials, error_code, user_data);

    /*
     * clean up
     */
    aws_credentials_release(credentials);
    aws_profile_collection_release(merged_profiles);

    return AWS_OP_SUCCESS;
}

static void s_profile_file_credentials_provider_destroy(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_profile_file_impl *impl = provider->impl;
    if (impl == NULL) {
        return;
    }

    aws_string_destroy(impl->config_file_path);
    aws_string_destroy(impl->credentials_file_path);
    aws_string_destroy(impl->profile_name);
    aws_profile_collection_release(impl->profile_collection_cached);
    aws_credentials_provider_invoke_shutdown_callback(provider);

    aws_mem_release(provider->allocator, provider);
}

static struct aws_credentials_provider_vtable s_aws_credentials_provider_profile_file_vtable = {
    .get_credentials = s_profile_file_credentials_provider_get_credentials_async,
    .destroy = s_profile_file_credentials_provider_destroy,
};

/* load a purely config/credentials file based provider. */
static struct aws_credentials_provider *s_create_profile_based_provider(
    struct aws_allocator *allocator,
    struct aws_string *credentials_file_path,
    struct aws_string *config_file_path,
    const struct aws_string *profile_name,
    struct aws_profile_collection *profile_collection_cached) {
    struct aws_credentials_provider *provider = NULL;
    struct aws_credentials_provider_profile_file_impl *impl = NULL;

    AWS_LOGF_INFO(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "static: profile %s attempting to create profile-based credentials provider",
        aws_string_c_str(profile_name));

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_credentials_provider_profile_file_impl));

    if (!provider) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);

    aws_credentials_provider_init_base(provider, allocator, &s_aws_credentials_provider_profile_file_vtable, impl);
    if (credentials_file_path) {
        impl->credentials_file_path = aws_string_clone_or_reuse(allocator, credentials_file_path);
    }
    if (config_file_path) {
        impl->config_file_path = aws_string_clone_or_reuse(allocator, config_file_path);
    }
    impl->profile_name = aws_string_clone_or_reuse(allocator, profile_name);
    impl->profile_collection_cached = aws_profile_collection_acquire(profile_collection_cached);
    return provider;
}

static struct aws_credentials_provider *s_create_process_based_provider(
    struct aws_allocator *allocator,
    const struct aws_string *profile_name,
    struct aws_profile_collection *profile_collection) {
    AWS_LOGF_INFO(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "static: profile %s attempting to create process-based credentials provider",
        aws_string_c_str(profile_name));

    struct aws_credentials_provider_process_options options = {
        .profile_to_use = aws_byte_cursor_from_string(profile_name),
        .config_profile_collection_cached = profile_collection,
    };
    return aws_credentials_provider_new_process(allocator, &options);
}

static struct aws_credentials_provider *s_credentials_provider_new_profile_internal(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_profile_options *options,
    struct aws_hash_table *source_profiles_table);

/* use the selected property that specifies a role_arn to load an STS based provider. */
static struct aws_credentials_provider *s_create_sts_based_provider(
    struct aws_allocator *allocator,
    const struct aws_profile_property *role_arn_property,
    const struct aws_profile *profile,
    const struct aws_credentials_provider_profile_options *options,
    struct aws_profile_collection *merged_profiles,
    struct aws_hash_table *source_profiles_table) {
    struct aws_credentials_provider *provider = NULL;

    /* role_arn */
    AWS_LOGF_INFO(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "static: profile %s has role_arn property is set to %s, attempting to "
        "create an STS credentials provider.",
        aws_string_c_str(aws_profile_get_name(profile)),
        aws_string_c_str(aws_profile_property_get_value(role_arn_property)));

    const struct aws_profile_property *source_profile_property =
        aws_profile_get_property(profile, s_source_profile_name);
    const struct aws_profile_property *credential_source_property =
        aws_profile_get_property(profile, s_credential_source_name);

    /* role_session_name */
    const struct aws_profile_property *role_session_name = aws_profile_get_property(profile, s_role_session_name_name);
    char session_name_array[MAX_SESSION_NAME_LEN + 1];
    AWS_ZERO_ARRAY(session_name_array);

    if (role_session_name) {
        size_t to_write = aws_profile_property_get_value(role_session_name)->len;
        if (to_write > MAX_SESSION_NAME_LEN) {
            AWS_LOGF_WARN(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "static: session_name property is %d bytes long, "
                "but the max is %d. Truncating",
                (int)aws_profile_property_get_value(role_session_name)->len,
                (int)MAX_SESSION_NAME_LEN);
            to_write = MAX_SESSION_NAME_LEN;
        }
        memcpy(session_name_array, aws_string_bytes(aws_profile_property_get_value(role_session_name)), to_write);
    } else {
        memcpy(session_name_array, s_default_session_name_pfx.ptr, s_default_session_name_pfx.len);
        snprintf(
            session_name_array + s_default_session_name_pfx.len,
            sizeof(session_name_array) - s_default_session_name_pfx.len,
            "-%d",
            aws_get_pid());
    }

    AWS_LOGF_DEBUG(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "static: computed session_name as %s", session_name_array);

    /* external_id */
    struct aws_byte_cursor external_id = {0};
    const struct aws_profile_property *external_id_property = aws_profile_get_property(profile, s_external_id_name);
    if (external_id_property) {
        external_id = aws_byte_cursor_from_string(aws_profile_property_get_value(external_id_property));
    }

    /* Automatically create a TLS context if necessary. We'd prefer that users pass one in, but can't force
     * them to because aws_credentials_provider_profile_options didn't always have a tls_ctx member. */
    struct aws_tls_ctx *tls_ctx = NULL;
    if (options->tls_ctx) {
        tls_ctx = aws_tls_ctx_acquire(options->tls_ctx);
    } else {
#ifdef BYO_CRYPTO
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "a TLS context must be provided to query STS");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        goto done;
#else
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "TLS context not provided, initializing a new one for querying STS");
        struct aws_tls_ctx_options tls_options;
        aws_tls_ctx_options_init_default_client(&tls_options, allocator);
        tls_ctx = aws_tls_client_ctx_new(allocator, &tls_options);
        aws_tls_ctx_options_clean_up(&tls_options);
        if (!tls_ctx) {
            goto done;
        }
#endif
    }

    struct aws_credentials_provider_sts_options sts_options = {
        .bootstrap = options->bootstrap,
        .tls_ctx = tls_ctx,
        .role_arn = aws_byte_cursor_from_string(aws_profile_property_get_value(role_arn_property)),
        .external_id = external_id,
        .session_name = aws_byte_cursor_from_c_str(session_name_array),
        .profile_name_override = options->profile_name_override,
        .config_file_name_override = options->config_file_name_override,
        .profile_collection_cached = options->profile_collection_cached,
        .duration_seconds = 0,
        .function_table = options->function_table,
    };

    if (source_profile_property) {

        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "static: source_profile set to %s",
            aws_string_c_str(aws_profile_property_get_value(source_profile_property)));

        struct aws_credentials_provider_profile_options profile_provider_options = {
            .bootstrap = options->bootstrap,
            .profile_name_override =
                aws_byte_cursor_from_string(aws_profile_property_get_value(source_profile_property)),
            /* reuse profile collection instead of reading it again */
            .profile_collection_cached = merged_profiles,
            .tls_ctx = options->tls_ctx,
            .function_table = options->function_table,
        };
        sts_options.creds_provider =
            s_credentials_provider_new_profile_internal(allocator, &profile_provider_options, source_profiles_table);

        if (!sts_options.creds_provider) {
            goto done;
        }

        provider = aws_credentials_provider_new_sts(allocator, &sts_options);

        aws_credentials_provider_release(sts_options.creds_provider);

        if (!provider) {
            AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "static: failed to load STS credentials provider");
        }
    } else if (credential_source_property) {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "static: credential_source property set to %s",
            aws_string_c_str(aws_profile_property_get_value(credential_source_property)));

        if (aws_string_eq_byte_cursor_ignore_case(
                aws_profile_property_get_value(credential_source_property), &s_ec2_imds_name)) {
            struct aws_credentials_provider_imds_options imds_options = {
                .bootstrap = options->bootstrap,
                .function_table = options->function_table,
            };

            struct aws_credentials_provider *imds_provider =
                aws_credentials_provider_new_imds(allocator, &imds_options);

            if (!imds_provider) {
                goto done;
            }

            sts_options.creds_provider = imds_provider;
            provider = aws_credentials_provider_new_sts(allocator, &sts_options);

            aws_credentials_provider_release(imds_provider);

        } else if (aws_string_eq_byte_cursor_ignore_case(
                       aws_profile_property_get_value(credential_source_property), &s_environment_name)) {
            struct aws_credentials_provider_environment_options env_options;
            AWS_ZERO_STRUCT(env_options);

            struct aws_credentials_provider *env_provider =
                aws_credentials_provider_new_environment(allocator, &env_options);

            if (!env_provider) {
                goto done;
            }

            sts_options.creds_provider = env_provider;
            provider = aws_credentials_provider_new_sts(allocator, &sts_options);

            aws_credentials_provider_release(env_provider);
        } else if (aws_string_eq_byte_cursor_ignore_case(
                       aws_profile_property_get_value(credential_source_property), &s_ecs_credentials_provider_name)) {
            struct aws_credentials_provider_ecs_environment_options ecs_options = {
                .bootstrap = options->bootstrap,
                .function_table = options->function_table,
                .tls_ctx = tls_ctx,
            };
            struct aws_credentials_provider *ecs_provider =
                aws_credentials_provider_new_ecs_from_environment(allocator, &ecs_options);
            if (!ecs_provider) {
                goto done;
            }
            sts_options.creds_provider = ecs_provider;
            provider = aws_credentials_provider_new_sts(allocator, &sts_options);

            aws_credentials_provider_release(ecs_provider);
        } else {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "static: invalid credential_source property: %s",
                aws_string_c_str(aws_profile_property_get_value(credential_source_property)));
            aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }
    }
done:
    aws_tls_ctx_release(tls_ctx);
    return provider;
}

static struct aws_credentials_provider *s_credentials_provider_new_profile_internal(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_profile_options *options,
    struct aws_hash_table *source_profiles_table) {

    struct aws_credentials_provider *provider = NULL;
    struct aws_profile_collection *config_profiles = NULL;
    struct aws_profile_collection *credentials_profiles = NULL;
    struct aws_profile_collection *merged_profiles = NULL;
    struct aws_string *credentials_file_path = NULL;
    struct aws_string *config_file_path = NULL;
    struct aws_string *profile_name = NULL;
    bool first_profile_in_chain = false;
    if (source_profiles_table == NULL) {
        source_profiles_table = aws_mem_calloc(allocator, 1, sizeof(struct aws_hash_table));
        first_profile_in_chain = true;
        /* source_profiles_table is an hashtable of (char *) -> NULL to detect recursion loop */
        if (aws_hash_table_init(
                source_profiles_table, allocator, 3, aws_hash_c_string, aws_hash_callback_c_str_eq, NULL, NULL)) {
            AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "hash_table_init failed");
            goto on_finished;
        }
    }

    profile_name = aws_get_profile_name(allocator, &options->profile_name_override);
    if (!profile_name) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "static: Profile credentials parser failed to resolve profile name");
        goto on_finished;
    }

    if (options->profile_collection_cached) {
        /* Use cached profile collection */
        merged_profiles = aws_profile_collection_acquire(options->profile_collection_cached);
    } else {
        /* Load profile collection from files */

        credentials_file_path = aws_get_credentials_file_path(allocator, &options->credentials_file_name_override);
        if (!credentials_file_path) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "static: Profile credentials parser failed resolve credentials file path");
            goto on_finished;
        }

        config_file_path = aws_get_config_file_path(allocator, &options->config_file_name_override);
        if (!config_file_path) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER, "static: Profile credentials parser failed resolve config file path");
            goto on_finished;
        }

        config_profiles = aws_profile_collection_new_from_file(allocator, config_file_path, AWS_PST_CONFIG);
        credentials_profiles =
            aws_profile_collection_new_from_file(allocator, credentials_file_path, AWS_PST_CREDENTIALS);

        if (!(config_profiles || credentials_profiles)) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "static: Profile credentials parser could not load or parse"
                " a credentials or config file.");
            goto on_finished;
        }

        merged_profiles = aws_profile_collection_new_from_merge(allocator, config_profiles, credentials_profiles);
    }
    const struct aws_profile *profile = aws_profile_collection_get_profile(merged_profiles, profile_name);

    if (!profile) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "static: Profile credentials provider could not load"
            " a profile at %s.",
            aws_string_c_str(profile_name));
        goto on_finished;
    }
    /* check if sts provider is applicable */
    const struct aws_profile_property *role_arn_property = aws_profile_get_property(profile, s_role_arn_name);
    bool profile_contains_access_key = aws_profile_get_property(profile, s_access_key_id_profile_var) != NULL;
    bool profile_contains_secret_access_key =
        aws_profile_get_property(profile, s_secret_access_key_profile_var) != NULL;
    bool profile_contains_credentials = profile_contains_access_key || profile_contains_secret_access_key;

    struct aws_hash_element *element = NULL;
    if (aws_hash_table_find(source_profiles_table, (void *)aws_string_c_str(profile_name), &element) == AWS_OP_ERR) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "hash_table_find failed");
        goto on_finished;
    }
    if (element != NULL) {
        /* self-reference chain of length 1 is allowed with static credentials */
        if (aws_hash_table_get_entry_count(source_profiles_table) > 1 || !profile_contains_credentials) {
            AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "assumeRole chain contains a circular reference");
            aws_raise_error(AWS_AUTH_PROFILE_STS_CREDENTIALS_PROVIDER_CYCLE_FAILURE);
            goto on_finished;
        }
    }

    aws_hash_table_put(source_profiles_table, (void *)aws_string_c_str(profile_name), NULL, 0);

    /* check if process provider is applicable */
    const struct aws_profile_property *process_property = aws_profile_get_property(profile, s_credentials_process);

    if (role_arn_property && (first_profile_in_chain || !profile_contains_credentials)) {
        provider = s_create_sts_based_provider(
            allocator, role_arn_property, profile, options, merged_profiles, source_profiles_table);
    } else if (process_property) {
        provider = s_create_process_based_provider(allocator, profile_name, merged_profiles);
    } else {
        provider = s_create_profile_based_provider(
            allocator, credentials_file_path, config_file_path, profile_name, options->profile_collection_cached);
    }

on_finished:
    aws_profile_collection_release(config_profiles);
    aws_profile_collection_release(credentials_profiles);
    aws_profile_collection_release(merged_profiles);

    aws_string_destroy(credentials_file_path);
    aws_string_destroy(config_file_path);
    aws_string_destroy(profile_name);
    if (first_profile_in_chain) {
        aws_hash_table_clean_up(source_profiles_table);
        aws_mem_release(allocator, source_profiles_table);
    }
    if (provider) {
        provider->shutdown_options = options->shutdown_options;
    }

    return provider;
}

struct aws_credentials_provider *aws_credentials_provider_new_profile(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_profile_options *options) {

    return s_credentials_provider_new_profile_internal(allocator, options, NULL);
}
