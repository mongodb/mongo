/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/array_list.h>
#include <aws/common/hash_table.h>
#include <aws/common/json.h>
#include <aws/common/ref_count.h>
#include <aws/common/string.h>
#include <aws/sdkutils/private/endpoints_types_impl.h>
#include <aws/sdkutils/private/endpoints_util.h>

/* parameter types */
static struct aws_byte_cursor s_string_type_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("string");
static struct aws_byte_cursor s_boolean_type_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("boolean");
static struct aws_byte_cursor s_string_array_type_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("stringArray");

/* rule types */
static struct aws_byte_cursor s_endpoint_type_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("endpoint");
static struct aws_byte_cursor s_error_type_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("error");
static struct aws_byte_cursor s_tree_type_cur = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("tree");

static struct aws_byte_cursor s_supported_version = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("1.0");

static struct aws_byte_cursor s_empty_cursor = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("");

/* TODO: improve error messages. Include json line num? or dump json node? */

struct aws_byte_cursor aws_endpoints_get_supported_ruleset_version(void) {
    return s_supported_version;
}

/*
******************************
* Parameter Getters.
******************************
*/
enum aws_endpoints_parameter_type aws_endpoints_parameter_get_type(const struct aws_endpoints_parameter *parameter) {
    AWS_PRECONDITION(parameter);
    return parameter->type;
}

struct aws_byte_cursor aws_endpoints_parameter_get_built_in(const struct aws_endpoints_parameter *parameter) {
    AWS_PRECONDITION(parameter);
    return parameter->built_in;
}

int aws_endpoints_parameter_get_default_string(
    const struct aws_endpoints_parameter *parameter,
    struct aws_byte_cursor *out_cursor) {
    AWS_PRECONDITION(parameter);
    AWS_PRECONDITION(out_cursor);

    if (parameter->type == AWS_ENDPOINTS_PARAMETER_STRING) {
        *out_cursor = parameter->default_value.v.owning_cursor_string.cur;
        return AWS_OP_SUCCESS;
    };

    *out_cursor = s_empty_cursor;
    return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
}

int aws_endpoints_parameter_get_default_boolean(
    const struct aws_endpoints_parameter *parameter,
    const bool **out_bool) {
    AWS_PRECONDITION(parameter);
    AWS_PRECONDITION(out_bool);

    if (parameter->type == AWS_ENDPOINTS_PARAMETER_BOOLEAN) {
        *out_bool = &parameter->default_value.v.boolean;
        return AWS_OP_SUCCESS;
    };

    *out_bool = NULL;
    return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
}

bool aws_endpoints_parameters_get_is_required(const struct aws_endpoints_parameter *parameter) {
    AWS_PRECONDITION(parameter);
    return parameter->is_required;
}

struct aws_byte_cursor aws_endpoints_parameter_get_documentation(const struct aws_endpoints_parameter *parameter) {
    AWS_PRECONDITION(parameter);
    return parameter->documentation;
}

bool aws_endpoints_parameters_get_is_deprecated(const struct aws_endpoints_parameter *parameter) {
    AWS_PRECONDITION(parameter);
    return parameter->is_deprecated;
}

struct aws_byte_cursor aws_endpoints_parameter_get_deprecated_message(const struct aws_endpoints_parameter *parameter) {
    AWS_PRECONDITION(parameter);
    return parameter->deprecated_message;
}

struct aws_byte_cursor aws_endpoints_parameter_get_deprecated_since(const struct aws_endpoints_parameter *parameter) {
    AWS_PRECONDITION(parameter);
    return parameter->deprecated_since;
}

/*
******************************
* Parser getters.
******************************
*/

const struct aws_hash_table *aws_endpoints_ruleset_get_parameters(struct aws_endpoints_ruleset *ruleset) {
    AWS_PRECONDITION(ruleset);
    return &ruleset->parameters;
}

struct aws_byte_cursor aws_endpoints_ruleset_get_version(const struct aws_endpoints_ruleset *ruleset) {
    AWS_PRECONDITION(ruleset);
    return ruleset->version;
}

struct aws_byte_cursor aws_endpoints_ruleset_get_service_id(const struct aws_endpoints_ruleset *ruleset) {
    AWS_PRECONDITION(ruleset);
    return ruleset->service_id;
}

