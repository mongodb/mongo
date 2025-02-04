/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/private/system_info_priv.h>

#include <aws/common/logging.h>

void s_destroy_env(void *arg) {
    struct aws_system_environment *env = arg;

    if (env) {
        aws_system_environment_destroy_platform_impl(env);
        aws_mem_release(env->allocator, env);
    }
}

struct aws_system_environment *aws_system_environment_load(struct aws_allocator *allocator) {
    struct aws_system_environment *env = aws_mem_calloc(allocator, 1, sizeof(struct aws_system_environment));
    env->allocator = allocator;
    aws_ref_count_init(&env->ref_count, env, s_destroy_env);

    if (aws_system_environment_load_platform_impl(env)) {
        AWS_LOGF_ERROR(
            AWS_LS_COMMON_GENERAL,
            "id=%p: failed to load system environment with error %s.",
            (void *)env,
            aws_error_debug_str(aws_last_error()));
        goto error;
    }

    AWS_LOGF_TRACE(
        AWS_LS_COMMON_GENERAL,
        "id=%p: virtualization vendor detected as \"" PRInSTR "\"",
        (void *)env,
        AWS_BYTE_CURSOR_PRI(aws_system_environment_get_virtualization_vendor(env)));
    AWS_LOGF_TRACE(
        AWS_LS_COMMON_GENERAL,
        "id=%p: virtualization product name detected as \"" PRInSTR " \"",
        (void *)env,
        AWS_BYTE_CURSOR_PRI(aws_system_environment_get_virtualization_vendor(env)));

    env->os = aws_get_platform_build_os();
    env->cpu_count = aws_system_info_processor_count();
    env->cpu_group_count = aws_get_cpu_group_count();

    return env;
error:
    s_destroy_env(env);
    return NULL;
}

struct aws_system_environment *aws_system_environment_acquire(struct aws_system_environment *env) {
    aws_ref_count_acquire(&env->ref_count);
    return env;
}

void aws_system_environment_release(struct aws_system_environment *env) {
    aws_ref_count_release(&env->ref_count);
}

struct aws_byte_cursor aws_system_environment_get_virtualization_vendor(const struct aws_system_environment *env) {
    struct aws_byte_cursor vendor_string = aws_byte_cursor_from_buf(&env->virtualization_vendor);
    return aws_byte_cursor_trim_pred(&vendor_string, aws_char_is_space);
}

struct aws_byte_cursor aws_system_environment_get_virtualization_product_name(
    const struct aws_system_environment *env) {
    struct aws_byte_cursor product_name_str = aws_byte_cursor_from_buf(&env->product_name);
    return aws_byte_cursor_trim_pred(&product_name_str, aws_char_is_space);
}

size_t aws_system_environment_get_processor_count(struct aws_system_environment *env) {
    return env->cpu_count;
}

AWS_COMMON_API
size_t aws_system_environment_get_cpu_group_count(const struct aws_system_environment *env) {
    return env->cpu_group_count;
}
