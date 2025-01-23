/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_SDKUTILS_ENDPOINTS_RULESET_TYPES_IMPL_H
#define AWS_SDKUTILS_ENDPOINTS_RULESET_TYPES_IMPL_H

#include <aws/common/hash_table.h>
#include <aws/common/ref_count.h>
#include <aws/sdkutils/endpoints_rule_engine.h>
#include <aws/sdkutils/private/endpoints_util.h>

struct aws_json_value;

/*
 * Rule engine is built around 2 major types:
 * - expr - can be a literal, like bool or number or expression like function or ref
 * - value - literal types only. result of resolving expr. Can have special None
 *   value depending on how expr is resolved. Ex. accessing array past bounds or
 *   substrings with invalid start/end combination will both result in null.
 *
 * There is a lot of overlap between expr and value, so why do we need both?
 * Primary reason is to create a clean boundary between ruleset and resolved
 * values as it allows to distinguish easily between things that need to be
 * resolved and things that have been lowered. Given this type system, rule
 * engine basically performs a task of transforming exprs into values to get
 * final result.
 *
 * Other important types:
 * Parameter - definition of values that can be provided to rule engine during
 * resolution. Can define default values if caller didn't provide a value for
 * parameter.
 * Request Context - set of parameter value defined for a particular request that
 * are used during resolution
 * Scope - set of values defined during resolution of a rule. Can grow/shrink as
 * rules are evaluated. Ex. scope can have value with name "Region" and value "us-west-2".
 */

/*
******************************
* Parse types.
******************************
*/

enum aws_endpoints_rule_type { AWS_ENDPOINTS_RULE_ENDPOINT, AWS_ENDPOINTS_RULE_ERROR, AWS_ENDPOINTS_RULE_TREE };

enum aws_endpoints_expr_type {
    AWS_ENDPOINTS_EXPR_STRING,
    AWS_ENDPOINTS_EXPR_NUMBER,
    AWS_ENDPOINTS_EXPR_BOOLEAN,
    AWS_ENDPOINTS_EXPR_ARRAY,
    AWS_ENDPOINTS_EXPR_REFERENCE,
    AWS_ENDPOINTS_EXPR_FUNCTION
};

enum aws_endpoints_fn_type {
    AWS_ENDPOINTS_FN_FIRST = 0,
    AWS_ENDPOINTS_FN_IS_SET = 0,
    AWS_ENDPOINTS_FN_NOT,
    AWS_ENDPOINTS_FN_GET_ATTR,
    AWS_ENDPOINTS_FN_SUBSTRING,
    AWS_ENDPOINTS_FN_STRING_EQUALS,
    AWS_ENDPOINTS_FN_BOOLEAN_EQUALS,
    AWS_ENDPOINTS_FN_URI_ENCODE,
    AWS_ENDPOINTS_FN_PARSE_URL,
    AWS_ENDPOINTS_FN_IS_VALID_HOST_LABEL,
    AWS_ENDPOINTS_FN_AWS_PARTITION,
    AWS_ENDPOINTS_FN_AWS_PARSE_ARN,
    AWS_ENDPOINTS_FN_AWS_IS_VIRTUAL_HOSTABLE_S3_BUCKET,
    AWS_ENDPOINTS_FN_LAST,
};

enum aws_endpoints_value_type {
    /* Special value to represent that any value type is expected from resolving an expresion.
        Not a valid value for a value type. */
    AWS_ENDPOINTS_VALUE_ANY,

    AWS_ENDPOINTS_VALUE_NONE,
    AWS_ENDPOINTS_VALUE_STRING,
    AWS_ENDPOINTS_VALUE_BOOLEAN,
    AWS_ENDPOINTS_VALUE_OBJECT, /* Generic type returned by some functions. json string under the covers. */
    AWS_ENDPOINTS_VALUE_NUMBER,
    AWS_ENDPOINTS_VALUE_ARRAY,

    AWS_ENDPOINTS_VALUE_SIZE
};

