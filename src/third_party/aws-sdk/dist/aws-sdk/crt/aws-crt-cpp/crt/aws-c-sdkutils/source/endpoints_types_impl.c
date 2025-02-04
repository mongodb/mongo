/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/array_list.h>
#include <aws/common/hash_table.h>
#include <aws/common/json.h>
#include <aws/common/string.h>
#include <aws/sdkutils/private/endpoints_regex.h>
#include <aws/sdkutils/private/endpoints_types_impl.h>
#include <aws/sdkutils/private/endpoints_util.h>

void s_endpoints_value_clean_up_cb(void *value);

uint64_t aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_LAST];

void aws_endpoints_rule_engine_init(void) {
    aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_IS_SET] = aws_hash_c_string("isSet");
    aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_NOT] = aws_hash_c_string("not");
    aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_GET_ATTR] = aws_hash_c_string("getAttr");
    aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_SUBSTRING] = aws_hash_c_string("substring");
    aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_STRING_EQUALS] = aws_hash_c_string("stringEquals");
    aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_BOOLEAN_EQUALS] = aws_hash_c_string("booleanEquals");
    aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_URI_ENCODE] = aws_hash_c_string("uriEncode");
    aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_PARSE_URL] = aws_hash_c_string("parseURL");
    aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_IS_VALID_HOST_LABEL] = aws_hash_c_string("isValidHostLabel");
    aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_AWS_PARTITION] = aws_hash_c_string("aws.partition");
    aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_AWS_PARSE_ARN] = aws_hash_c_string("aws.parseArn");
    aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_AWS_IS_VIRTUAL_HOSTABLE_S3_BUCKET] =
        aws_hash_c_string("aws.isVirtualHostableS3Bucket");
}

static void s_on_condition_array_element_clean_up(void *element) {
    struct aws_endpoints_condition *condition = element;
    aws_endpoints_condition_clean_up(condition);
}

static void s_on_rule_array_element_clean_up(void *element) {
    struct aws_endpoints_rule *rule = element;
    aws_endpoints_rule_clean_up(rule);
}

static void s_on_expr_array_element_clean_up(void *element) {
    struct aws_endpoints_expr *expr = element;
    aws_endpoints_expr_clean_up(expr);
}

struct aws_partition_info *aws_partition_info_new(struct aws_allocator *allocator, struct aws_byte_cursor name) {
    AWS_PRECONDITION(allocator);
    struct aws_partition_info *partition_info = aws_mem_calloc(allocator, 1, sizeof(struct aws_partition_info));

    partition_info->allocator = allocator;
    partition_info->name = name;

    return partition_info;
}

void aws_partition_info_destroy(struct aws_partition_info *partition_info) {
    if (partition_info == NULL) {
        return;
    }

    if (!partition_info->is_copy) {
        aws_string_destroy(partition_info->info);
    }

    aws_endpoints_regex_destroy(partition_info->region_regex);

    aws_mem_release(partition_info->allocator, partition_info);
}

struct aws_endpoints_parameter *aws_endpoints_parameter_new(
    struct aws_allocator *allocator,
    struct aws_byte_cursor name) {
    AWS_PRECONDITION(allocator);
    struct aws_endpoints_parameter *parameter = aws_mem_calloc(allocator, 1, sizeof(struct aws_endpoints_parameter));

    parameter->allocator = allocator;
    parameter->name = name;

    return parameter;
}

void aws_endpoints_parameter_destroy(struct aws_endpoints_parameter *parameter) {
    if (parameter == NULL) {
        return;
    }

    if (parameter->has_default_value && parameter->type == AWS_ENDPOINTS_PARAMETER_STRING_ARRAY) {
        aws_array_list_deep_clean_up(&parameter->default_value.v.array, s_endpoints_value_clean_up_cb);
    }

    aws_mem_release(parameter->allocator, parameter);
}

void aws_endpoints_rule_clean_up(struct aws_endpoints_rule *rule) {
    AWS_PRECONDITION(rule);

    aws_array_list_deep_clean_up(&rule->conditions, s_on_condition_array_element_clean_up);

    switch (rule->type) {
        case AWS_ENDPOINTS_RULE_ENDPOINT:
            aws_endpoints_rule_data_endpoint_clean_up(&rule->rule_data.endpoint);
            break;
        case AWS_ENDPOINTS_RULE_ERROR:
            aws_endpoints_rule_data_error_clean_up(&rule->rule_data.error);
            break;
        case AWS_ENDPOINTS_RULE_TREE:
            aws_endpoints_rule_data_tree_clean_up(&rule->rule_data.tree);
            break;
        default:
            AWS_FATAL_ASSERT(false);
    }

    AWS_ZERO_STRUCT(*rule);
}

void aws_endpoints_rule_data_endpoint_clean_up(struct aws_endpoints_rule_data_endpoint *rule_data) {
    AWS_PRECONDITION(rule_data);

    aws_endpoints_expr_clean_up(&rule_data->url);

    aws_byte_buf_clean_up(&rule_data->properties);
    aws_hash_table_clean_up(&rule_data->headers);

    AWS_ZERO_STRUCT(*rule_data);
}

void aws_endpoints_rule_data_error_clean_up(struct aws_endpoints_rule_data_error *rule_data) {
    AWS_PRECONDITION(rule_data);

    aws_endpoints_expr_clean_up(&rule_data->error);

    AWS_ZERO_STRUCT(*rule_data);
}