/*
******************************
* Parser helpers.
******************************
*/

static void s_on_rule_array_element_clean_up(void *element) {
    struct aws_endpoints_rule *rule = element;
    aws_endpoints_rule_clean_up(rule);
}

static void s_on_expr_element_clean_up(void *data) {
    struct aws_endpoints_expr *expr = data;
    aws_endpoints_expr_clean_up(expr);
}

static void s_callback_endpoints_parameter_destroy(void *data) {
    struct aws_endpoints_parameter *parameter = data;
    aws_endpoints_parameter_destroy(parameter);
}

static void s_callback_headers_destroy(void *data) {
    struct aws_array_list *array = data;
    struct aws_allocator *alloc = array->alloc;
    aws_array_list_deep_clean_up(array, s_on_expr_element_clean_up);
    aws_array_list_clean_up(array);
    aws_mem_release(alloc, array);
}

struct array_parser_wrapper {
    struct aws_allocator *allocator;
    struct aws_array_list *array;
};

static int s_init_array_from_json(
    struct aws_allocator *allocator,
    const struct aws_json_value *value_node,
    struct aws_array_list *values,
    aws_json_on_value_encountered_const_fn *value_fn) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(values);
    AWS_PRECONDITION(value_node);
    AWS_PRECONDITION(value_fn);

    struct array_parser_wrapper wrapper = {
        .allocator = allocator,
        .array = values,
    };

    if (aws_json_const_iterate_array(value_node, value_fn, &wrapper)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to iterate through array.");
        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
    }

    return AWS_OP_SUCCESS;
}

struct member_parser_wrapper {
    struct aws_allocator *allocator;
    struct aws_hash_table *table;
};

static int s_init_members_from_json(
    struct aws_allocator *allocator,
    struct aws_json_value *node,
    struct aws_hash_table *table,
    aws_json_on_member_encountered_const_fn *member_fn) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(node);
    AWS_PRECONDITION(table);

    struct member_parser_wrapper wrapper = {
        .allocator = allocator,
        .table = table,
    };

    if (aws_json_const_iterate_object(node, member_fn, &wrapper)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to iterate through member fields.");
        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
    }

    return AWS_OP_SUCCESS;
}

/*
******************************
* Parser functions.
******************************
*/

static int s_parse_function(
    struct aws_allocator *allocator,
    const struct aws_json_value *node,
    struct aws_endpoints_function *function);

/*
 * Note: this function only fails in cases where node is a ref (ie object with a
 * ref field), but cannot be parsed completely.
 */
static int s_try_parse_reference(const struct aws_json_value *node, struct aws_byte_cursor *out_reference) {
    AWS_PRECONDITION(node);

    AWS_ZERO_STRUCT(*out_reference);

    struct aws_json_value *ref_node = aws_json_value_get_from_object_c_str(node, "ref");
    if (ref_node != NULL && aws_json_value_get_string(ref_node, out_reference)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to parse ref.");
        AWS_ZERO_STRUCT(*out_reference);
        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
    }

    return AWS_OP_SUCCESS;
}

static int s_parse_expr(
    struct aws_allocator *allocator,
    const struct aws_json_value *node,
    struct aws_endpoints_expr *expr);

static int s_on_expr_element(
    size_t idx,
    const struct aws_json_value *value_node,
    bool *out_should_continue,
    void *user_data) {
    (void)idx;
    (void)out_should_continue;
    AWS_PRECONDITION(value_node);
    AWS_PRECONDITION(user_data);

    struct array_parser_wrapper *wrapper = user_data;

    struct aws_endpoints_expr expr;
    if (s_parse_expr(wrapper->allocator, value_node, &expr)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to parse expr.");
        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
    }

    aws_array_list_push_back(wrapper->array, &expr);

    return AWS_OP_SUCCESS;
}