/* concrete type value */
struct aws_endpoints_value {
    enum aws_endpoints_value_type type;
    union {
        struct aws_owning_cursor owning_cursor_string;
        bool boolean;
        struct aws_owning_cursor owning_cursor_object;
        double number;
        struct aws_array_list array;
    } v;
    /* Value is a reference to another value, no need to clean it up. */
    bool is_ref;
};

struct aws_endpoints_parameter {
    struct aws_allocator *allocator;

    struct aws_byte_cursor name;

    enum aws_endpoints_parameter_type type;
    struct aws_byte_cursor built_in;

    bool has_default_value;
    struct aws_endpoints_value default_value;

    bool is_required;
    struct aws_byte_cursor documentation;
    bool is_deprecated;
    struct aws_byte_cursor deprecated_message;
    struct aws_byte_cursor deprecated_since;
};

struct aws_endpoints_ruleset {
    struct aws_allocator *allocator;
    struct aws_ref_count ref_count;

    struct aws_json_value *json_root;

    /* list of (aws_endpoints_rule) */
    struct aws_array_list rules;

    struct aws_byte_cursor version;
    struct aws_byte_cursor service_id;
    /* map of (aws_byte_cursor *) -> (aws_endpoints_parameter *) */
    struct aws_hash_table parameters;
};

struct aws_endpoints_function {
    enum aws_endpoints_fn_type fn;
    /* List of (aws_endpoints_expr) */
    struct aws_array_list argv;
};

struct aws_endpoints_expr {
    enum aws_endpoints_expr_type type;
    union {
        struct aws_byte_cursor string;
        double number;
        bool boolean;
        struct aws_array_list array; /* List of (aws_endpoints_expr) */
        struct aws_byte_cursor reference;
        struct aws_endpoints_function function;
    } e;
};

struct aws_endpoints_rule_data_endpoint {
    struct aws_allocator *allocator;
    struct aws_endpoints_expr url;

    /*
     * Note: this is a custom properties json associated with the result.
     * Properties are unstable and format can change frequently.
     * Its up to caller to parse json to retrieve properties.
     */
    struct aws_byte_buf properties;
    /* Map of (aws_string *) -> (aws_array_list * of aws_endpoints_expr) */
    struct aws_hash_table headers;
};

struct aws_endpoints_rule_data_error {
    struct aws_endpoints_expr error;
};

struct aws_endpoints_rule_data_tree {
    /* List of (aws_endpoints_rule) */
    struct aws_array_list rules;
};

struct aws_endpoints_condition {
    struct aws_endpoints_expr expr;
    struct aws_byte_cursor assign;
};

struct aws_endpoints_rule {
    /* List of (aws_endpoints_condition) */
    struct aws_array_list conditions;
    struct aws_byte_cursor documentation;

    enum aws_endpoints_rule_type type;
    union {
        struct aws_endpoints_rule_data_endpoint endpoint;
        struct aws_endpoints_rule_data_error error;
        struct aws_endpoints_rule_data_tree tree;
    } rule_data;
};

struct aws_partition_info {
    struct aws_allocator *allocator;
    struct aws_byte_cursor name;

    bool is_copy;
    struct aws_string *info;

    struct aws_endpoints_regex *region_regex;
};

/*
 * Basic partitions file structure is a list of partitions at top level that has
 * some metadata associated with it and then each partition has a list of
 * regions within it, with each region possibly overriding some of that info.
 * The 2 use cases we need to support is matching region to partition and then
 * iterating over all partitions and matching regex in partition meta to region name.
 * To support both cases we have 2 structures:
 * - base_partitions - list of all partitions. this is a primary owner for partition
 *   meta data
 * - region_to_partition_info - mapping from region name to partition. creates
 *   new meta info if region overrides any meta values, otherwise points to
 *   partitions copy of meta info (is_copy flag is true)
 */
struct aws_partitions_config {
    struct aws_allocator *allocator;
    struct aws_ref_count ref_count;

    struct aws_json_value *json_root;

