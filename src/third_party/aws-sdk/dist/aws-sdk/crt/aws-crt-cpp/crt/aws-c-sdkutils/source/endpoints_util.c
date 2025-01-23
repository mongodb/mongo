/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/json.h>
#include <aws/common/logging.h>
#include <aws/common/string.h>
#include <aws/sdkutils/private/endpoints_util.h>
#include <aws/sdkutils/sdkutils.h>

#include <inttypes.h>

/* arbitrary max length of a region. curent longest region name is 16 chars */
#define AWS_REGION_LEN 50

bool aws_is_valid_host_label(struct aws_byte_cursor label, bool allow_subdomains) {
    bool next_must_be_alnum = true;
    size_t subdomain_count = 0;

    for (size_t i = 0; i < label.len; ++i) {
        if (label.ptr[i] == '.') {
            if (!allow_subdomains || subdomain_count == 0) {
                return false;
            }

            if (!aws_isalnum(label.ptr[i - 1])) {
                return false;
            }

            next_must_be_alnum = true;
            subdomain_count = 0;
            continue;
        }

        if (next_must_be_alnum && !aws_isalnum(label.ptr[i])) {
            return false;
        } else if (label.ptr[i] != '-' && !aws_isalnum(label.ptr[i])) {
            return false;
        }

        next_must_be_alnum = false;
        ++subdomain_count;

        if (subdomain_count > 63) {
            return false;
        }
    }

    return aws_isalnum(label.ptr[label.len - 1]);
}

struct aws_byte_cursor s_path_slash = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("/");

int aws_byte_buf_init_from_normalized_uri_path(
    struct aws_allocator *allocator,
    struct aws_byte_cursor path,
    struct aws_byte_buf *out_normalized_path) {
    /* Normalized path is just regular path that ensures that path starts and ends with slash */

    if (aws_byte_buf_init(out_normalized_path, allocator, path.len + 2)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed init buffer for parseUrl return.");
        goto on_error;
    }

    if (path.len == 0) {
        if (aws_byte_buf_append(out_normalized_path, &s_path_slash)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to add path to object.");
            goto on_error;
        }
        return AWS_OP_SUCCESS;
    }

    if (path.ptr[0] != '/') {
        if (aws_byte_buf_append_dynamic(out_normalized_path, &s_path_slash)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to append slash to normalized path.");
            goto on_error;
        }
    }

    if (aws_byte_buf_append_dynamic(out_normalized_path, &path)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to append path to normalized path.");
        goto on_error;
    }

    if (out_normalized_path->buffer[out_normalized_path->len - 1] != '/') {
        if (aws_byte_buf_append_dynamic(out_normalized_path, &s_path_slash)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to append slash to normalized path.");
            goto on_error;
        }
    }

    return AWS_OP_SUCCESS;

on_error:
    aws_byte_buf_clean_up(out_normalized_path);
    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
}

struct aws_string *aws_string_new_from_json(struct aws_allocator *allocator, const struct aws_json_value *value) {
    struct aws_byte_buf json_blob;
    if (aws_byte_buf_init(&json_blob, allocator, 0)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to init buffer for json conversion.");
        goto on_error;
    }

    if (aws_byte_buf_append_json_string(value, &json_blob)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to convert json to string.");
        goto on_error;
    }

    struct aws_string *ret = aws_string_new_from_buf(allocator, &json_blob);
    aws_byte_buf_clean_up(&json_blob);
    return ret;

on_error:
    aws_byte_buf_clean_up(&json_blob);
    aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
    return NULL;
}

bool aws_endpoints_byte_cursor_eq(const void *a, const void *b) {
    const struct aws_byte_cursor *a_cur = a;
    const struct aws_byte_cursor *b_cur = b;
    return aws_byte_cursor_eq(a_cur, b_cur);
}

void aws_array_list_deep_clean_up(struct aws_array_list *array, aws_array_callback_clean_up_fn on_clean_up_element) {
    for (size_t idx = 0; idx < aws_array_list_length(array); ++idx) {
        void *element = NULL;

        aws_array_list_get_at_ptr(array, &element, idx);
        AWS_ASSERT(element);
        on_clean_up_element(element);
    }

    aws_array_list_clean_up(array);
}

/* TODO: this can be moved into common */
static bool s_split_on_first_delim(
    struct aws_byte_cursor input,
    char split_on,
    struct aws_byte_cursor *out_split,
    struct aws_byte_cursor *out_rest) {
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&input));

    uint8_t *delim = memchr(input.ptr, split_on, input.len);
    if (delim != NULL) {
        out_split->ptr = input.ptr;
        out_split->len = delim - input.ptr;

        out_rest->ptr = delim;
        out_rest->len = input.len - (delim - input.ptr);
        return true;
    }

    *out_split = input;
    out_rest->ptr = NULL;
    out_rest->len = 0;
    return false;
}