static int s_parse_expr(
    struct aws_allocator *allocator,
    const struct aws_json_value *node,
    struct aws_endpoints_expr *expr) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(node);
    AWS_PRECONDITION(expr);

    AWS_ZERO_STRUCT(*expr);

    /* TODO: this recurses. in practical circumstances depth will never be high,
    but we should still consider doing iterative approach */
    if (aws_json_value_is_string(node) && !aws_json_value_get_string(node, &expr->e.string)) {
        expr->type = AWS_ENDPOINTS_EXPR_STRING;
        return AWS_OP_SUCCESS;
    } else if (aws_json_value_is_number(node) && !aws_json_value_get_number(node, &expr->e.number)) {
        expr->type = AWS_ENDPOINTS_EXPR_NUMBER;
        return AWS_OP_SUCCESS;
    } else if (aws_json_value_is_boolean(node) && !aws_json_value_get_boolean(node, &expr->e.boolean)) {
        expr->type = AWS_ENDPOINTS_EXPR_BOOLEAN;
        return AWS_OP_SUCCESS;
    } else if (aws_json_value_is_array(node)) {
        expr->type = AWS_ENDPOINTS_EXPR_ARRAY;
        size_t num_elements = aws_json_get_array_size(node);
        aws_array_list_init_dynamic(&expr->e.array, allocator, num_elements, sizeof(struct aws_endpoints_expr));
        if (s_init_array_from_json(allocator, node, &expr->e.array, s_on_expr_element)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to parse array value type.");
            goto on_error;
        }
        return AWS_OP_SUCCESS;
    }

    struct aws_byte_cursor reference;
    if (s_try_parse_reference(node, &reference)) {
        goto on_error;
    }

    if (reference.len > 0) {
        expr->type = AWS_ENDPOINTS_EXPR_REFERENCE;
        expr->e.reference = reference;
        return AWS_OP_SUCCESS;
    }

    expr->type = AWS_ENDPOINTS_EXPR_FUNCTION;
    if (s_parse_function(allocator, node, &expr->e.function)) {
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:
    aws_endpoints_expr_clean_up(expr);
    AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to parse expr type");
    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
}

static int s_parse_function(
    struct aws_allocator *allocator,
    const struct aws_json_value *node,
    struct aws_endpoints_function *function) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(node);

    AWS_ZERO_STRUCT(*function);

    struct aws_json_value *fn_node = aws_json_value_get_from_object_c_str(node, "fn");
    if (fn_node == NULL) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Node is not a function.");
        goto on_error;
    }

    struct aws_byte_cursor fn_cur;
    if (aws_json_value_get_string(fn_node, &fn_cur)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract fn name.");
        goto on_error;
    }

    function->fn = AWS_ENDPOINTS_FN_LAST;
    uint64_t hash = aws_hash_byte_cursor_ptr(&fn_cur);
    for (int idx = AWS_ENDPOINTS_FN_FIRST; idx < AWS_ENDPOINTS_FN_LAST; ++idx) {
        if (aws_endpoints_fn_name_hash[idx] == hash) {
            function->fn = idx;
            break;
        }
    }

    if (function->fn == AWS_ENDPOINTS_FN_LAST) {
        AWS_LOGF_ERROR(
            AWS_LS_SDKUTILS_ENDPOINTS_PARSING,
            "Could not map function name to function type: " PRInSTR,
            AWS_BYTE_CURSOR_PRI(fn_cur));
        goto on_error;
    }

    struct aws_json_value *argv_node = aws_json_value_get_from_object_c_str(node, "argv");
    if (argv_node == NULL || !aws_json_value_is_array(argv_node)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "No argv or unexpected type.");
        goto on_error;
    }

    size_t num_args = aws_json_get_array_size(argv_node);
    aws_array_list_init_dynamic(&function->argv, allocator, num_args, sizeof(struct aws_endpoints_expr));

    if (s_init_array_from_json(allocator, argv_node, &function->argv, s_on_expr_element)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to parse argv.");
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:
    aws_endpoints_function_clean_up(function);
    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
}