void aws_endpoints_rule_data_tree_clean_up(struct aws_endpoints_rule_data_tree *rule_data) {
    AWS_PRECONDITION(rule_data);

    aws_array_list_deep_clean_up(&rule_data->rules, s_on_rule_array_element_clean_up);
    AWS_ZERO_STRUCT(*rule_data);
}

void aws_endpoints_condition_clean_up(struct aws_endpoints_condition *condition) {
    AWS_PRECONDITION(condition);

    aws_endpoints_expr_clean_up(&condition->expr);
    AWS_ZERO_STRUCT(*condition);
}

void aws_endpoints_function_clean_up(struct aws_endpoints_function *function) {
    AWS_PRECONDITION(function);

    aws_array_list_deep_clean_up(&function->argv, s_on_expr_array_element_clean_up);
    AWS_ZERO_STRUCT(*function);
}

void aws_endpoints_expr_clean_up(struct aws_endpoints_expr *expr) {
    AWS_PRECONDITION(expr);

    switch (expr->type) {
        case AWS_ENDPOINTS_EXPR_STRING:
        case AWS_ENDPOINTS_EXPR_BOOLEAN:
        case AWS_ENDPOINTS_EXPR_NUMBER:
        case AWS_ENDPOINTS_EXPR_REFERENCE:
            break;
        case AWS_ENDPOINTS_EXPR_FUNCTION:
            aws_endpoints_function_clean_up(&expr->e.function);
            break;
        case AWS_ENDPOINTS_EXPR_ARRAY:
            aws_array_list_deep_clean_up(&expr->e.array, s_on_expr_array_element_clean_up);
            break;
        default:
            AWS_FATAL_ASSERT(false);
    }

    AWS_ZERO_STRUCT(*expr);
}

struct aws_endpoints_scope_value *aws_endpoints_scope_value_new(
    struct aws_allocator *allocator,
    struct aws_byte_cursor name_cur) {
    AWS_PRECONDITION(allocator);
    struct aws_endpoints_scope_value *value = aws_mem_calloc(allocator, 1, sizeof(struct aws_endpoints_scope_value));

    value->allocator = allocator;
    value->name = aws_endpoints_owning_cursor_from_cursor(allocator, name_cur);

    return value;
}

void aws_endpoints_scope_value_destroy(struct aws_endpoints_scope_value *scope_value) {
    if (scope_value == NULL) {
        return;
    }
    aws_string_destroy(scope_value->name.string);

    aws_endpoints_value_clean_up(&scope_value->value);
    aws_mem_release(scope_value->allocator, scope_value);
}

void aws_endpoints_value_clean_up_cb(void *value);

void aws_endpoints_value_clean_up(struct aws_endpoints_value *aws_endpoints_value) {
    AWS_PRECONDITION(aws_endpoints_value);

    if (aws_endpoints_value->is_ref) {
        goto on_done;
    }

    if (aws_endpoints_value->type == AWS_ENDPOINTS_VALUE_STRING) {
        aws_string_destroy(aws_endpoints_value->v.owning_cursor_string.string);
    }

    if (aws_endpoints_value->type == AWS_ENDPOINTS_VALUE_OBJECT) {
        aws_string_destroy(aws_endpoints_value->v.owning_cursor_object.string);
    }

    if (aws_endpoints_value->type == AWS_ENDPOINTS_VALUE_ARRAY) {
        aws_array_list_deep_clean_up(&aws_endpoints_value->v.array, s_endpoints_value_clean_up_cb);
    }

on_done:
    AWS_ZERO_STRUCT(*aws_endpoints_value);
}

void s_endpoints_value_clean_up_cb(void *value) {
    struct aws_endpoints_value *aws_endpoints_value = value;
    aws_endpoints_value_clean_up(aws_endpoints_value);
}

int aws_endpoints_deep_copy_parameter_value(
    struct aws_allocator *allocator,
    const struct aws_endpoints_value *from,
    struct aws_endpoints_value *to) {

    to->type = from->type;
    to->is_ref = false;

    if (to->type == AWS_ENDPOINTS_VALUE_STRING) {
        to->v.owning_cursor_string =
            aws_endpoints_owning_cursor_from_cursor(allocator, from->v.owning_cursor_string.cur);
    } else if (to->type == AWS_ENDPOINTS_VALUE_BOOLEAN) {
        to->v.boolean = from->v.boolean;
    } else if (to->type == AWS_ENDPOINTS_VALUE_ARRAY) {
        size_t len = aws_array_list_length(&from->v.array);
        aws_array_list_init_dynamic(&to->v.array, allocator, len, sizeof(struct aws_endpoints_value));
        for (size_t i = 0; i < len; ++i) {
            struct aws_endpoints_value val;
            aws_array_list_get_at(&from->v.array, &val, i);

            struct aws_endpoints_value to_val;
            if (aws_endpoints_deep_copy_parameter_value(allocator, &val, &to_val)) {
                AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Unexpected array element type.");
                goto on_error;
            }

            aws_array_list_set_at(&to->v.array, &to_val, i);
        }
    } else {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_RESOLVE, "Unexpected value type.");
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }
    return AWS_OP_SUCCESS;

on_error:
    aws_endpoints_value_clean_up(to);
    return AWS_OP_ERR;
}