static int s_buf_append_and_update_quote_count(
    struct aws_byte_buf *buf,
    struct aws_byte_cursor to_append,
    size_t *quote_count,
    bool is_json) {

    /* Dont count quotes if its not json. escaped quotes will be replaced with
    regular quotes when ruleset json is parsed, which will lead to incorrect
    results for when templates should be resolved in regular strings.
    Note: in json blobs escaped quotes are preserved and bellow approach works. */
    if (is_json) {
        for (size_t idx = 0; idx < to_append.len; ++idx) {
            if (to_append.ptr[idx] == '"' && !(idx > 0 && to_append.ptr[idx - 1] == '\\')) {
                ++*quote_count;
            }
        }
    }
    return aws_byte_buf_append_dynamic(buf, &to_append);
}

static struct aws_byte_cursor escaped_closing_curly = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("}}");
static struct aws_byte_cursor escaped_opening_curly = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("{{");

/*
 * Small helper to deal with escapes correctly in strings that occur before
 * template opening curly. General flow for resolving is to look for opening and
 * then closing curly. This function correctly appends any escaped closing
 * curlies and errors out if closing is not escaped (i.e. its unmatched).
 */
int s_append_template_prefix_to_buffer(
    struct aws_byte_buf *out_buf,
    struct aws_byte_cursor prefix,
    size_t *quote_count,
    bool is_json) {

    struct aws_byte_cursor split = {0};
    struct aws_byte_cursor rest = {0};

    while (s_split_on_first_delim(prefix, '}', &split, &rest)) {
        if (s_buf_append_and_update_quote_count(out_buf, split, quote_count, is_json)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_GENERAL, "Failed to append to resolved template buffer.");
            goto on_error;
        }

        if (*quote_count % 2 == 0) {
            if (aws_byte_buf_append_byte_dynamic(out_buf, '}')) {
                AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_GENERAL, "Failed to append to resolved template buffer.");
                goto on_error;
            }
            aws_byte_cursor_advance(&rest, 1);
            prefix = rest;
            continue;
        }

        if (aws_byte_cursor_starts_with(&rest, &escaped_closing_curly)) {
            if (aws_byte_buf_append_byte_dynamic(out_buf, '}')) {
                AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_GENERAL, "Failed to append to resolved template buffer.");
                goto on_error;
            }
            aws_byte_cursor_advance(&rest, 2);
        } else {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_GENERAL, "Unmatched or unescaped closing curly.");
            goto on_error;
        }

        prefix = rest;
    }

    if (s_buf_append_and_update_quote_count(out_buf, split, quote_count, is_json)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_GENERAL, "Failed to append to resolved template buffer.");
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:
    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
}

int aws_byte_buf_init_from_resolved_templated_string(
    struct aws_allocator *allocator,
    struct aws_byte_buf *out_buf,
    struct aws_byte_cursor string,
    aws_endpoints_template_resolve_fn resolve_callback,
    void *user_data,
    bool is_json) {
    AWS_PRECONDITION(allocator);

    struct aws_owning_cursor resolved_template;
    AWS_ZERO_STRUCT(resolved_template);

    if (aws_byte_buf_init(out_buf, allocator, string.len)) {
        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
    }

    size_t quote_count = is_json ? 0 : 1;
    struct aws_byte_cursor split = {0};
    struct aws_byte_cursor rest = {0};
    while (s_split_on_first_delim(string, '{', &split, &rest)) {
        if (s_append_template_prefix_to_buffer(out_buf, split, &quote_count, is_json)) {
            AWS_LOGF_ERROR(
                AWS_LS_SDKUTILS_ENDPOINTS_GENERAL, "Failed to append to buffer while evaluating templated sting.");
            goto on_error;
        }

        if (quote_count % 2 == 0) {
            if (aws_byte_buf_append_byte_dynamic(out_buf, '{')) {
                AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_GENERAL, "Failed to append to resolved template buffer.");
                goto on_error;
            }
            aws_byte_cursor_advance(&rest, 1);
            string = rest;
            continue;
        }

        if (aws_byte_cursor_starts_with(&rest, &escaped_opening_curly)) {
            if (aws_byte_buf_append_byte_dynamic(out_buf, '{')) {
                AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_GENERAL, "Failed to append to resolved template buffer.");
                goto on_error;
            }
            aws_byte_cursor_advance(&rest, 2);
            string = rest;
            continue;
        }

        aws_byte_cursor_advance(&rest, 1);

        struct aws_byte_cursor after_closing = {0};
        if (!s_split_on_first_delim(rest, '}', &split, &after_closing)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_GENERAL, "Unmatched closing curly.");
            goto on_error;
        }
        aws_byte_cursor_advance(&after_closing, 1);
        string = after_closing;

        if (resolve_callback(split, user_data, &resolved_template)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_GENERAL, "Failed to resolve template.");
            goto on_error;
        }

        if (s_buf_append_and_update_quote_count(out_buf, resolved_template.cur, &quote_count, is_json)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_GENERAL, "Failed to append resolved value.");
            goto on_error;
        }

        aws_owning_cursor_clean_up(&resolved_template);
    }

    if (s_buf_append_and_update_quote_count(out_buf, split, &quote_count, is_json)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_GENERAL, "Failed to append to resolved template buffer.");
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:
    aws_byte_buf_clean_up(out_buf);
    aws_owning_cursor_clean_up(&resolved_template);
    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
}