static int s_on_parameter_key(
    const struct aws_byte_cursor *key,
    const struct aws_json_value *value,
    bool *out_should_continue,
    void *user_data) {
    (void)out_should_continue;
    AWS_PRECONDITION(key);
    AWS_PRECONDITION(value);
    AWS_PRECONDITION(user_data);

    struct member_parser_wrapper *wrapper = user_data;

    struct aws_endpoints_parameter *parameter = aws_endpoints_parameter_new(wrapper->allocator, *key);

    /* required fields */
    struct aws_byte_cursor type_cur;
    struct aws_json_value *type_node = aws_json_value_get_from_object_c_str(value, "type");
    if (type_node == NULL || aws_json_value_get_string(type_node, &type_cur)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract parameter type.");
        goto on_error;
    }

    enum aws_endpoints_parameter_type type;
    if (aws_byte_cursor_eq_ignore_case(&type_cur, &s_string_type_cur)) {
        type = AWS_ENDPOINTS_PARAMETER_STRING;
    } else if (aws_byte_cursor_eq_ignore_case(&type_cur, &s_boolean_type_cur)) {
        type = AWS_ENDPOINTS_PARAMETER_BOOLEAN;
    } else if (aws_byte_cursor_eq_ignore_case(&type_cur, &s_string_array_type_cur)) {
        type = AWS_ENDPOINTS_PARAMETER_STRING_ARRAY;
    } else {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Unexpected type for parameter.");
        goto on_error;
    }

    parameter->type = type;

    struct aws_json_value *documentation_node = aws_json_value_get_from_object_c_str(value, "documentation");

    /* TODO: spec calls for documentation to be required, but several test-cases
        are missing docs on parameters */
    if (documentation_node != NULL) {
        if (aws_json_value_get_string(documentation_node, &parameter->documentation)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract parameter documentation.");
            goto on_error;
        }
    }

    /* optional fields */
    struct aws_json_value *built_in_node = aws_json_value_get_from_object_c_str(value, "builtIn");
    if (built_in_node != NULL) {
        if (aws_json_value_get_string(built_in_node, &parameter->built_in)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Unexpected type for built-in parameter field.");
            goto on_error;
        }
    }

    struct aws_json_value *required_node = aws_json_value_get_from_object_c_str(value, "required");
    if (required_node != NULL) {
        if (!aws_json_value_is_boolean(required_node)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Unexpected type for required parameter field.");
            goto on_error;
        }
        aws_json_value_get_boolean(required_node, &parameter->is_required);
    }

    struct aws_json_value *default_node = aws_json_value_get_from_object_c_str(value, "default");
    parameter->has_default_value = default_node != NULL;
    if (default_node != NULL) {
        if (type == AWS_ENDPOINTS_PARAMETER_STRING && aws_json_value_is_string(default_node)) {
            struct aws_byte_cursor cur;
            aws_json_value_get_string(default_node, &cur);
            parameter->default_value.type = AWS_ENDPOINTS_VALUE_STRING;
            parameter->default_value.v.owning_cursor_string = aws_endpoints_non_owning_cursor_create(cur);
        } else if (type == AWS_ENDPOINTS_PARAMETER_BOOLEAN && aws_json_value_is_boolean(default_node)) {
            parameter->default_value.type = AWS_ENDPOINTS_VALUE_BOOLEAN;
            aws_json_value_get_boolean(default_node, &parameter->default_value.v.boolean);
        } else if (type == AWS_ENDPOINTS_PARAMETER_STRING_ARRAY && aws_json_value_is_array(default_node)) {
            parameter->default_value.type = AWS_ENDPOINTS_VALUE_ARRAY;
            size_t len = aws_json_get_array_size(default_node);
            aws_array_list_init_dynamic(
                &parameter->default_value.v.array, wrapper->allocator, len, sizeof(struct aws_endpoints_value));
            for (size_t i = 0; i < len; ++i) {
                struct aws_json_value *element = aws_json_get_array_element(default_node, i);
                if (!aws_json_value_is_string(element)) {
                    AWS_LOGF_ERROR(
                        AWS_LS_SDKUTILS_ENDPOINTS_PARSING,
                        "Unexpected type for default parameter value. String array parameter must have string "
                        "elements");
                    goto on_error;
                }

                struct aws_byte_cursor cur;
                aws_json_value_get_string(element, &cur);

                struct aws_endpoints_value val = {
                    .is_ref = false,
                    .type = AWS_ENDPOINTS_VALUE_STRING,
                    .v.owning_cursor_string = aws_endpoints_non_owning_cursor_create(cur)};

                aws_array_list_set_at(&parameter->default_value.v.array, &val, i);
            }
        } else {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Unexpected type for default parameter value.");
            goto on_error;
        }
    }

    struct aws_json_value *deprecated_node = aws_json_value_get_from_object_c_str(value, "deprecated");
    if (deprecated_node != NULL) {
        struct aws_json_value *deprecated_message_node =
            aws_json_value_get_from_object_c_str(deprecated_node, "message");
        if (deprecated_message_node != NULL &&
            aws_json_value_get_string(deprecated_message_node, &parameter->deprecated_message)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Unexpected value for deprecated message.");
            goto on_error;
        }

        struct aws_json_value *deprecated_since_node = aws_json_value_get_from_object_c_str(deprecated_node, "since");
        if (deprecated_since_node != NULL &&
            aws_json_value_get_string(deprecated_since_node, &parameter->deprecated_since)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Unexpected value for deprecated since.");
            goto on_error;
        }
    }

    if (aws_hash_table_put(wrapper->table, &parameter->name, parameter, NULL)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to add parameter.");
        goto on_error;
    }
    return AWS_OP_SUCCESS;

on_error:
    aws_endpoints_parameter_destroy(parameter);
    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
}

