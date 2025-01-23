/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_SDKUTILS_ENDPOINTS_RULESET_H
#define AWS_SDKUTILS_ENDPOINTS_RULESET_H

#include <aws/common/byte_buf.h>
#include <aws/sdkutils/sdkutils.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_endpoints_ruleset;
struct aws_partitions_config;
struct aws_endpoints_parameter;
struct aws_endpoints_rule_engine;
struct aws_endpoints_resolved_endpoint;
struct aws_endpoints_request_context;
struct aws_hash_table;

enum aws_endpoints_parameter_type {
    AWS_ENDPOINTS_PARAMETER_STRING,
    AWS_ENDPOINTS_PARAMETER_BOOLEAN,
    AWS_ENDPOINTS_PARAMETER_STRING_ARRAY,
};
enum aws_endpoints_resolved_endpoint_type { AWS_ENDPOINTS_RESOLVED_ENDPOINT, AWS_ENDPOINTS_RESOLVED_ERROR };

AWS_EXTERN_C_BEGIN

AWS_SDKUTILS_API struct aws_byte_cursor aws_endpoints_get_supported_ruleset_version(void);

/*
******************************
* Parameter
******************************
*/

/*
 * Value type of parameter.
 */
AWS_SDKUTILS_API enum aws_endpoints_parameter_type aws_endpoints_parameter_get_type(
    const struct aws_endpoints_parameter *parameter);

/*
 * Specifies whether parameter maps to one of SDK built ins (ex. "AWS::Region").
 * Return is a cursor specifying the name of associated built in.
 * If there is no mapping, cursor will be empty.
 * Cursor is guaranteed to be valid for lifetime of paramater.
 */
AWS_SDKUTILS_API struct aws_byte_cursor aws_endpoints_parameter_get_built_in(
    const struct aws_endpoints_parameter *parameter);

/*
 * Default string value.
 * out_cursor will point to default string value if one exist and will be empty
 * otherwise.
 * Cursor is guaranteed to be valid for lifetime of paramater.
 * Returns AWS_OP_ERR if parameter is not a string.
 */
AWS_SDKUTILS_API int aws_endpoints_parameter_get_default_string(
    const struct aws_endpoints_parameter *parameter,
    struct aws_byte_cursor *out_cursor);

/*
 * Default boolean value.
 * out_bool will have pointer to value if default is specified, NULL otherwise.
 * Owned by parameter.
 * Returns AWS_OP_ERR if parameter is not a boolean.
 */
AWS_SDKUTILS_API int aws_endpoints_parameter_get_default_boolean(
    const struct aws_endpoints_parameter *parameter,
    const bool **out_bool);

/*
 * Whether parameter is required.
 */
AWS_SDKUTILS_API bool aws_endpoints_parameter_get_is_required(const struct aws_endpoints_parameter *parameter);

/*
 * Returns cursor to parameter documentation.
 * Cursor is guaranteed to be valid for lifetime of paramater.
 * Will not be empty as doc is required.
 */
AWS_SDKUTILS_API struct aws_byte_cursor aws_endpoints_parameter_get_documentation(
    const struct aws_endpoints_parameter *parameter);

/*
 * Whether parameter is deprecated.
 */
AWS_SDKUTILS_API bool aws_endpoints_parameters_get_is_deprecated(const struct aws_endpoints_parameter *parameter);

/*
 * Deprecation message. Cursor is empty if parameter is not deprecated.
 * Cursor is guaranteed to be valid for lifetime of paramater.
 */
AWS_SDKUTILS_API struct aws_byte_cursor aws_endpoints_parameter_get_deprecated_message(
    const struct aws_endpoints_parameter *parameter);

/*
 * Deprecated since. Cursor is empty if parameter is not deprecated.
 * Cursor is guaranteed to be valid for lifetime of paramater.
 */
AWS_SDKUTILS_API struct aws_byte_cursor aws_endpoints_parameter_get_deprecated_since(
    const struct aws_endpoints_parameter *parameter);

/*
******************************
* Ruleset
******************************
*/

/*
 * Create new ruleset from a json string.
 * In cases of failure NULL is returned and last error is set.
 */
