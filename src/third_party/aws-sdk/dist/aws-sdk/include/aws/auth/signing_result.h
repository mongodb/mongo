#ifndef AWS_AUTH_SIGNING_RESULT_H
#define AWS_AUTH_SIGNING_RESULT_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/auth.h>

#include <aws/common/hash_table.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_array_list;
struct aws_byte_cursor;
struct aws_http_message;
struct aws_string;

struct aws_signing_result_property {
    struct aws_string *name;
    struct aws_string *value;
};

/**
 * A structure for tracking all the signer-requested changes to a signable.  Interpreting
 * these changes is signing-algorithm specific.
 *
 * A signing result consists of
 *
 *   (1) Properties - A set of key-value pairs
 *   (2) Property Lists - A set of named key-value pair lists
 *
 * The hope is that these two generic structures are enough to model the changes required
 * by any generic message-signing algorithm.
 *
 * Note that the key-value pairs of a signing_result are different types (but same intent) as
 * the key-value pairs in the signable interface.  This is because the signing result stands alone
 * and owns its own copies of all values, whereas a signable can wrap an existing object and thus
 * use non-owning references (like byte cursors) if appropriate to its implementation.
 */
struct aws_signing_result {
    struct aws_allocator *allocator;
    struct aws_hash_table properties;
    struct aws_hash_table property_lists;
};

AWS_EXTERN_C_BEGIN

/**
 * Initialize a signing result to its starting state
 *
 * @param result signing result to initialize
 * @param allocator allocator to use for all memory allocation
 *
 * @return AWS_OP_SUCCESS if initialization was successful, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_signing_result_init(struct aws_signing_result *result, struct aws_allocator *allocator);

/**
 * Clean up all resources held by the signing result
 *
 * @param result signing result to clean up resources for
 */
AWS_AUTH_API
void aws_signing_result_clean_up(struct aws_signing_result *result);

/**
 * Sets the value of a property on a signing result
 *
 * @param result signing result to modify
 * @param property_name name of the property to set
 * @param property_value value that the property should assume
 *
 * @return AWS_OP_SUCCESS if the set was successful, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_signing_result_set_property(
    struct aws_signing_result *result,
    const struct aws_string *property_name,
    const struct aws_byte_cursor *property_value);

/**
 * Gets the value of a property on a signing result
 *
 * @param result signing result to query from
 * @param property_name name of the property to query the value of
 * @param out_property_value output parameter for the property value
 *
 * @return AWS_OP_SUCCESS if the get was successful, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_signing_result_get_property(
    const struct aws_signing_result *result,
    const struct aws_string *property_name,
    struct aws_string **out_property_value);

/**
 * Adds a key-value pair to a named property list.  If the named list does not yet exist, it will be created as
 * an empty list before the pair is added.  No uniqueness checks are made against existing pairs.
 *
 * @param result signing result to modify
 * @param list_name name of the list to add the property key-value pair to
 * @param property_name key value of the key-value pair to append
 * @param property_value property value of the key-value pair to append
 *
 * @return AWS_OP_SUCCESS if the operation was successful, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_signing_result_append_property_list(
    struct aws_signing_result *result,
    const struct aws_string *list_name,
    const struct aws_byte_cursor *property_name,
    const struct aws_byte_cursor *property_value);

/**
 * Gets a named property list on the signing result.  If the list does not exist, *out_list will be set to null
 *
 * @param result signing result to query
 * @param list_name name of the list of key-value pairs to get
 * @param out_list output parameter for the list of key-value pairs
 *
 */
AWS_AUTH_API
void aws_signing_result_get_property_list(
    const struct aws_signing_result *result,
    const struct aws_string *list_name,
    struct aws_array_list **out_list);

/**
 * Looks for a property within a named property list on the signing result.  If the list does not exist, or the property
 * does not exist within the list, *out_value will be set to NULL.
 *
 * @param result signing result to query
 * @param list_name name of the list of key-value pairs to search through for the property
 * @param property_name name of the property to search for within the list
 * @param out_value output parameter for the property value, if found
 *
 */
AWS_AUTH_API
void aws_signing_result_get_property_value_in_property_list(
    const struct aws_signing_result *result,
    const struct aws_string *list_name,
    const struct aws_string *property_name,
    struct aws_string **out_value);

/*
 * Specific implementation that applies a signing result to a mutable http request
 *
 * @param request http request to apply the signing result to
 * @param allocator memory allocator to use for all memory allocation
 * @param result signing result to apply to the request
 *
 * @return AWS_OP_SUCCESS if the application operation was successful, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_apply_signing_result_to_http_request(
    struct aws_http_message *request,
    struct aws_allocator *allocator,
    const struct aws_signing_result *result);

AWS_AUTH_API extern const struct aws_string *g_aws_signing_authorization_header_name;
AWS_AUTH_API extern const struct aws_string *g_aws_signing_authorization_query_param_name;

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_AUTH_SIGNING_RESULT_H */