static int s_on_condition_element(
    size_t idx,
    const struct aws_json_value *condition_node,
    bool *out_should_continue,
    void *user_data) {
    (void)idx;
    (void)out_should_continue;
    AWS_PRECONDITION(condition_node);
    AWS_PRECONDITION(user_data);

    struct array_parser_wrapper *wrapper = user_data;

    struct aws_endpoints_condition condition;
    AWS_ZERO_STRUCT(condition);

    condition.expr.type = AWS_ENDPOINTS_EXPR_FUNCTION;
    if (s_parse_function(wrapper->allocator, condition_node, &condition.expr.e.function)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to parse function.");
        goto on_error;
    }

    struct aws_json_value *assign_node = aws_json_value_get_from_object_c_str(condition_node, "assign");
    if (assign_node != NULL && aws_json_value_get_string(assign_node, &condition.assign)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Unexpected value for assign.");
        goto on_error;
    }

    aws_array_list_push_back(wrapper->array, &condition);
    return AWS_OP_SUCCESS;

on_error:
    aws_endpoints_condition_clean_up(&condition);
    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
}

static int s_on_header_element(
    size_t idx,
    const struct aws_json_value *value,
    bool *out_should_continue,
    void *user_data) {
    (void)idx;
    (void)out_should_continue;
    AWS_PRECONDITION(value);
    AWS_PRECONDITION(user_data);
    struct array_parser_wrapper *wrapper = user_data;

    struct aws_endpoints_expr expr;
    if (s_parse_expr(wrapper->allocator, value, &expr)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Unexpected format for header element.");
        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
    }

    aws_array_list_push_back(wrapper->array, &expr);
    return AWS_OP_SUCCESS;
}

static int s_on_headers_key(
    const struct aws_byte_cursor *key,
    const struct aws_json_value *value,
    bool *out_should_continue,
    void *user_data) {
    (void)out_should_continue;
    AWS_PRECONDITION(key);
    AWS_PRECONDITION(value);
    AWS_PRECONDITION(user_data);
    struct member_parser_wrapper *wrapper = user_data;

    if (!aws_json_value_is_array(value)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Unexpected format for header value.");
        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
    }

    size_t num_elements = aws_json_get_array_size(value);
    struct aws_array_list *headers = aws_mem_calloc(wrapper->allocator, 1, sizeof(struct aws_array_list));
    aws_array_list_init_dynamic(headers, wrapper->allocator, num_elements, sizeof(struct aws_endpoints_expr));
    if (s_init_array_from_json(wrapper->allocator, value, headers, s_on_header_element)) {
        goto on_error;
    }

    aws_hash_table_put(wrapper->table, aws_string_new_from_cursor(wrapper->allocator, key), headers, NULL);

    return AWS_OP_SUCCESS;

on_error:
    if (headers) {
        s_callback_headers_destroy(headers);
    }
    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
}