AWS_SDKUTILS_API struct aws_endpoints_ruleset *aws_endpoints_ruleset_new_from_string(
    struct aws_allocator *allocator,
    struct aws_byte_cursor ruleset_json);

/*
 * Increment ref count
 */
AWS_SDKUTILS_API struct aws_endpoints_ruleset *aws_endpoints_ruleset_acquire(struct aws_endpoints_ruleset *ruleset);

/*
 * Decrement ref count
 */
AWS_SDKUTILS_API struct aws_endpoints_ruleset *aws_endpoints_ruleset_release(struct aws_endpoints_ruleset *ruleset);

/*
 * Get ruleset parameters.
 * Return is a hashtable with paramater name as a key (aws_byte_cursor *) and parameter
 * (aws_endpoints_parameter *) as a value. Ruleset owns the owns the hashtable and
 * pointer is valid during ruleset lifetime. Will never return a NULL. In case
 * there are no parameters in the ruleset, hash table will contain 0 elements.
 *
 * Note on usage in bindings:
 * - this is basically a map from a parameter name to a structure describing parameter
 * - deep copy all the fields and let language take ownership of data
 *   Consider transforming this into language specific map (dict for python, Map
 *   in Java, std::map in C++, etc...) instead of wrapping it into a custom class.
 */
AWS_SDKUTILS_API const struct aws_hash_table *aws_endpoints_ruleset_get_parameters(
    struct aws_endpoints_ruleset *ruleset);

/*
 * Ruleset version.
 * Returned pointer is owned by ruleset.
 * Will not return NULL as version is a required field for ruleset.
 */
AWS_SDKUTILS_API struct aws_byte_cursor aws_endpoints_ruleset_get_version(const struct aws_endpoints_ruleset *ruleset);

/*
 * Ruleset service id.
 * Returned pointer is owned by ruleset.
 * Can be NULL if not specified in ruleset.
 */
AWS_SDKUTILS_API struct aws_byte_cursor aws_endpoints_ruleset_get_service_id(
    const struct aws_endpoints_ruleset *ruleset);

/*
******************************
* Rule engine
******************************
*/

/**
 * Create new rule engine for a given ruleset.
 * In cases of failure NULL is returned and last error is set.
 */
AWS_SDKUTILS_API struct aws_endpoints_rule_engine *aws_endpoints_rule_engine_new(
    struct aws_allocator *allocator,
    struct aws_endpoints_ruleset *ruleset,
    struct aws_partitions_config *partitions_config);

/*
 * Increment rule engine ref count.
 */
AWS_SDKUTILS_API struct aws_endpoints_rule_engine *aws_endpoints_rule_engine_acquire(
    struct aws_endpoints_rule_engine *rule_engine);

/*
 * Decrement rule engine ref count.
 */
AWS_SDKUTILS_API struct aws_endpoints_rule_engine *aws_endpoints_rule_engine_release(
    struct aws_endpoints_rule_engine *rule_engine);

/*
 * Creates new request context.
 * This is basically a property bag containing all request parameter values needed to
 * resolve endpoint. Parameter value names must match parameter names specified
 * in ruleset.
 * Caller is responsible for releasing request context.
 * Note on usage in bindings:
 * - Consider exposing it as a custom property bag or a standard map and then
 *   transform it into request context.
 */
AWS_SDKUTILS_API struct aws_endpoints_request_context *aws_endpoints_request_context_new(
    struct aws_allocator *allocator);

/*
 * Increment resolved endpoint ref count.
 */
AWS_SDKUTILS_API struct aws_endpoints_request_context *aws_endpoints_request_context_acquire(
    struct aws_endpoints_request_context *request_context);

/*
 * Decrement resolved endpoint ref count.
 */
AWS_SDKUTILS_API struct aws_endpoints_request_context *aws_endpoints_request_context_release(
    struct aws_endpoints_request_context *request_context);

/*
 * Add string value to request context.
 * Note: this function will make a copy of the memory backing the cursors.
 * The function will override any previous value stored in the context with the
 * same name.
 */
