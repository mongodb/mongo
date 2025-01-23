/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/private/sso_token_utils.h>
#include <aws/cal/hash.h>
#include <aws/common/encoding.h>
#include <aws/common/file.h>
#include <aws/common/json.h>

#if defined(_MSC_VER)
#    pragma warning(disable : 4232)
#endif /* _MSC_VER */

struct aws_string *aws_construct_sso_token_path(struct aws_allocator *allocator, const struct aws_string *input) {
    AWS_PRECONDITION(input);

    struct aws_string *sso_token_path_str = NULL;

    struct aws_string *home_directory = aws_get_home_directory(allocator);
    if (!home_directory) {
        return NULL;
    }

    struct aws_byte_cursor home_dir_cursor = aws_byte_cursor_from_string(home_directory);
    struct aws_byte_cursor input_cursor = aws_byte_cursor_from_string(input);
    struct aws_byte_cursor json_cursor = aws_byte_cursor_from_c_str(".json");

    struct aws_byte_buf sso_token_path_buf;
    AWS_ZERO_STRUCT(sso_token_path_buf);
    struct aws_byte_buf sha1_buf;
    AWS_ZERO_STRUCT(sha1_buf);

    /* append home directory */
    if (aws_byte_buf_init_copy_from_cursor(&sso_token_path_buf, allocator, home_dir_cursor)) {
        goto cleanup;
    }

    /* append sso cache directory */
    struct aws_byte_cursor sso_cache_dir_cursor = aws_byte_cursor_from_c_str("/.aws/sso/cache/");
    if (aws_byte_buf_append_dynamic(&sso_token_path_buf, &sso_cache_dir_cursor)) {
        goto cleanup;
    }

    /* append hex encoded sha1 of input */
    if (aws_byte_buf_init(&sha1_buf, allocator, AWS_SHA1_LEN) ||
        aws_sha1_compute(allocator, &input_cursor, &sha1_buf, 0)) {
        goto cleanup;
    }
    struct aws_byte_cursor sha1_cursor = aws_byte_cursor_from_buf(&sha1_buf);
    if (aws_hex_encode_append_dynamic(&sha1_cursor, &sso_token_path_buf)) {
        goto cleanup;
    }

    /* append .json */
    if (aws_byte_buf_append_dynamic(&sso_token_path_buf, &json_cursor)) {
        goto cleanup;
    }

    /* use platform-specific directory separator. */
    aws_normalize_directory_separator(&sso_token_path_buf);

    sso_token_path_str = aws_string_new_from_buf(allocator, &sso_token_path_buf);
    AWS_LOGF_INFO(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "successfully constructed token path: %s",
        aws_string_c_str(sso_token_path_str));
cleanup:
    aws_byte_buf_clean_up(&sso_token_path_buf);
    aws_byte_buf_clean_up(&sha1_buf);
    aws_string_destroy(home_directory);
    return sso_token_path_str;
}

void aws_sso_token_destroy(struct aws_sso_token *sso_token) {
    if (sso_token == NULL) {
        return;
    }

    aws_string_destroy(sso_token->access_token);
    aws_mem_release(sso_token->allocator, sso_token);
}

struct aws_sso_token *aws_sso_token_new_from_file(struct aws_allocator *allocator, const struct aws_string *file_path) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(file_path);

    bool success = false;

    struct aws_sso_token *token = aws_mem_calloc(allocator, 1, sizeof(struct aws_sso_token));
    token->allocator = allocator;
    struct aws_byte_buf file_contents_buf;
    AWS_ZERO_STRUCT(file_contents_buf);
    struct aws_json_value *document_root = NULL;

    if (aws_byte_buf_init_from_file(&file_contents_buf, allocator, aws_string_c_str(file_path))) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "sso token: failed to load token file %s", aws_string_c_str(file_path));
        goto cleanup;
    }

    struct aws_byte_cursor document_cursor = aws_byte_cursor_from_buf(&file_contents_buf);
    document_root = aws_json_value_new_from_string(allocator, document_cursor);
    if (document_root == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "sso token: failed to parse sso token file %s",
            aws_string_c_str(file_path));
        aws_raise_error(AWS_AUTH_SSO_TOKEN_INVALID);
        goto cleanup;
    }

    struct aws_byte_cursor access_token_cursor;
    struct aws_json_value *access_token =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("accessToken"));
    if (!aws_json_value_is_string(access_token) || aws_json_value_get_string(access_token, &access_token_cursor)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "sso token: failed to parse accessToken from %s",
            aws_string_c_str(file_path));
        aws_raise_error(AWS_AUTH_SSO_TOKEN_INVALID);
        goto cleanup;
    }

    struct aws_byte_cursor expires_at_cursor;
    struct aws_json_value *expires_at =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("expiresAt"));
    if (!aws_json_value_is_string(expires_at) || aws_json_value_get_string(expires_at, &expires_at_cursor)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "sso token: failed to parse expiresAt from %s",
            aws_string_c_str(file_path));
        aws_raise_error(AWS_AUTH_SSO_TOKEN_INVALID);
        goto cleanup;
    }
    struct aws_date_time expiration;
    if (aws_date_time_init_from_str_cursor(&expiration, &expires_at_cursor, AWS_DATE_FORMAT_ISO_8601)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "sso token: expiresAt '" PRInSTR "' in %s is not a valid ISO-8601 date string",
            AWS_BYTE_CURSOR_PRI(expires_at_cursor),
            aws_string_c_str(file_path));
        aws_raise_error(AWS_AUTH_SSO_TOKEN_INVALID);
        goto cleanup;
    }
    token->access_token = aws_string_new_from_cursor(allocator, &access_token_cursor);
    token->expiration = expiration;

    success = true;

cleanup:
    aws_json_value_destroy(document_root);
    aws_byte_buf_clean_up(&file_contents_buf);
    if (!success) {
        aws_sso_token_destroy(token);
        token = NULL;
    }
    return token;
}