static int s_parse_endpoints_rule_data_endpoint(
    struct aws_allocator *allocator,
    const struct aws_json_value *rule_node,
    struct aws_endpoints_rule_data_endpoint *data_rule) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(rule_node);
    AWS_PRECONDITION(data_rule);

    data_rule->allocator = allocator;
    struct aws_json_value *url_node = aws_json_value_get_from_object_c_str(rule_node, "url");
    if (url_node == NULL || aws_json_value_is_string(url_node)) {
        data_rule->url.type = AWS_ENDPOINTS_EXPR_STRING;
        aws_json_value_get_string(url_node, &data_rule->url.e.string);
    } else {
        struct aws_byte_cursor reference;
        if (s_try_parse_reference(url_node, &reference)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to parse reference.");
            goto on_error;
        }

        if (reference.len > 0) {
            data_rule->url.type = AWS_ENDPOINTS_EXPR_REFERENCE;
            data_rule->url.e.reference = reference;
        } else {
            data_rule->url.type = AWS_ENDPOINTS_EXPR_FUNCTION;
            if (s_parse_function(allocator, url_node, &data_rule->url.e.function)) {
                AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to function.");
                goto on_error;
            }
        }
    }

    struct aws_json_value *properties_node = aws_json_value_get_from_object_c_str(rule_node, "properties");
    if (properties_node != NULL) {
        aws_byte_buf_init(&data_rule->properties, allocator, 0);

        if (aws_byte_buf_append_json_string(properties_node, &data_rule->properties)) {
            aws_byte_buf_clean_up(&data_rule->properties);
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract properties.");
            goto on_error;
        }
    }

    /* TODO: this is currently aws_string* to aws_array_list*
     * We cannot use same trick as for params to use aws_byte_cursor as key,
     * since value is a generic type. We can wrap list into a struct, but
     * seems ugly. Anything cleaner?
     */
    aws_hash_table_init(
        &data_rule->headers,
        allocator,
        20,
        aws_hash_string,
        aws_hash_callback_string_eq,
        aws_hash_callback_string_destroy,
        s_callback_headers_destroy);

    struct aws_json_value *headers_node = aws_json_value_get_from_object_c_str(rule_node, "headers");
    if (headers_node != NULL) {

        if (s_init_members_from_json(allocator, headers_node, &data_rule->headers, s_on_headers_key)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract headers.");
            goto on_error;
        }
    }

    return AWS_OP_SUCCESS;

on_error:
    aws_endpoints_rule_data_endpoint_clean_up(data_rule);
    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
}

static int s_parse_endpoints_rule_data_error(
    struct aws_allocator *allocator,
    const struct aws_json_value *error_node,
    struct aws_endpoints_rule_data_error *data_rule) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(error_node);
    AWS_PRECONDITION(data_rule);

    if (aws_json_value_is_string(error_node)) {
        data_rule->error.type = AWS_ENDPOINTS_EXPR_STRING;
        aws_json_value_get_string(error_node, &data_rule->error.e.string);

        return AWS_OP_SUCCESS;
    }

    struct aws_byte_cursor reference;
    if (s_try_parse_reference(error_node, &reference)) {
        goto on_error;
    }

    if (reference.len > 0) {
        data_rule->error.type = AWS_ENDPOINTS_EXPR_REFERENCE;
        data_rule->error.e.reference = reference;
        return AWS_OP_SUCCESS;
    }

    data_rule->error.type = AWS_ENDPOINTS_EXPR_FUNCTION;
    if (s_parse_function(allocator, error_node, &data_rule->error.e.function)) {
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:
    aws_endpoints_rule_data_error_clean_up(data_rule);
    AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to parse error rule.");
    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
}

static int s_on_rule_element(
    size_t idx,
    const struct aws_json_value *value,
    bool *out_should_continue,
    void *user_data);