int aws_path_through_json(
    struct aws_allocator *allocator,
    const struct aws_json_value *root,
    struct aws_byte_cursor path,
    const struct aws_json_value **out_value) {

    struct aws_array_list path_segments;
    if (aws_array_list_init_dynamic(&path_segments, allocator, 10, sizeof(struct aws_byte_cursor)) ||
        aws_byte_cursor_split_on_char(&path, '.', &path_segments)) {
        goto on_error;
    }

    *out_value = root;
    for (size_t idx = 0; idx < aws_array_list_length(&path_segments); ++idx) {
        struct aws_byte_cursor path_el_cur;
        if (aws_array_list_get_at(&path_segments, &path_el_cur, idx)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to get path element");
            goto on_error;
        }

        struct aws_byte_cursor element_cur = {0};
        aws_byte_cursor_next_split(&path_el_cur, '[', &element_cur);

        struct aws_byte_cursor index_cur = {0};
        bool has_index = aws_byte_cursor_next_split(&path_el_cur, '[', &index_cur) &&
                         aws_byte_cursor_next_split(&path_el_cur, ']', &index_cur);

        if (element_cur.len > 0) {
            *out_value = aws_json_value_get_from_object(*out_value, element_cur);
            if (NULL == *out_value) {
                AWS_LOGF_ERROR(
                    AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Invalid path. " PRInSTR ".", AWS_BYTE_CURSOR_PRI(element_cur));
                goto on_error;
            }
        }

        if (has_index) {
            uint64_t index;
            if (aws_byte_cursor_utf8_parse_u64(index_cur, &index)) {
                AWS_LOGF_ERROR(
                    AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE,
                    "Failed to parse index: " PRInSTR,
                    AWS_BYTE_CURSOR_PRI(index_cur));
                goto on_error;
            }
            *out_value = aws_json_get_array_element(*out_value, (size_t)index);
            if (NULL == *out_value) {
                aws_reset_error();
                goto on_success;
            }
        }
    }

on_success:
    aws_array_list_clean_up(&path_segments);
    return AWS_OP_SUCCESS;

on_error:
    aws_array_list_clean_up(&path_segments);
    *out_value = NULL;
    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
}

struct aws_owning_cursor aws_endpoints_owning_cursor_create(
    struct aws_allocator *allocator,
    const struct aws_string *str) {
    struct aws_string *clone = aws_string_clone_or_reuse(allocator, str);
    struct aws_owning_cursor ret = {.string = clone, .cur = aws_byte_cursor_from_string(clone)};
    return ret;
}

struct aws_owning_cursor aws_endpoints_owning_cursor_from_string(struct aws_string *str) {
    struct aws_owning_cursor ret = {.string = str, .cur = aws_byte_cursor_from_string(str)};
    return ret;
}

struct aws_owning_cursor aws_endpoints_owning_cursor_from_cursor(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor cur) {
    struct aws_string *clone = aws_string_new_from_cursor(allocator, &cur);
    struct aws_owning_cursor ret = {.string = clone, .cur = aws_byte_cursor_from_string(clone)};
    return ret;
}

struct aws_owning_cursor aws_endpoints_non_owning_cursor_create(struct aws_byte_cursor cur) {
    struct aws_owning_cursor ret = {.string = NULL, .cur = cur};
    return ret;
}

void aws_owning_cursor_clean_up(struct aws_owning_cursor *cursor) {
    aws_string_destroy(cursor->string);
    cursor->string = NULL;
    cursor->cur.ptr = NULL;
    cursor->cur.len = 0;
}