    /* map of (byte_cur -> aws_partition_info) */
    struct aws_hash_table region_to_partition_info;

    /* map of (byte_cur -> aws_partition_info) */
    struct aws_hash_table base_partitions;

    struct aws_string *version;
};

/*
******************************
* Eval types.
******************************
*/

struct aws_endpoints_request_context {
    struct aws_allocator *allocator;
    struct aws_ref_count ref_count;

    struct aws_hash_table values;
};

/* wrapper around aws_endpoints_value to store it more easily in hash table*/
struct aws_endpoints_scope_value {
    struct aws_allocator *allocator;

    struct aws_owning_cursor name;

    struct aws_endpoints_value value;
};

struct aws_endpoints_resolution_scope {
    /* current values in scope. byte_cur -> aws_endpoints_scope_value */
    struct aws_hash_table values;
    /* list of value keys added since last cleanup */
    struct aws_array_list added_keys;

    /* index of the rule currently being evaluated */
    size_t rule_idx;
    /* pointer to rules array */
    const struct aws_array_list *rules;

    const struct aws_partitions_config *partitions;
};

struct aws_partition_info *aws_partition_info_new(struct aws_allocator *allocator, struct aws_byte_cursor name);
void aws_partition_info_destroy(struct aws_partition_info *partition_info);

struct aws_endpoints_parameter *aws_endpoints_parameter_new(
    struct aws_allocator *allocator,
    struct aws_byte_cursor name);
void aws_endpoints_parameter_destroy(struct aws_endpoints_parameter *parameter);

void aws_endpoints_rule_clean_up(struct aws_endpoints_rule *rule);

void aws_endpoints_rule_data_endpoint_clean_up(struct aws_endpoints_rule_data_endpoint *rule_data);
void aws_endpoints_rule_data_error_clean_up(struct aws_endpoints_rule_data_error *rule_data);
void aws_endpoints_rule_data_tree_clean_up(struct aws_endpoints_rule_data_tree *rule_data);

void aws_endpoints_condition_clean_up(struct aws_endpoints_condition *condition);
void aws_endpoints_function_clean_up(struct aws_endpoints_function *function);
void aws_endpoints_expr_clean_up(struct aws_endpoints_expr *expr);

struct aws_endpoints_scope_value *aws_endpoints_scope_value_new(
    struct aws_allocator *allocator,
    struct aws_byte_cursor name_cur);
void aws_endpoints_scope_value_destroy(struct aws_endpoints_scope_value *scope_value);

int aws_endpoints_deep_copy_parameter_value(
    struct aws_allocator *allocator,
    const struct aws_endpoints_value *from,
    struct aws_endpoints_value *to);

void aws_endpoints_value_clean_up(struct aws_endpoints_value *aws_endpoints_value);

/* Helper to resolve argv. Implemented in rule engine. */
int aws_endpoints_argv_expect(
    struct aws_allocator *allocator,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_array_list *argv,
    size_t idx,
    enum aws_endpoints_value_type expected_type,
    struct aws_endpoints_value *out_value);

extern uint64_t aws_endpoints_fn_name_hash[AWS_ENDPOINTS_FN_LAST];
void aws_endpoints_rule_engine_init(void);

int aws_endpoints_dispatch_standard_lib_fn_resolve(
    enum aws_endpoints_fn_type type,
    struct aws_allocator *allocator,
    struct aws_array_list *argv,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *out_value);

int aws_endpoints_path_through_array(
    struct aws_allocator *allocator,
    struct aws_endpoints_resolution_scope *scope,
    struct aws_endpoints_value *eval_val,
    struct aws_byte_cursor path_cur,
    struct aws_endpoints_value *out_value);

int aws_endpoints_path_through_object(
    struct aws_allocator *allocator,
    struct aws_endpoints_value *eval_val,
    struct aws_byte_cursor path_cur,
    struct aws_endpoints_value *out_value);

#endif /* AWS_SDKUTILS_ENDPOINTS_RULESET_TYPES_IMPL_H */