static int s_parse_endpoints_rule_data_tree(
    struct aws_allocator *allocator,
    const struct aws_json_value *rule_node,
    struct aws_endpoints_rule_data_tree *rule_data) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(rule_node);
    AWS_PRECONDITION(rule_data);

    struct aws_json_value *rules_node = aws_json_value_get_from_object_c_str(rule_node, "rules");
    if (rules_node == NULL || !aws_json_value_is_array(rules_node)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Rules node is missing or unexpected type.");
        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
    }

    size_t num_rules = aws_json_get_array_size(rules_node);
    aws_array_list_init_dynamic(&rule_data->rules, allocator, num_rules, sizeof(struct aws_endpoints_rule));
    if (s_init_array_from_json(allocator, rules_node, &rule_data->rules, s_on_rule_element)) {
        aws_endpoints_rule_data_tree_clean_up(rule_data);
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to parse rules.");
        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
    }

    return AWS_OP_SUCCESS;
}

static int s_on_rule_element(
    size_t idx,
    const struct aws_json_value *value,
    bool *out_should_continue,
    void *user_data) {
    (void)idx;
    (void)out_should_continue;
    AWS_PRECONDITION(value);
    AWS_PRECONDITION(user_data);

    struct array_parser_wrapper *wrapper = user_data;

    /* Required fields */
    struct aws_byte_cursor type_cur;
    struct aws_json_value *type_node = aws_json_value_get_from_object_c_str(value, "type");
    if (type_node == NULL || aws_json_value_get_string(type_node, &type_cur)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract rule type.");
        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
    }

    enum aws_endpoints_rule_type type;
    if (aws_byte_cursor_eq_ignore_case(&type_cur, &s_endpoint_type_cur)) {
        type = AWS_ENDPOINTS_RULE_ENDPOINT;
    } else if (aws_byte_cursor_eq_ignore_case(&type_cur, &s_error_type_cur)) {
        type = AWS_ENDPOINTS_RULE_ERROR;
    } else if (aws_byte_cursor_eq_ignore_case(&type_cur, &s_tree_type_cur)) {
        type = AWS_ENDPOINTS_RULE_TREE;
    } else {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Unexpected rule type.");
        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
    }

    struct aws_endpoints_rule rule;
    AWS_ZERO_STRUCT(rule);
    rule.type = type;

    struct aws_json_value *conditions_node = aws_json_value_get_from_object_c_str(value, "conditions");
    if (conditions_node == NULL || !aws_json_value_is_array(conditions_node)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Conditions node missing.");
        goto on_error;
    }

    size_t num_conditions = aws_json_get_array_size(conditions_node);
    aws_array_list_init_dynamic(
        &rule.conditions, wrapper->allocator, num_conditions, sizeof(struct aws_endpoints_condition));

    if (s_init_array_from_json(wrapper->allocator, conditions_node, &rule.conditions, s_on_condition_element)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract conditions.");
        goto on_error;
    }

    switch (type) {
        case AWS_ENDPOINTS_RULE_ENDPOINT: {
            struct aws_json_value *endpoint_node = aws_json_value_get_from_object_c_str(value, "endpoint");
            if (endpoint_node == NULL ||
                s_parse_endpoints_rule_data_endpoint(wrapper->allocator, endpoint_node, &rule.rule_data.endpoint)) {
                AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract endpoint rule data.");
                goto on_error;
            }
            break;
        }
        case AWS_ENDPOINTS_RULE_ERROR: {
            struct aws_json_value *error_node = aws_json_value_get_from_object_c_str(value, "error");
            if (error_node == NULL ||
                s_parse_endpoints_rule_data_error(wrapper->allocator, error_node, &rule.rule_data.error)) {
                AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract error rule data.");
                goto on_error;
            }
            break;
        }
        case AWS_ENDPOINTS_RULE_TREE: {
            if (s_parse_endpoints_rule_data_tree(wrapper->allocator, value, &rule.rule_data.tree)) {
                AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract tree rule data.");
                goto on_error;
            }
            break;
        }
        default:
            AWS_FATAL_ASSERT(false);
    }

    /* Optional fields */
    struct aws_json_value *documentation_node = aws_json_value_get_from_object_c_str(value, "documentation");
    if (documentation_node != NULL) {
        if (aws_json_value_get_string(documentation_node, &rule.documentation)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract parameter documentation.");
            goto on_error;
        }
    }

    aws_array_list_push_back(wrapper->array, &rule);

    return AWS_OP_SUCCESS;

on_error:
    aws_endpoints_rule_clean_up(&rule);
    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
}

