/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/signing_result.h>

#include <aws/common/byte_buf.h>
#include <aws/common/string.h>

#define INITIAL_SIGNING_RESULT_PROPERTIES_SIZE 10
#define INITIAL_SIGNING_RESULT_PROPERTY_LISTS_TABLE_SIZE 10
#define INITIAL_SIGNING_RESULT_PROPERTY_LIST_SIZE 10

static void s_aws_signing_result_property_clean_up(struct aws_signing_result_property *pair) {
    aws_string_destroy(pair->name);
    aws_string_destroy(pair->value);
}

static void s_aws_hash_callback_property_list_destroy(void *value) {
    struct aws_array_list *property_list = value;

    size_t property_count = aws_array_list_length(property_list);
    for (size_t i = 0; i < property_count; ++i) {
        struct aws_signing_result_property property;
        AWS_ZERO_STRUCT(property);

        if (aws_array_list_get_at(property_list, &property, i)) {
            continue;
        }

        s_aws_signing_result_property_clean_up(&property);
    }

    struct aws_allocator *allocator = property_list->alloc;
    aws_array_list_clean_up(property_list);

    aws_mem_release(allocator, property_list);
}

int aws_signing_result_init(struct aws_signing_result *result, struct aws_allocator *allocator) {
    AWS_ZERO_STRUCT(*result);

    result->allocator = allocator;
    if (aws_hash_table_init(
            &result->properties,
            allocator,
            INITIAL_SIGNING_RESULT_PROPERTIES_SIZE,
            aws_hash_string,
            aws_hash_callback_string_eq,
            aws_hash_callback_string_destroy,
            aws_hash_callback_string_destroy) ||
        aws_hash_table_init(
            &result->property_lists,
            allocator,
            INITIAL_SIGNING_RESULT_PROPERTY_LISTS_TABLE_SIZE,
            aws_hash_string,
            aws_hash_callback_string_eq,
            aws_hash_callback_string_destroy,
            s_aws_hash_callback_property_list_destroy)) {
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:

    aws_signing_result_clean_up(result);

    return AWS_OP_ERR;
}

void aws_signing_result_clean_up(struct aws_signing_result *result) {
    aws_hash_table_clean_up(&result->properties);
    aws_hash_table_clean_up(&result->property_lists);
}

int aws_signing_result_set_property(
    struct aws_signing_result *result,
    const struct aws_string *property_name,
    const struct aws_byte_cursor *property_value) {

    struct aws_string *name = NULL;
    struct aws_string *value = NULL;

    name = aws_string_new_from_string(result->allocator, property_name);
    value = aws_string_new_from_array(result->allocator, property_value->ptr, property_value->len);
    if (name == NULL || value == NULL) {
        goto on_error;
    }

    if (aws_hash_table_put(&result->properties, name, value, NULL)) {
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:

    aws_string_destroy(name);
    aws_string_destroy(value);

    return AWS_OP_ERR;
}

int aws_signing_result_get_property(
    const struct aws_signing_result *result,
    const struct aws_string *property_name,
    struct aws_string **out_property_value) {

    struct aws_hash_element *element = NULL;
    aws_hash_table_find(&result->properties, property_name, &element);

    *out_property_value = NULL;
    if (element != NULL) {
        *out_property_value = element->value;
    }

    return AWS_OP_SUCCESS;
}

static struct aws_array_list *s_get_or_create_property_list(
    struct aws_signing_result *result,
    const struct aws_string *list_name) {
    struct aws_hash_element *element = NULL;
    aws_hash_table_find(&result->property_lists, list_name, &element);

    if (element != NULL) {
        return element->value;
    }

    struct aws_array_list *properties = aws_mem_acquire(result->allocator, sizeof(struct aws_array_list));
    if (properties == NULL) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*properties);
    struct aws_string *name_copy = aws_string_new_from_string(result->allocator, list_name);
    if (name_copy == NULL) {
        goto on_error;
    }

    if (aws_array_list_init_dynamic(
            properties,
            result->allocator,
            INITIAL_SIGNING_RESULT_PROPERTY_LIST_SIZE,
            sizeof(struct aws_signing_result_property))) {
        goto on_error;
    }

    if (aws_hash_table_put(&result->property_lists, name_copy, properties, NULL)) {
        goto on_error;
    }

    return properties;

on_error:

    aws_string_destroy(name_copy);
    aws_array_list_clean_up(properties);
    aws_mem_release(result->allocator, properties);

    return NULL;
}

int aws_signing_result_append_property_list(
    struct aws_signing_result *result,
    const struct aws_string *list_name,
    const struct aws_byte_cursor *property_name,
    const struct aws_byte_cursor *property_value) {

    struct aws_array_list *properties = s_get_or_create_property_list(result, list_name);
    if (properties == NULL) {
        return AWS_OP_ERR;
    }

    struct aws_string *name = NULL;
    struct aws_string *value = NULL;

    name = aws_string_new_from_array(result->allocator, property_name->ptr, property_name->len);
    value = aws_string_new_from_array(result->allocator, property_value->ptr, property_value->len);

    struct aws_signing_result_property property;
    property.name = name;
    property.value = value;

    if (aws_array_list_push_back(properties, &property)) {
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:

    aws_string_destroy(name);
    aws_string_destroy(value);

    return AWS_OP_ERR;
}

void aws_signing_result_get_property_list(
    const struct aws_signing_result *result,
    const struct aws_string *list_name,
    struct aws_array_list **out_list) {

    *out_list = NULL;

    struct aws_hash_element *element = NULL;
    aws_hash_table_find(&result->property_lists, list_name, &element);

    if (element != NULL) {
        *out_list = element->value;
    }
}

void aws_signing_result_get_property_value_in_property_list(
    const struct aws_signing_result *result,
    const struct aws_string *list_name,
    const struct aws_string *property_name,
    struct aws_string **out_value) {

    *out_value = NULL;

    struct aws_array_list *property_list = NULL;
    aws_signing_result_get_property_list(result, list_name, &property_list);
    if (property_list == NULL) {
        return;
    }

    size_t pair_count = aws_array_list_length(property_list);
    for (size_t i = 0; i < pair_count; ++i) {
        struct aws_signing_result_property pair;
        AWS_ZERO_STRUCT(pair);
        if (aws_array_list_get_at(property_list, &pair, i)) {
            continue;
        }

        if (pair.name == NULL) {
            continue;
        }

        if (aws_string_eq_ignore_case(property_name, pair.name)) {
            *out_value = pair.value;
            break;
        }
    }
}
