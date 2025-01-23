/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/byte_buf.h>
#include <aws/common/string.h>

#include <aws/common/json.h>
#include <aws/common/private/external_module_impl.h>

#include "external/cJSON.h"

static struct aws_allocator *s_aws_json_module_allocator = NULL;
static bool s_aws_json_module_initialized = false;

struct aws_json_value *aws_json_value_new_string(struct aws_allocator *allocator, struct aws_byte_cursor string) {
    struct aws_string *tmp = aws_string_new_from_cursor(allocator, &string);
    void *ret_val = cJSON_CreateString(aws_string_c_str(tmp));
    aws_string_destroy_secure(tmp);
    return ret_val;
}

struct aws_json_value *aws_json_value_new_string_from_c_str(struct aws_allocator *allocator, const char *string) {
    (void)allocator; /* No need for allocator. It is overriden through hooks. */
    void *ret_val = cJSON_CreateString(string);
    return ret_val;
}

struct aws_json_value *aws_json_value_new_number(struct aws_allocator *allocator, double number) {
    (void)allocator; // prevent warnings over unused parameter
    return (void *)cJSON_CreateNumber(number);
}

struct aws_json_value *aws_json_value_new_array(struct aws_allocator *allocator) {
    (void)allocator; // prevent warnings over unused parameter
    return (void *)cJSON_CreateArray();
}

struct aws_json_value *aws_json_value_new_boolean(struct aws_allocator *allocator, bool boolean) {
    (void)allocator; // prevent warnings over unused parameter
    return (void *)cJSON_CreateBool(boolean);
}

struct aws_json_value *aws_json_value_new_null(struct aws_allocator *allocator) {
    (void)allocator; // prevent warnings over unused parameter
    return (void *)cJSON_CreateNull();
}

struct aws_json_value *aws_json_value_new_object(struct aws_allocator *allocator) {
    (void)allocator; // prevent warnings over unused parameter
    return (void *)cJSON_CreateObject();
}

int aws_json_value_get_string(const struct aws_json_value *value, struct aws_byte_cursor *output) {
    const struct cJSON *cjson = (const struct cJSON *)value;
    if (!cJSON_IsString(cjson)) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
    *output = aws_byte_cursor_from_c_str(cJSON_GetStringValue(cjson));
    return AWS_OP_SUCCESS;
}

int aws_json_value_get_number(const struct aws_json_value *value, double *output) {
    const struct cJSON *cjson = (const struct cJSON *)value;
    if (!cJSON_IsNumber(cjson)) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
    *output = cjson->valuedouble;
    return AWS_OP_SUCCESS;
}

int aws_json_value_get_boolean(const struct aws_json_value *value, bool *output) {
    const struct cJSON *cjson = (const struct cJSON *)value;
    if (!cJSON_IsBool(cjson)) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
    *output = cjson->type == cJSON_True;
    return AWS_OP_SUCCESS;
}

int aws_json_value_add_to_object(
    struct aws_json_value *object,
    struct aws_byte_cursor key,
    struct aws_json_value *value) {

    struct aws_string *tmp = aws_string_new_from_cursor(s_aws_json_module_allocator, &key);
    int result = aws_json_value_add_to_object_c_str(object, aws_string_c_str(tmp), value);

    aws_string_destroy_secure(tmp);
    return result;
}

int aws_json_value_add_to_object_c_str(struct aws_json_value *object, const char *key, struct aws_json_value *value) {

    struct cJSON *cjson = (struct cJSON *)object;
    if (!cJSON_IsObject(cjson)) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct cJSON *cjson_value = (struct cJSON *)value;
    if (cJSON_IsInvalid(cjson_value)) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
    if (cJSON_HasObjectItem(cjson, key)) {
        return AWS_OP_ERR;
    }

    cJSON_AddItemToObject(cjson, key, cjson_value);
    return AWS_OP_SUCCESS;
}

struct aws_json_value *aws_json_value_get_from_object(const struct aws_json_value *object, struct aws_byte_cursor key) {

    struct aws_string *tmp = aws_string_new_from_cursor(s_aws_json_module_allocator, &key);
    void *return_value = aws_json_value_get_from_object_c_str(object, aws_string_c_str(tmp));

    aws_string_destroy_secure(tmp);
    return return_value;
}

struct aws_json_value *aws_json_value_get_from_object_c_str(const struct aws_json_value *object, const char *key) {
    const struct cJSON *cjson = (const struct cJSON *)object;
    if (!cJSON_IsObject(cjson)) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }
    if (!cJSON_HasObjectItem(cjson, key)) {
        return NULL;
    }

    return (void *)cJSON_GetObjectItem(cjson, key);
}

