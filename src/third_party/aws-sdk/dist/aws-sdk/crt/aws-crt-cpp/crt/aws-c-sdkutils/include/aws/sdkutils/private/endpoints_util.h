/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_SDKUTILS_ENDPOINTS_EVAL_UTIL_H
#define AWS_SDKUTILS_ENDPOINTS_EVAL_UTIL_H

#include <aws/sdkutils/sdkutils.h>

struct aws_string;
struct aws_byte_buf;
struct aws_json_value;

/* Cursor that optionally owns underlying memory. */
struct aws_owning_cursor {
    struct aws_byte_cursor cur;
    struct aws_string *string;
};

/* Clones string and wraps it in owning cursor. */
AWS_SDKUTILS_API struct aws_owning_cursor aws_endpoints_owning_cursor_create(
    struct aws_allocator *allocator,
    const struct aws_string *str);
/* Creates new cursor that takes ownership of created string. */
AWS_SDKUTILS_API struct aws_owning_cursor aws_endpoints_owning_cursor_from_string(struct aws_string *str);
/* Clones memory pointer to by cursor and wraps in owning cursor */
AWS_SDKUTILS_API struct aws_owning_cursor aws_endpoints_owning_cursor_from_cursor(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor cur);
/* Creates owning cursor with memory pointer set to NULL */
AWS_SDKUTILS_API struct aws_owning_cursor aws_endpoints_non_owning_cursor_create(struct aws_byte_cursor cur);

/* Cleans up memory associated with the cursor */
AWS_SDKUTILS_API void aws_owning_cursor_clean_up(struct aws_owning_cursor *cursor);

/*
 * Determine whether label is a valid host label.
 */
AWS_SDKUTILS_API bool aws_is_valid_host_label(struct aws_byte_cursor label, bool allow_subdomains);

/*
 * Normalize uri path - make sure it starts and ends with /
 * Will initialize out_normalized_path.
 * In cases of error out_normalized_path will be uninitialized.
 */
AWS_SDKUTILS_API int aws_byte_buf_init_from_normalized_uri_path(
    struct aws_allocator *allocator,
    struct aws_byte_cursor path,
    struct aws_byte_buf *out_normalized_path);

/*
 * Creates new string from json value.
 * NULL in cases of error.
 */
AWS_SDKUTILS_API struct aws_string *aws_string_new_from_json(
    struct aws_allocator *allocator,
    const struct aws_json_value *value);

/*
 * Convenience helper for comparing byte cursors.
 * Typeless for use with hash tables.
 */
AWS_SDKUTILS_API bool aws_endpoints_byte_cursor_eq(const void *a, const void *b);

/*
 * Helpers to do deep clean up of array list.
 * TODO: move to aws-c-common?
 */
typedef void(aws_array_callback_clean_up_fn)(void *value);
AWS_SDKUTILS_API void aws_array_list_deep_clean_up(
    struct aws_array_list *array,
    aws_array_callback_clean_up_fn on_clean_up_element);

/* Function that resolves template. */
typedef int(aws_endpoints_template_resolve_fn)(
    struct aws_byte_cursor template_cursor,
    void *user_data,
    struct aws_owning_cursor *out_resolved);
/*
 * Resolve templated string and write it out to buf.
 * Will parse templated values (i.e. values enclosed in {}) and replace them with
 * the value returned from resolve_callback.
 * Note: callback must be able to support syntax for pathing through value (path
 * provided after #).
 * Will replace escaped template delimiters ({{ and }}) with single chars.
 * Supports replacing templated values inside json strings (controlled by
 * is_json), by ignoring json { and } chars.
 */
AWS_SDKUTILS_API int aws_byte_buf_init_from_resolved_templated_string(
    struct aws_allocator *allocator,
    struct aws_byte_buf *out_buf,
    struct aws_byte_cursor string,
    aws_endpoints_template_resolve_fn resolve_callback,
    void *user_data,
    bool is_json);

/*
 * Path through json structure and return final json node in out_value.
 * In cases of error, error is returned and out_value is set to NULL.
 * Array access out of bounds returns success, but set out_value to NULL (to be
 * consistent with spec).
 *
 * Path is defined as a string of '.' delimited fields names, that can optionally
 * end with [] to indicate indexing.
 * Note: only last element can be indexed.
 * ex. path "a.b.c[5]" results in going through a, b and then c and finally
 * taking index of 5.
 */
AWS_SDKUTILS_API int aws_path_through_json(
    struct aws_allocator *allocator,
    const struct aws_json_value *root,
    struct aws_byte_cursor path,
    const struct aws_json_value **out_value);

#endif /* AWS_SDKUTILS_ENDPOINTS_EVAL_UTIL_H */
