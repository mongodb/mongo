/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/private/s3_list_objects.h>
#include <aws/s3/private/s3_paginator.h>
#include <aws/s3/private/s3_util.h>

#include <aws/common/ref_count.h>
#include <aws/common/xml_parser.h>

#include <aws/io/uri.h>

#include <aws/http/http.h>
#include <aws/http/request_response.h>

struct aws_s3_operation_data {
    struct aws_allocator *allocator;

    struct aws_string *prefix;
    struct aws_string *delimiter;

    struct aws_ref_count ref_count;

    aws_s3_on_object_fn *on_object;

    void *user_data;
};

static void s_ref_count_zero_callback(void *arg) {
    struct aws_s3_operation_data *operation_data = arg;

    if (operation_data->delimiter) {
        aws_string_destroy(operation_data->delimiter);
    }

    if (operation_data->prefix) {
        aws_string_destroy(operation_data->prefix);
    }

    aws_mem_release(operation_data->allocator, operation_data);
}

static void s_on_paginator_cleanup(void *user_data) {
    struct aws_s3_operation_data *operation_data = user_data;

    aws_ref_count_release(&operation_data->ref_count);
}

struct fs_parser_wrapper {
    struct aws_allocator *allocator;
    struct aws_s3_object_info fs_info;
};

/* invoked when the ListBucketResult/Contents node is iterated. */
static int s_on_contents_node(struct aws_xml_node *node, void *user_data) {
    struct fs_parser_wrapper *fs_wrapper = user_data;
    struct aws_s3_object_info *fs_info = &fs_wrapper->fs_info;

    /* for each Contents node, get the info from it and send it off as an object we've encountered */
    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "ETag")) {
        return aws_xml_node_as_body(node, &fs_info->e_tag);
    }

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "Key")) {
        return aws_xml_node_as_body(node, &fs_info->key);
    }

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "LastModified")) {
        struct aws_byte_cursor date_cur;
        if (aws_xml_node_as_body(node, &date_cur) != AWS_OP_SUCCESS) {
            return AWS_OP_ERR;
        }

        if (aws_date_time_init_from_str_cursor(&fs_info->last_modified, &date_cur, AWS_DATE_FORMAT_ISO_8601)) {
            return AWS_OP_ERR;
        }

        return AWS_OP_SUCCESS;
    }

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "Size")) {
        struct aws_byte_cursor size_cur;
        if (aws_xml_node_as_body(node, &size_cur) != AWS_OP_SUCCESS) {
            return AWS_OP_ERR;
        }

        if (aws_byte_cursor_utf8_parse_u64(size_cur, &fs_info->size) != AWS_OP_SUCCESS) {
            return AWS_OP_ERR;
        }

        return AWS_OP_SUCCESS;
    }

    return AWS_OP_SUCCESS;
}

/* invoked when the ListBucketResult/CommonPrefixes node is iterated. */
static int s_on_common_prefixes_node(struct aws_xml_node *node, void *user_data) {
    struct fs_parser_wrapper *fs_wrapper = user_data;

    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "Prefix")) {
        return aws_xml_node_as_body(node, &fs_wrapper->fs_info.prefix);
    }

    return AWS_OP_SUCCESS;
}

static int s_on_list_bucket_result_node_encountered(struct aws_xml_node *node, void *user_data) {
    struct aws_s3_operation_data *operation_data = user_data;

    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);

    struct fs_parser_wrapper fs_wrapper;
    AWS_ZERO_STRUCT(fs_wrapper);

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "Contents")) {
        fs_wrapper.allocator = operation_data->allocator;
        /* this will traverse the current Contents node, get the metadata necessary to construct
         * an instance of fs_info so we can invoke the callback on it. This happens once per object. */
        if (aws_xml_node_traverse(node, s_on_contents_node, &fs_wrapper) != AWS_OP_SUCCESS) {
            return AWS_OP_ERR;
        }

        if (operation_data->prefix && !fs_wrapper.fs_info.prefix.len) {
            fs_wrapper.fs_info.prefix = aws_byte_cursor_from_string(operation_data->prefix);
        }

        struct aws_byte_buf trimmed_etag = aws_replace_quote_entities(fs_wrapper.allocator, fs_wrapper.fs_info.e_tag);
        fs_wrapper.fs_info.e_tag = aws_byte_cursor_from_buf(&trimmed_etag);

        int ret_val = AWS_OP_SUCCESS;
        if (operation_data->on_object) {
            ret_val = operation_data->on_object(&fs_wrapper.fs_info, operation_data->user_data);
        }

        aws_byte_buf_clean_up(&trimmed_etag);

        return ret_val;
    }

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "CommonPrefixes")) {
        /* this will traverse the current CommonPrefixes node, get the metadata necessary to construct
         * an instance of fs_info so we can invoke the callback on it. This happens once per prefix. */
        if (aws_xml_node_traverse(node, s_on_common_prefixes_node, &fs_wrapper) != AWS_OP_SUCCESS) {
            return AWS_OP_ERR;
        }

        int ret_val = AWS_OP_SUCCESS;
        if (operation_data->on_object) {
            ret_val = operation_data->on_object(&fs_wrapper.fs_info, operation_data->user_data);
        }
        return ret_val;
    }

    return AWS_OP_SUCCESS;
}

