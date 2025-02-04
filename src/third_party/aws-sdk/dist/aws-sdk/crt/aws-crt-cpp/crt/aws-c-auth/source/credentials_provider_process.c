/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/credentials.h>
#include <aws/auth/private/aws_profile.h>
#include <aws/auth/private/credentials_utils.h>
#include <aws/common/clock.h>
#include <aws/common/date_time.h>
#include <aws/common/environment.h>
#include <aws/common/process.h>
#include <aws/common/string.h>

#if defined(_MSC_VER)
#    pragma warning(disable : 4204)
#endif /* _MSC_VER */

struct aws_credentials_provider_process_impl {
    struct aws_string *command;
};

static int s_get_credentials_from_process(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_credentials_provider_process_impl *impl = provider->impl;
    struct aws_credentials *credentials = NULL;
    struct aws_run_command_options options = {
        .command = aws_string_c_str(impl->command),
    };

    struct aws_run_command_result result;
    if (aws_run_command_result_init(provider->allocator, &result)) {
        goto on_finish;
    }

    if (aws_run_command(provider->allocator, &options, &result) || result.ret_code || !result.std_out) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Failed to source credentials from running process credentials provider with command: %s, err:%s",
            (void *)provider,
            aws_string_c_str(impl->command),
            aws_error_str(aws_last_error()));
        goto on_finish;
    }

    struct aws_parse_credentials_from_json_doc_options parse_options = {
        .access_key_id_name = "AccessKeyId",
        .secret_access_key_name = "SecretAccessKey",
        .token_name = "SessionToken",
        .expiration_name = "Expiration",
        .token_required = false,
        .expiration_required = false,
    };

    credentials = aws_parse_credentials_from_json_document(
        provider->allocator, aws_byte_cursor_from_string(result.std_out), &parse_options);
    if (!credentials) {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Process credentials provider failed to parse credentials from command output (output is not "
            "logged in case sensitive information).",
            (void *)provider);
        goto on_finish;
    }

    AWS_LOGF_INFO(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p) Process credentials provider successfully sourced credentials.",
        (void *)provider);

on_finish:

    ;
    int error_code = AWS_ERROR_SUCCESS;
    if (credentials == NULL) {
        error_code = aws_last_error();
        if (error_code == AWS_ERROR_SUCCESS) {
            error_code = AWS_AUTH_CREDENTIALS_PROVIDER_PROCESS_SOURCE_FAILURE;
        }
    }

    callback(credentials, error_code, user_data);
    aws_run_command_result_cleanup(&result);
    aws_credentials_release(credentials);
    return AWS_OP_SUCCESS;
}

static void s_credentials_provider_process_destroy(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_process_impl *impl = provider->impl;
    if (impl) {
        aws_string_destroy_secure(impl->command);
    }
    aws_credentials_provider_invoke_shutdown_callback(provider);
    aws_mem_release(provider->allocator, provider);
}

AWS_STATIC_STRING_FROM_LITERAL(s_credentials_process, "credential_process");

static struct aws_profile_collection *s_load_profile(struct aws_allocator *allocator) {

    struct aws_profile_collection *config_profiles = NULL;
    struct aws_string *config_file_path = NULL;

    config_file_path = aws_get_config_file_path(allocator, NULL);
    if (!config_file_path) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Failed to resolve config file path during process credentials provider initialization: %s",
            aws_error_str(aws_last_error()));
        goto on_done;
    }

    config_profiles = aws_profile_collection_new_from_file(allocator, config_file_path, AWS_PST_CONFIG);
    if (config_profiles != NULL) {
        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Successfully built config profile collection from file at (%s)",
            aws_string_c_str(config_file_path));
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Failed to build config profile collection from file at (%s) : %s",
            aws_string_c_str(config_file_path),
            aws_error_str(aws_last_error()));
        goto on_done;
    }

on_done:
    aws_string_destroy(config_file_path);
    return config_profiles;
}