bool aws_json_value_has_key(const struct aws_json_value *object, struct aws_byte_cursor key) {

    struct aws_string *tmp = aws_string_new_from_cursor(s_aws_json_module_allocator, &key);
    bool result = aws_json_value_has_key_c_str(object, aws_string_c_str(tmp));

    aws_string_destroy_secure(tmp);
    return result;
}

bool aws_json_value_has_key_c_str(const struct aws_json_value *object, const char *key) {
    const struct cJSON *cjson = (const struct cJSON *)object;
    if (!cJSON_IsObject(cjson)) {
        return false;
    }
    if (!cJSON_HasObjectItem(cjson, key)) {
        return false;
    }

    return true;
}

int aws_json_value_remove_from_object(struct aws_json_value *object, struct aws_byte_cursor key) {

    struct aws_string *tmp = aws_string_new_from_cursor(s_aws_json_module_allocator, &key);
    int result = aws_json_value_remove_from_object_c_str(object, aws_string_c_str(tmp));

    aws_string_destroy_secure(tmp);
    return result;
}

int aws_json_value_remove_from_object_c_str(struct aws_json_value *object, const char *key) {
    struct cJSON *cjson = (struct cJSON *)object;
    if (!cJSON_IsObject(cjson)) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
    if (!cJSON_HasObjectItem(cjson, key)) {
        return AWS_OP_ERR;
    }

    cJSON_DeleteItemFromObject(cjson, key);
    return AWS_OP_SUCCESS;
}

int aws_json_const_iterate_object(
    const struct aws_json_value *object,
    aws_json_on_member_encountered_const_fn *on_member,
    void *user_data) {
    int result = AWS_OP_ERR;

    const struct cJSON *cjson = (const struct cJSON *)object;
    if (!cJSON_IsObject(cjson)) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        goto done;
    }

    const cJSON *key = NULL;
    cJSON_ArrayForEach(key, cjson) {
        bool should_continue = true;
        struct aws_byte_cursor key_cur = aws_byte_cursor_from_c_str(key->string);
        if (on_member(&key_cur, (const struct aws_json_value *)key, &should_continue, user_data)) {
            goto done;
        }

        if (!should_continue) {
            break;
        }
    }

    result = AWS_OP_SUCCESS;

done:
    return result;
}

int aws_json_value_add_array_element(struct aws_json_value *array, const struct aws_json_value *value) {

    struct cJSON *cjson = (struct cJSON *)array;
    if (!cJSON_IsArray(cjson)) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct cJSON *cjson_value = (struct cJSON *)value;
    if (cJSON_IsInvalid(cjson_value)) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    cJSON_AddItemToArray(cjson, cjson_value);
    return AWS_OP_SUCCESS;
}

struct aws_json_value *aws_json_get_array_element(const struct aws_json_value *array, size_t index) {
    const struct cJSON *cjson = (const struct cJSON *)array;
    if (!cJSON_IsArray(cjson)) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (index > (size_t)cJSON_GetArraySize(cjson)) {
        aws_raise_error(AWS_ERROR_INVALID_INDEX);
        return NULL;
    }

    return (void *)cJSON_GetArrayItem(cjson, (int)index);
}

size_t aws_json_get_array_size(const struct aws_json_value *array) {
    const struct cJSON *cjson = (const struct cJSON *)array;
    if (!cJSON_IsArray(cjson)) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    return cJSON_GetArraySize(cjson);
}

int aws_json_value_remove_array_element(struct aws_json_value *array, size_t index) {

    struct cJSON *cjson = (struct cJSON *)array;
    if (!cJSON_IsArray(cjson)) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (index > (size_t)cJSON_GetArraySize(cjson)) {
        return aws_raise_error(AWS_ERROR_INVALID_INDEX);
    }

    cJSON_DeleteItemFromArray(cjson, (int)index);
    return AWS_OP_SUCCESS;
}

int aws_json_const_iterate_array(
    const struct aws_json_value *array,
    aws_json_on_value_encountered_const_fn *on_value,
    void *user_data) {
    int result = AWS_OP_ERR;

    const struct cJSON *cjson = (const struct cJSON *)array;
    if (!cJSON_IsArray(cjson)) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        goto done;
    }

    size_t idx = 0;
    const cJSON *value = NULL;
    cJSON_ArrayForEach(value, cjson) {
        bool should_continue = true;
        if (on_value(idx, (const struct aws_json_value *)value, &should_continue, user_data)) {
            goto done;
        }

        if (!should_continue) {
            break;
        }
        ++idx;
    }

    result = AWS_OP_SUCCESS;

done:
    return result;
}

bool aws_json_value_compare(const struct aws_json_value *a, const struct aws_json_value *b, bool is_case_sensitive) {
    const struct cJSON *cjson_a = (const struct cJSON *)a;
    const struct cJSON *cjson_b = (const struct cJSON *)b;
    return cJSON_Compare(cjson_a, cjson_b, is_case_sensitive);
}