static int s_construct_next_request_http_message(
    struct aws_byte_cursor *continuation_token,
    void *user_data,
    struct aws_http_message **out_message) {
    AWS_PRECONDITION(user_data);

    struct aws_s3_operation_data *operation_data = user_data;

    struct aws_byte_cursor s_path_start = aws_byte_cursor_from_c_str("/?list-type=2");
    struct aws_byte_buf request_path;
    aws_byte_buf_init_copy_from_cursor(&request_path, operation_data->allocator, s_path_start);

    if (operation_data->prefix) {
        struct aws_byte_cursor s_prefix = aws_byte_cursor_from_c_str("&prefix=");
        aws_byte_buf_append_dynamic(&request_path, &s_prefix);
        struct aws_byte_cursor s_prefix_val = aws_byte_cursor_from_string(operation_data->prefix);
        aws_byte_buf_append_encoding_uri_param(&request_path, &s_prefix_val);
    }

    if (operation_data->delimiter) {
        struct aws_byte_cursor s_delimiter = aws_byte_cursor_from_c_str("&delimiter=");
        aws_byte_buf_append_dynamic(&request_path, &s_delimiter);
        struct aws_byte_cursor s_delimiter_val = aws_byte_cursor_from_string(operation_data->delimiter);
        aws_byte_buf_append_dynamic(&request_path, &s_delimiter_val);
    }

    if (continuation_token) {
        struct aws_byte_cursor s_continuation = aws_byte_cursor_from_c_str("&continuation-token=");
        aws_byte_buf_append_dynamic(&request_path, &s_continuation);
        aws_byte_buf_append_encoding_uri_param(&request_path, continuation_token);
    }

    struct aws_http_message *list_objects_v2_request = aws_http_message_new_request(operation_data->allocator);
    aws_http_message_set_request_path(list_objects_v2_request, aws_byte_cursor_from_buf(&request_path));

    aws_byte_buf_clean_up(&request_path);

    struct aws_http_header accept_header = {
        .name = aws_byte_cursor_from_c_str("accept"),
        .value = aws_byte_cursor_from_c_str("application/xml"),
    };

    aws_http_message_add_header(list_objects_v2_request, accept_header);

    aws_http_message_set_request_method(list_objects_v2_request, aws_http_method_get);

    *out_message = list_objects_v2_request;
    return AWS_OP_SUCCESS;
}

struct aws_s3_paginator *aws_s3_initiate_list_objects(
    struct aws_allocator *allocator,
    const struct aws_s3_list_objects_params *params) {
    AWS_FATAL_PRECONDITION(params);
    AWS_FATAL_PRECONDITION(params->client);
    AWS_FATAL_PRECONDITION(params->bucket_name.len);
    AWS_FATAL_PRECONDITION(params->endpoint.len);

    struct aws_s3_operation_data *operation_data = aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_operation_data));
    operation_data->allocator = allocator;
    operation_data->delimiter =
        params->delimiter.len > 0 ? aws_string_new_from_cursor(allocator, &params->delimiter) : NULL;
    operation_data->prefix = params->prefix.len > 0 ? aws_string_new_from_cursor(allocator, &params->prefix) : NULL;
    operation_data->on_object = params->on_object;
    operation_data->user_data = params->user_data;

    aws_ref_count_init(&operation_data->ref_count, operation_data, s_ref_count_zero_callback);

    struct aws_s3_paginated_operation_params operation_params = {
        .operation_name = aws_byte_cursor_from_c_str("ListObjectsV2"),
        .next_message = s_construct_next_request_http_message,
        .on_result_node_encountered_fn = s_on_list_bucket_result_node_encountered,
        .on_paginated_operation_cleanup = s_on_paginator_cleanup,
        .result_xml_node_name = aws_byte_cursor_from_c_str("ListBucketResult"),
        .continuation_token_node_name = aws_byte_cursor_from_c_str("NextContinuationToken"),
        .user_data = operation_data,
    };

    struct aws_s3_paginated_operation *operation = aws_s3_paginated_operation_new(allocator, &operation_params);

    struct aws_s3_paginator_params paginator_params = {
        .client = params->client,
        .bucket_name = params->bucket_name,
        .endpoint = params->endpoint,
        .on_page_finished_fn = params->on_list_finished,
        .operation = operation,
        .user_data = params->user_data,
    };

    struct aws_s3_paginator *paginator = aws_s3_initiate_paginator(allocator, &paginator_params);

    // transfer control to paginator
    aws_s3_paginated_operation_release(operation);

    return paginator;
}
