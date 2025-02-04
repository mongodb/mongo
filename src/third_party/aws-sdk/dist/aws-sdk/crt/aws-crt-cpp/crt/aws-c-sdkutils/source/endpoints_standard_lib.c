/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/host_utils.h>
#include <aws/common/json.h>
#include <aws/common/string.h>
#include <aws/common/uri.h>

#include <aws/sdkutils/private/endpoints_regex.h>
#include <aws/sdkutils/private/endpoints_types_impl.h>
#include <aws/sdkutils/private/endpoints_util.h>
#include <aws/sdkutils/resource_name.h>

static struct aws_byte_cursor s_scheme_http = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("http");
static struct aws_byte_cursor s_scheme_https = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("https");

static int s_resolve_fn_is_set(
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value) {

    int result = AWS_OP_SUCCESS;
    struct aws_endpoints_value argv_value = {0};
    if (aws_array_list_length(argv) != 1 ||
        aws_endpoints_argv_expect(allocator, scope, argv, 0, AWS_ENDPOINTS_VALUE_ANY, &argv_value)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to resolve args for isSet.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    out_value->type = AWS_ENDPOINTS_VALUE_BOOLEAN;
    out_value->v.boolean = argv_value.type != AWS_ENDPOINTS_VALUE_NONE;

on_done:
    aws_endpoints_value_clean_up(&argv_value);
    return result;
}

static int s_resolve_fn_not(
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value) {

    int result = AWS_OP_SUCCESS;
    struct aws_endpoints_value argv_value = {0};
    if (aws_array_list_length(argv) != 1 ||
        aws_endpoints_argv_expect(allocator, scope, argv, 0, AWS_ENDPOINTS_VALUE_BOOLEAN, &argv_value)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to resolve args for not.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    out_value->type = AWS_ENDPOINTS_VALUE_BOOLEAN;
    out_value->v.boolean = !argv_value.v.boolean;

on_done:
    aws_endpoints_value_clean_up(&argv_value);
    return result;
}

static int s_resolve_fn_get_attr(
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value) {

    int result = AWS_OP_SUCCESS;
    struct aws_endpoints_value argv_value = {0};
    struct aws_endpoints_value argv_path = {0};
    if (aws_array_list_length(argv) != 2 ||
        aws_endpoints_argv_expect(allocator, scope, argv, 0, AWS_ENDPOINTS_VALUE_ANY, &argv_value) ||
        aws_endpoints_argv_expect(allocator, scope, argv, 1, AWS_ENDPOINTS_VALUE_STRING, &argv_path)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to resolve args for get attr.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    struct aws_byte_cursor path_cur = argv_path.v.owning_cursor_string.cur;

    if (argv_value.type == AWS_ENDPOINTS_VALUE_OBJECT) {
        if (aws_endpoints_path_through_object(allocator, &argv_value, path_cur, out_value)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to path through object.");
            result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
            goto on_done;
        }
    } else if (argv_value.type == AWS_ENDPOINTS_VALUE_ARRAY) {
        if (aws_endpoints_path_through_array(allocator, scope, &argv_value, path_cur, out_value)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to path through array.");
            result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
            goto on_done;
        }
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Invalid value type for pathing through. type: %d", argv_value.type);
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

on_done:
    aws_endpoints_value_clean_up(&argv_value);
    aws_endpoints_value_clean_up(&argv_path);
    return result;
}

static int s_resolve_fn_substring(
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value) {

    int result = AWS_OP_SUCCESS;
    struct aws_endpoints_value input_value = {0};
    struct aws_endpoints_value start_value = {0};
    struct aws_endpoints_value stop_value = {0};
    struct aws_endpoints_value reverse_value = {0};
    if (aws_array_list_length(argv) != 4 ||
        aws_endpoints_argv_expect(allocator, scope, argv, 0, AWS_ENDPOINTS_VALUE_STRING, &input_value) ||
        aws_endpoints_argv_expect(allocator, scope, argv, 1, AWS_ENDPOINTS_VALUE_NUMBER, &start_value) ||
        aws_endpoints_argv_expect(allocator, scope, argv, 2, AWS_ENDPOINTS_VALUE_NUMBER, &stop_value) ||
        aws_endpoints_argv_expect(allocator, scope, argv, 3, AWS_ENDPOINTS_VALUE_BOOLEAN, &reverse_value)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to resolve args for substring.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    if (start_value.v.number >= stop_value.v.number ||
        input_value.v.owning_cursor_string.cur.len < stop_value.v.number) {
        out_value->type = AWS_ENDPOINTS_VALUE_NONE;
        goto on_done;
    }

    for (size_t idx = 0; idx < input_value.v.owning_cursor_string.cur.len; ++idx) {
        if (input_value.v.owning_cursor_string.cur.ptr[idx] > 127) {
            out_value->type = AWS_ENDPOINTS_VALUE_NONE;
            goto on_done;
        }
    }

    if (!reverse_value.v.boolean) {
        size_t start = (size_t)start_value.v.number;
        size_t end = (size_t)stop_value.v.number;
        struct aws_byte_cursor substring = {
            .ptr = input_value.v.owning_cursor_string.cur.ptr + start,
            .len = end - start,
        };

        out_value->type = AWS_ENDPOINTS_VALUE_STRING;
        out_value->v.owning_cursor_string = aws_endpoints_owning_cursor_from_cursor(allocator, substring);
    } else {
        size_t r_start = input_value.v.owning_cursor_string.cur.len - (size_t)stop_value.v.number;
        size_t r_stop = input_value.v.owning_cursor_string.cur.len - (size_t)start_value.v.number;

        struct aws_byte_cursor substring = {
            .ptr = input_value.v.owning_cursor_string.cur.ptr + r_start,
            .len = r_stop - r_start,
        };
        out_value->type = AWS_ENDPOINTS_VALUE_STRING;
        out_value->v.owning_cursor_string = aws_endpoints_owning_cursor_from_cursor(allocator, substring);
    }

on_done:
    aws_endpoints_value_clean_up(&input_value);
    aws_endpoints_value_clean_up(&start_value);
    aws_endpoints_value_clean_up(&stop_value);
    aws_endpoints_value_clean_up(&reverse_value);
    return result;
}

static int s_resolve_fn_string_equals(
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value) {

    int result = AWS_OP_SUCCESS;
    struct aws_endpoints_value argv_value_1 = {0};
    struct aws_endpoints_value argv_value_2 = {0};
    if (aws_array_list_length(argv) != 2 ||
        aws_endpoints_argv_expect(allocator, scope, argv, 0, AWS_ENDPOINTS_VALUE_STRING, &argv_value_1) ||
        aws_endpoints_argv_expect(allocator, scope, argv, 1, AWS_ENDPOINTS_VALUE_STRING, &argv_value_2)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to resolve stringEquals.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    out_value->type = AWS_ENDPOINTS_VALUE_BOOLEAN;
    out_value->v.boolean =
        aws_byte_cursor_eq(&argv_value_1.v.owning_cursor_string.cur, &argv_value_2.v.owning_cursor_string.cur);

on_done:
    aws_endpoints_value_clean_up(&argv_value_1);
    aws_endpoints_value_clean_up(&argv_value_2);
    return result;
}

static int s_resolve_fn_boolean_equals(
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value) {

    int result = AWS_OP_SUCCESS;
    struct aws_endpoints_value argv_value_1 = {0};
    struct aws_endpoints_value argv_value_2 = {0};
    if (aws_array_list_length(argv) != 2 ||
        aws_endpoints_argv_expect(allocator, scope, argv, 0, AWS_ENDPOINTS_VALUE_BOOLEAN, &argv_value_1) ||
        aws_endpoints_argv_expect(allocator, scope, argv, 1, AWS_ENDPOINTS_VALUE_BOOLEAN, &argv_value_2)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to resolve booleanEquals.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    out_value->type = AWS_ENDPOINTS_VALUE_BOOLEAN;
    out_value->v.boolean = argv_value_1.v.boolean == argv_value_2.v.boolean;

on_done:
    aws_endpoints_value_clean_up(&argv_value_1);
    aws_endpoints_value_clean_up(&argv_value_2);
    return result;
}

static int s_resolve_fn_uri_encode(
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value) {

    int result = AWS_OP_SUCCESS;
    struct aws_byte_buf buf = {0};
    struct aws_endpoints_value argv_value = {0};
    if (aws_array_list_length(argv) != 1 ||
        aws_endpoints_argv_expect(allocator, scope, argv, 0, AWS_ENDPOINTS_VALUE_STRING, &argv_value)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to resolve parameter to uri encode.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    if (aws_byte_buf_init(&buf, allocator, 10)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to resolve parameter to uri encode.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    if (aws_byte_buf_append_encoding_uri_param(&buf, &argv_value.v.owning_cursor_string.cur)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to uri encode value.");
        aws_byte_buf_clean_up(&buf);
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    out_value->type = AWS_ENDPOINTS_VALUE_STRING;
    out_value->v.owning_cursor_string =
        aws_endpoints_owning_cursor_from_string(aws_string_new_from_buf(allocator, &buf));

on_done:
    aws_endpoints_value_clean_up(&argv_value);
    aws_byte_buf_clean_up(&buf);
    return result;
}

static bool s_is_uri_ip(struct aws_byte_cursor host, bool is_uri_encoded) {
    return aws_host_utils_is_ipv4(host) || aws_host_utils_is_ipv6(host, is_uri_encoded);
}

static int s_resolve_fn_parse_url(
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value) {

    int result = AWS_OP_SUCCESS;
    struct aws_uri uri;
    struct aws_json_value *root = NULL;
    struct aws_endpoints_value argv_url = {0};
    if (aws_array_list_length(argv) != 1 ||
        aws_endpoints_argv_expect(allocator, scope, argv, 0, AWS_ENDPOINTS_VALUE_STRING, &argv_url)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to resolve args for parse url.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    if (aws_uri_init_parse(&uri, allocator, &argv_url.v.owning_cursor_string.cur)) {
        out_value->type = AWS_ENDPOINTS_VALUE_NONE;
        /* reset error from parser, since non-uri strings should successfully resolve to none. */
        aws_reset_error();
        goto on_done;
    }

    if (aws_uri_query_string(&uri)->len > 0) {
        out_value->type = AWS_ENDPOINTS_VALUE_NONE;
        goto on_done;
    }

    const struct aws_byte_cursor *scheme = aws_uri_scheme(&uri);
    AWS_ASSERT(scheme != NULL);

    root = aws_json_value_new_object(allocator);

    if (scheme->len == 0) {
        out_value->type = AWS_ENDPOINTS_VALUE_NONE;
        goto on_done;
    }

    if (!(aws_byte_cursor_eq(scheme, &s_scheme_http) || aws_byte_cursor_eq(scheme, &s_scheme_https))) {
        out_value->type = AWS_ENDPOINTS_VALUE_NONE;
        goto on_done;
    }

    if (aws_json_value_add_to_object(
            root, aws_byte_cursor_from_c_str("scheme"), aws_json_value_new_string(allocator, *scheme))) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to add scheme to object.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    const struct aws_byte_cursor *authority = aws_uri_authority(&uri);
    AWS_ASSERT(authority != NULL);

    if (authority->len == 0) {
        out_value->type = AWS_ENDPOINTS_VALUE_NONE;
        goto on_done;
    }

    if (aws_json_value_add_to_object(
            root, aws_byte_cursor_from_c_str("authority"), aws_json_value_new_string(allocator, *authority))) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to add authority to object.");
        goto on_done;
    }

    const struct aws_byte_cursor *path = aws_uri_path(&uri);

    if (aws_json_value_add_to_object(
            root, aws_byte_cursor_from_c_str("path"), aws_json_value_new_string(allocator, *path))) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to add path to object.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    struct aws_byte_cursor normalized_path_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("normalizedPath");
    struct aws_byte_buf normalized_path_buf;
    if (aws_byte_buf_init_from_normalized_uri_path(allocator, *path, &normalized_path_buf) ||
        aws_json_value_add_to_object(
            root,
            normalized_path_cur,
            aws_json_value_new_string(allocator, aws_byte_cursor_from_buf(&normalized_path_buf)))) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to normalize path.");
        aws_byte_buf_clean_up(&normalized_path_buf);
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    aws_byte_buf_clean_up(&normalized_path_buf);

    const struct aws_byte_cursor *host_name = aws_uri_host_name(&uri);
    bool is_ip = s_is_uri_ip(*host_name, true);
    if (aws_json_value_add_to_object(
            root, aws_byte_cursor_from_c_str("isIp"), aws_json_value_new_boolean(allocator, is_ip))) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to add isIp to object.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    struct aws_byte_buf buf;
    if (aws_byte_buf_init(&buf, allocator, 0)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed init buffer for parseUrl return.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    if (aws_byte_buf_append_json_string(root, &buf)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to create JSON object.");
        aws_byte_buf_clean_up(&buf);
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    out_value->type = AWS_ENDPOINTS_VALUE_OBJECT;
    out_value->v.owning_cursor_object =
        aws_endpoints_owning_cursor_from_string(aws_string_new_from_buf(allocator, &buf));

    aws_byte_buf_clean_up(&buf);

on_done:
    aws_uri_clean_up(&uri);
    aws_endpoints_value_clean_up(&argv_url);
    aws_json_value_destroy(root);
    return result;
}

static int s_resolve_is_valid_host_label(
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value) {

    struct aws_endpoints_value argv_value = {0};
    struct aws_endpoints_value argv_allow_subdomains = {0};
    if (aws_array_list_length(argv) != 2 ||
        aws_endpoints_argv_expect(allocator, scope, argv, 0, AWS_ENDPOINTS_VALUE_STRING, &argv_value) ||
        aws_endpoints_argv_expect(allocator, scope, argv, 1, AWS_ENDPOINTS_VALUE_BOOLEAN, &argv_allow_subdomains)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to resolve not.");
        goto on_error;
    }

    out_value->type = AWS_ENDPOINTS_VALUE_BOOLEAN;
    out_value->v.boolean =
        aws_is_valid_host_label(argv_value.v.owning_cursor_string.cur, argv_allow_subdomains.v.boolean);

    aws_endpoints_value_clean_up(&argv_value);
    aws_endpoints_value_clean_up(&argv_allow_subdomains);
    return AWS_OP_SUCCESS;

on_error:
    aws_endpoints_value_clean_up(&argv_value);
    aws_endpoints_value_clean_up(&argv_allow_subdomains);
    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
}

static int s_resolve_fn_aws_partition(
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value) {

    int result = AWS_OP_SUCCESS;
    struct aws_endpoints_value argv_region = {0};

    if (aws_array_list_length(argv) != 1 ||
        aws_endpoints_argv_expect(allocator, scope, argv, 0, AWS_ENDPOINTS_VALUE_STRING, &argv_region)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to resolve arguments for partitions.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    struct aws_hash_element *element = NULL;
    struct aws_byte_cursor key = argv_region.v.owning_cursor_string.cur;
    if (aws_hash_table_find(&scope->partitions->region_to_partition_info, &key, &element)) {
        AWS_LOGF_ERROR(
            AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to find partition info. " PRInSTR, AWS_BYTE_CURSOR_PRI(key));
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    if (element != NULL) {
        out_value->type = AWS_ENDPOINTS_VALUE_OBJECT;
        out_value->v.owning_cursor_object =
            aws_endpoints_owning_cursor_create(allocator, ((struct aws_partition_info *)element->value)->info);
        goto on_done;
    }

    struct aws_byte_cursor partition_cur = {0};
    for (struct aws_hash_iter iter = aws_hash_iter_begin(&scope->partitions->base_partitions);
         !aws_hash_iter_done(&iter);
         aws_hash_iter_next(&iter)) {

        struct aws_partition_info *partition = (struct aws_partition_info *)iter.element.value;

        if (partition->region_regex && aws_endpoints_regex_match(partition->region_regex, key) == AWS_OP_SUCCESS) {
            partition_cur = partition->name;
            break;
        }
    }

    if (partition_cur.len == 0) {
        partition_cur = aws_byte_cursor_from_c_str("aws");
    }

    if (aws_hash_table_find(&scope->partitions->base_partitions, &partition_cur, &element) || element == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to find partition info. " PRInSTR, AWS_BYTE_CURSOR_PRI(key));
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    out_value->type = AWS_ENDPOINTS_VALUE_OBJECT;
    out_value->v.owning_cursor_object =
        aws_endpoints_owning_cursor_create(allocator, ((struct aws_partition_info *)element->value)->info);

on_done:
    aws_endpoints_value_clean_up(&argv_region);
    return result;
}

static int s_resolve_fn_aws_parse_arn(
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value) {

    int result = AWS_OP_SUCCESS;
    struct aws_json_value *object = NULL;
    struct aws_endpoints_value argv_value = {0};
    if (aws_array_list_length(argv) != 1 ||
        aws_endpoints_argv_expect(allocator, scope, argv, 0, AWS_ENDPOINTS_VALUE_STRING, &argv_value)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to resolve parseArn.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    struct aws_resource_name arn;
    if (aws_resource_name_init_from_cur(&arn, &argv_value.v.owning_cursor_string.cur)) {
        out_value->type = AWS_ENDPOINTS_VALUE_NONE;
        goto on_done;
    }

    object = aws_json_value_new_object(allocator);
    if (object == NULL) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to init object for parseArn.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    if (arn.partition.len == 0 || arn.resource_id.len == 0 || arn.service.len == 0) {
        out_value->type = AWS_ENDPOINTS_VALUE_NONE;
        goto on_done;
    }

    /* Split resource id into components, either on : or / */
    /* TODO: support multiple delims in existing split helper? */
    struct aws_json_value *resource_id_node = aws_json_value_new_array(allocator);
    size_t start = 0;
    for (size_t i = 0; i < arn.resource_id.len; ++i) {
        if (arn.resource_id.ptr[i] == '/' || arn.resource_id.ptr[i] == ':') {
            struct aws_byte_cursor cur = {
                .ptr = arn.resource_id.ptr + start,
                .len = i - start,
            };

            struct aws_json_value *element = aws_json_value_new_string(allocator, cur);
            if (element == NULL || aws_json_value_add_array_element(resource_id_node, element)) {
                AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to add resource id element");
                result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
                goto on_done;
            }

            start = i + 1;
        }
    }

    if (start <= arn.resource_id.len) {
        struct aws_byte_cursor cur = {
            .ptr = arn.resource_id.ptr + start,
            .len = arn.resource_id.len - start,
        };
        struct aws_json_value *element = aws_json_value_new_string(allocator, cur);
        if (element == NULL || aws_json_value_add_array_element(resource_id_node, element)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to add resource id element");
            result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
            goto on_done;
        }
    }

    if (aws_json_value_add_to_object(
            object, aws_byte_cursor_from_c_str("partition"), aws_json_value_new_string(allocator, arn.partition)) ||
        aws_json_value_add_to_object(
            object, aws_byte_cursor_from_c_str("service"), aws_json_value_new_string(allocator, arn.service)) ||
        aws_json_value_add_to_object(
            object, aws_byte_cursor_from_c_str("region"), aws_json_value_new_string(allocator, arn.region)) ||
        aws_json_value_add_to_object(
            object, aws_byte_cursor_from_c_str("accountId"), aws_json_value_new_string(allocator, arn.account_id)) ||
        aws_json_value_add_to_object(object, aws_byte_cursor_from_c_str("resourceId"), resource_id_node)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to add elements to object for parseArn.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    out_value->type = AWS_ENDPOINTS_VALUE_OBJECT;
    out_value->v.owning_cursor_object =
        aws_endpoints_owning_cursor_from_string(aws_string_new_from_json(allocator, object));

    if (out_value->v.owning_cursor_object.cur.len == 0) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to create string from json.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

on_done:
    aws_json_value_destroy(object);
    aws_endpoints_value_clean_up(&argv_value);
    return result;
}

static int s_resolve_is_virtual_hostable_s3_bucket(
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value) {

    int result = AWS_OP_SUCCESS;
    struct aws_endpoints_value argv_value = {0};
    struct aws_endpoints_value argv_allow_subdomains = {0};
    if (aws_array_list_length(argv) != 2 ||
        aws_endpoints_argv_expect(allocator, scope, argv, 0, AWS_ENDPOINTS_VALUE_STRING, &argv_value) ||
        aws_endpoints_argv_expect(allocator, scope, argv, 1, AWS_ENDPOINTS_VALUE_BOOLEAN, &argv_allow_subdomains)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Failed to resolve args for isVirtualHostableS3Bucket.");
        result = aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_RESOLVE_FAILED);
        goto on_done;
    }

    struct aws_byte_cursor label_cur = argv_value.v.owning_cursor_string.cur;

    bool has_uppercase_chars = false;
    for (size_t i = 0; i < label_cur.len; ++i) {
        if (label_cur.ptr[i] >= 'A' && label_cur.ptr[i] <= 'Z') {
            has_uppercase_chars = true;
            break;
        }
    }

    out_value->type = AWS_ENDPOINTS_VALUE_BOOLEAN;
    out_value->v.boolean = (label_cur.len >= 3 && label_cur.len <= 63) && !has_uppercase_chars &&
                           aws_is_valid_host_label(label_cur, argv_allow_subdomains.v.boolean) &&
                           !aws_host_utils_is_ipv4(label_cur);

on_done:
    aws_endpoints_value_clean_up(&argv_value);
    aws_endpoints_value_clean_up(&argv_allow_subdomains);
    return result;
}

typedef int(standard_lib_function_fn)(
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value);

static standard_lib_function_fn *s_resolve_fn_vt[AWS_ENDPOINTS_FN_LAST] = {
    [AWS_ENDPOINTS_FN_IS_SET] = s_resolve_fn_is_set,
    [AWS_ENDPOINTS_FN_NOT] = s_resolve_fn_not,
    [AWS_ENDPOINTS_FN_GET_ATTR] = s_resolve_fn_get_attr,
    [AWS_ENDPOINTS_FN_SUBSTRING] = s_resolve_fn_substring,
    [AWS_ENDPOINTS_FN_STRING_EQUALS] = s_resolve_fn_string_equals,
    [AWS_ENDPOINTS_FN_BOOLEAN_EQUALS] = s_resolve_fn_boolean_equals,
    [AWS_ENDPOINTS_FN_URI_ENCODE] = s_resolve_fn_uri_encode,
    [AWS_ENDPOINTS_FN_PARSE_URL] = s_resolve_fn_parse_url,
    [AWS_ENDPOINTS_FN_IS_VALID_HOST_LABEL] = s_resolve_is_valid_host_label,
    [AWS_ENDPOINTS_FN_AWS_PARTITION] = s_resolve_fn_aws_partition,
    [AWS_ENDPOINTS_FN_AWS_PARSE_ARN] = s_resolve_fn_aws_parse_arn,
    [AWS_ENDPOINTS_FN_AWS_IS_VIRTUAL_HOSTABLE_S3_BUCKET] = s_resolve_is_virtual_hostable_s3_bucket,
};

int aws_endpoints_dispatch_standard_lib_fn_resolve(
    enum aws_endpoints_fn_type type,
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value) {
    return s_resolve_fn_vt[type](allocator, argv, scope, out_value);
}