struct aws_json_value *aws_json_value_duplicate(const struct aws_json_value *value) {
    const struct cJSON *cjson = (const struct cJSON *)value;
    if (cJSON_IsInvalid(cjson)) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct cJSON *ret = cJSON_Duplicate(cjson, true);
    if (ret == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    return (void *)ret;
}

bool aws_json_value_is_string(const struct aws_json_value *value) {
    const struct cJSON *cjson = (const struct cJSON *)value;
    if (cJSON_IsInvalid(cjson)) {
        return false;
    }
    return cJSON_IsString(cjson);
}

bool aws_json_value_is_number(const struct aws_json_value *value) {
    const struct cJSON *cjson = (const struct cJSON *)value;
    if (cJSON_IsInvalid(cjson)) {
        return false;
    }
    return cJSON_IsNumber(cjson);
}

bool aws_json_value_is_array(const struct aws_json_value *value) {
    const struct cJSON *cjson = (const struct cJSON *)value;
    if (cJSON_IsInvalid(cjson)) {
        return false;
    }
    return cJSON_IsArray(cjson);
}

bool aws_json_value_is_boolean(const struct aws_json_value *value) {
    const struct cJSON *cjson = (const struct cJSON *)value;
    if (cJSON_IsInvalid(cjson)) {
        return false;
    }
    return cJSON_IsBool(cjson);
}

bool aws_json_value_is_null(const struct aws_json_value *value) {
    const struct cJSON *cjson = (const struct cJSON *)value;
    if (cJSON_IsInvalid(cjson)) {
        return false;
    }
    return cJSON_IsNull(cjson);
}

bool aws_json_value_is_object(const struct aws_json_value *value) {
    const struct cJSON *cjson = (const struct cJSON *)value;
    if (cJSON_IsInvalid(cjson)) {
        return false;
    }
    return cJSON_IsObject(cjson);
}

static void *s_aws_cJSON_alloc(size_t sz) {
    return aws_mem_acquire(s_aws_json_module_allocator, sz);
}

static void s_aws_cJSON_free(void *ptr) {
    aws_mem_release(s_aws_json_module_allocator, ptr);
}

void aws_json_module_init(struct aws_allocator *allocator) {
    if (!s_aws_json_module_initialized) {
        s_aws_json_module_allocator = allocator;
        struct cJSON_Hooks allocation_hooks = {
            .malloc_fn = s_aws_cJSON_alloc,
            .free_fn = s_aws_cJSON_free,
        };
        cJSON_InitHooks(&allocation_hooks);
        s_aws_json_module_initialized = true;
    }
}

void aws_json_module_cleanup(void) {
    if (s_aws_json_module_initialized) {
        s_aws_json_module_allocator = NULL;
        s_aws_json_module_initialized = false;
    }
}

void aws_json_value_destroy(struct aws_json_value *value) {
    struct cJSON *cjson = (struct cJSON *)value;
    /* Note: cJSON_IsInvalid returns false for NULL values, so we need explicit
        check for NULL to skip delete */
    if (cjson != NULL && !cJSON_IsInvalid(cjson)) {
        cJSON_Delete(cjson);
    }
}

int aws_byte_buf_append_json_string(const struct aws_json_value *value, struct aws_byte_buf *output) {
    const struct cJSON *cjson = (const struct cJSON *)value;
    if (cJSON_IsInvalid(cjson)) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    char *tmp = cJSON_PrintUnformatted(cjson);
    if (tmp == NULL) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    // Append the text to the byte buffer
    struct aws_byte_cursor tmp_cursor = aws_byte_cursor_from_c_str(tmp);
    int return_val = aws_byte_buf_append_dynamic_secure(output, &tmp_cursor);
    s_aws_cJSON_free(tmp); // free the char* now that we do not need it
    return return_val;
}

int aws_byte_buf_append_json_string_formatted(const struct aws_json_value *value, struct aws_byte_buf *output) {
    const struct cJSON *cjson = (const struct cJSON *)value;
    if (cJSON_IsInvalid(cjson)) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    char *tmp = cJSON_Print(cjson);
    if (tmp == NULL) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    // Append the text to the byte buffer
    struct aws_byte_cursor tmp_cursor = aws_byte_cursor_from_c_str(tmp);
    int return_val = aws_byte_buf_append_dynamic_secure(output, &tmp_cursor);
    s_aws_cJSON_free(tmp); // free the char* now that we do not need it
    return return_val;
}

struct aws_json_value *aws_json_value_new_from_string(struct aws_allocator *allocator, struct aws_byte_cursor string) {
    struct aws_string *tmp = aws_string_new_from_cursor(allocator, &string);
    struct cJSON *cjson = cJSON_Parse(aws_string_c_str(tmp));
    aws_string_destroy_secure(tmp);
    return (void *)cjson;
}