static int s_init_ruleset_from_json(
    struct aws_allocator *allocator,
    struct aws_endpoints_ruleset *ruleset,
    struct aws_byte_cursor json) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(ruleset);
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&json));

    struct aws_json_value *root = aws_json_value_new_from_string(allocator, json);

    if (root == NULL) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to parse provided string as json.");
        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
    }

    ruleset->json_root = root;

    struct aws_json_value *version_node = aws_json_value_get_from_object_c_str(root, "version");
    if (version_node == NULL || aws_json_value_get_string(version_node, &ruleset->version)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract version.");
        aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_UNSUPPORTED_RULESET);
        goto on_error;
    }

#ifdef ENDPOINTS_VERSION_CHECK /* TODO: samples are currently inconsistent with versions. skip check for now */
    if (!aws_byte_cursor_eq_c_str(&ruleset->version, &s_supported_version)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Unsupported ruleset version.");
        aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_UNSUPPORTED_RULESET);
        goto on_error;
    }
#endif

    struct aws_json_value *service_id_node = aws_json_value_get_from_object_c_str(root, "serviceId");

    if (service_id_node != NULL && aws_json_value_get_string(service_id_node, &ruleset->service_id)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract serviceId.");
        aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_UNSUPPORTED_RULESET);
        goto on_error;
    }

    aws_hash_table_init(
        &ruleset->parameters,
        allocator,
        20,
        aws_hash_byte_cursor_ptr,
        aws_endpoints_byte_cursor_eq,
        NULL,
        s_callback_endpoints_parameter_destroy);

    struct aws_json_value *parameters_node = aws_json_value_get_from_object_c_str(root, "parameters");
    if (parameters_node == NULL ||
        s_init_members_from_json(allocator, parameters_node, &ruleset->parameters, s_on_parameter_key)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract parameters.");
        aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
        goto on_error;
    }

    struct aws_json_value *rules_node = aws_json_value_get_from_object_c_str(root, "rules");
    if (rules_node == NULL || !aws_json_value_is_array(rules_node)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Unexpected type for rules node.");
        aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
        goto on_error;
    }
    size_t num_rules = aws_json_get_array_size(rules_node);
    aws_array_list_init_dynamic(&ruleset->rules, allocator, num_rules, sizeof(struct aws_endpoints_rule));
    if (s_init_array_from_json(allocator, rules_node, &ruleset->rules, s_on_rule_element)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_PARSING, "Failed to extract rules.");
        aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_PARSE_FAILED);
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:
    return AWS_OP_ERR;
}

static void s_endpoints_ruleset_destroy(void *data) {
    if (data == NULL) {
        return;
    }

    struct aws_endpoints_ruleset *ruleset = data;

    aws_hash_table_clean_up(&ruleset->parameters);

    aws_array_list_deep_clean_up(&ruleset->rules, s_on_rule_array_element_clean_up);

    aws_json_value_destroy(ruleset->json_root);

    aws_mem_release(ruleset->allocator, ruleset);
}

struct aws_endpoints_ruleset *aws_endpoints_ruleset_new_from_string(
    struct aws_allocator *allocator,
    struct aws_byte_cursor ruleset_json) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&ruleset_json));

    struct aws_endpoints_ruleset *ruleset = aws_mem_calloc(allocator, 1, sizeof(struct aws_endpoints_ruleset));
    ruleset->allocator = allocator;

    if (s_init_ruleset_from_json(allocator, ruleset, ruleset_json)) {
        s_endpoints_ruleset_destroy(ruleset);
        return NULL;
    }

    aws_ref_count_init(&ruleset->ref_count, ruleset, s_endpoints_ruleset_destroy);

    return ruleset;
}

struct aws_endpoints_ruleset *aws_endpoints_ruleset_acquire(struct aws_endpoints_ruleset *ruleset) {
    AWS_PRECONDITION(ruleset);
    if (ruleset) {
        aws_ref_count_acquire(&ruleset->ref_count);
    }
    return ruleset;
}

struct aws_endpoints_ruleset *aws_endpoints_ruleset_release(struct aws_endpoints_ruleset *ruleset) {
    if (ruleset) {
        aws_ref_count_release(&ruleset->ref_count);
    }
    return NULL;
}