static void s_check_or_get_with_profile_config(
    struct aws_allocator *allocator,
    const struct aws_profile *profile,
    const struct aws_string *config_key,
    struct aws_byte_buf *target) {

    if (!allocator || !profile || !config_key || !target) {
        return;
    }
    if (!target->len) {
        aws_byte_buf_clean_up(target);
        const struct aws_profile_property *property = aws_profile_get_property(profile, config_key);
        if (property) {
            aws_byte_buf_init_copy_from_cursor(
                target, allocator, aws_byte_cursor_from_string(aws_profile_property_get_value(property)));
        }
    }
}

/* Redirect stderr to /dev/null
 * As of Sep 2024, aws_process_run() can only capture stdout, and the
 * process's stderr goes into the stderr of the application that launched it.
 * Some credentials-processes log to stderr during normal operation.
 * To prevent this from polluting the application's stderr,
 * we redirect the credential-process's stderr into oblivion.
 *
 * It would be better to fix aws_process_run() so it captures stderr as well,
 * and logging it if the process fails. This is recommended by the SEP:
 * > SDKs SHOULD make this error message accessible to the customer. */
#ifdef _WIN32
static struct aws_byte_cursor s_stderr_redirect_to_devnull = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(" 2> nul");
#else
static struct aws_byte_cursor s_stderr_redirect_to_devnull = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(" 2> /dev/null");
#endif

static struct aws_string *s_get_command(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_process_options *options) {

    struct aws_byte_buf command_buf;
    AWS_ZERO_STRUCT(command_buf);
    struct aws_string *command = NULL;
    struct aws_profile_collection *config_profiles = NULL;
    struct aws_string *profile_name = NULL;
    const struct aws_profile *profile = NULL;
    if (options->config_profile_collection_cached) {
        config_profiles = aws_profile_collection_acquire(options->config_profile_collection_cached);
    } else {
        config_profiles = s_load_profile(allocator);
    }
    profile_name = aws_get_profile_name(allocator, &options->profile_to_use);

    if (config_profiles && profile_name) {
        profile = aws_profile_collection_get_profile(config_profiles, profile_name);
    }

    if (!profile) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Failed to resolve config profile during process credentials provider initialization.");
        goto on_finish;
    }

    s_check_or_get_with_profile_config(allocator, profile, s_credentials_process, &command_buf);

    if (!command_buf.len) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Failed to resolve credentials_process command during process credentials provider initialization.");
        goto on_finish;
    }

    if (aws_byte_buf_append_dynamic(&command_buf, &s_stderr_redirect_to_devnull)) {
        goto on_finish;
    }

    command = aws_string_new_from_array(allocator, command_buf.buffer, command_buf.len);
    if (!command) {
        goto on_finish;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "Successfully loaded credentials_process command for process credentials provider.");

on_finish:
    aws_string_destroy(profile_name);
    aws_profile_collection_release(config_profiles);
    aws_byte_buf_clean_up_secure(&command_buf);
    return command;
}

static struct aws_credentials_provider_vtable s_aws_credentials_provider_process_vtable = {
    .get_credentials = s_get_credentials_from_process,
    .destroy = s_credentials_provider_process_destroy,
};

struct aws_credentials_provider *aws_credentials_provider_new_process(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_process_options *options) {

    struct aws_credentials_provider *provider = NULL;
    struct aws_credentials_provider_process_impl *impl = NULL;

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_credentials_provider_process_impl));

    if (!provider) {
        goto on_error;
    }

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);

    impl->command = s_get_command(allocator, options);
    if (!impl->command) {
        goto on_error;
    }

    aws_credentials_provider_init_base(provider, allocator, &s_aws_credentials_provider_process_vtable, impl);
    provider->shutdown_options = options->shutdown_options;
    AWS_LOGF_TRACE(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): Successfully initializing a process credentials provider.",
        (void *)provider);

    return provider;

on_error:
    aws_mem_release(allocator, provider);
    return NULL;
}