AWS_SDKUTILS_API int aws_endpoints_request_context_add_string(
    struct aws_allocator *allocator,
    struct aws_endpoints_request_context *context,
    struct aws_byte_cursor name,
    struct aws_byte_cursor value);

/*
 * Add boolean value to request context.
 * The function will override any previous value stored in the context with the
 * same name.
 */
AWS_SDKUTILS_API int aws_endpoints_request_context_add_boolean(
    struct aws_allocator *allocator,
    struct aws_endpoints_request_context *context,
    struct aws_byte_cursor name,
    bool value);

/*
 * Add string array value to request context.
 * Note: this function will make a copy of the memory backing the cursors.
 * The function will override any previous value stored in the context with the
 * same name.
 */
AWS_SDKUTILS_API int aws_endpoints_request_context_add_string_array(
    struct aws_allocator *allocator,
    struct aws_endpoints_request_context *context,
    struct aws_byte_cursor name,
    const struct aws_byte_cursor *value_array,
    size_t len);

/*
 * Resolve an endpoint given request context.
 * Resolved endpoint is returned through out_resolved_endpoint.
 * In cases of error out_resolved_endpoint is set to NULL and error is returned.
 * Resolved endpoint is ref counter and caller is responsible for releasing it.
 */
AWS_SDKUTILS_API int aws_endpoints_rule_engine_resolve(
    struct aws_endpoints_rule_engine *engine,
    const struct aws_endpoints_request_context *context,
    struct aws_endpoints_resolved_endpoint **out_resolved_endpoint);

/*
 * Increment resolved endpoint ref count.
 */
AWS_SDKUTILS_API struct aws_endpoints_resolved_endpoint *aws_endpoints_resolved_endpoint_acquire(
    struct aws_endpoints_resolved_endpoint *resolved_endpoint);

/*
 * Decrement resolved endpoint ref count.
 */
AWS_SDKUTILS_API struct aws_endpoints_resolved_endpoint *aws_endpoints_resolved_endpoint_release(
    struct aws_endpoints_resolved_endpoint *resolved_endpoint);

/*
 * Get type of resolved endpoint.
 */
AWS_SDKUTILS_API enum aws_endpoints_resolved_endpoint_type aws_endpoints_resolved_endpoint_get_type(
    const struct aws_endpoints_resolved_endpoint *resolved_endpoint);

/*
 * Get url for the resolved endpoint.
 * Valid only if resolved endpoint has endpoint type and will error otherwise.
 */
AWS_SDKUTILS_API int aws_endpoints_resolved_endpoint_get_url(
    const struct aws_endpoints_resolved_endpoint *resolved_endpoint,
    struct aws_byte_cursor *out_url);

/*
 * Get properties for the resolved endpoint.
 * Note: properties is a json string containing additional data for a given
 * endpoint. Data is not typed and is not guaranteed to change in the future.
 * For use at callers discretion.
 * Valid only if resolved endpoint has endpoint type and will error otherwise.
 */
AWS_SDKUTILS_API int aws_endpoints_resolved_endpoint_get_properties(
    const struct aws_endpoints_resolved_endpoint *resolved_endpoint,
    struct aws_byte_cursor *out_properties);

/*
 * Get headers for the resolved endpoint.
 * out_headers type is aws_hash_table with (aws_string *) as key
 * and (aws_array_list * of aws_string *) as value.
 * Note on usage in bindings:
 * - this is a map to a list of strings and can be implemented as such in the
 *   target language with deep copy of all underlying strings.
 * Valid only if resolved endpoint has endpoint type and will error otherwise.
 */
AWS_SDKUTILS_API int aws_endpoints_resolved_endpoint_get_headers(
    const struct aws_endpoints_resolved_endpoint *resolved_endpoint,
    const struct aws_hash_table **out_headers);

/*
 * Get error for the resolved endpoint.
 * Valid only if resolved endpoint has error type and will error otherwise.
 */
AWS_SDKUTILS_API int aws_endpoints_resolved_endpoint_get_error(
    const struct aws_endpoints_resolved_endpoint *resolved_endpoint,
    struct aws_byte_cursor *out_error);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_SDKUTILS_ENDPOINTS_RULESET_H */
