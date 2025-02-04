/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/byte_buf.h>
#include <aws/common/environment.h>
#include <aws/common/file.h>
#include <aws/common/hash_table.h>
#include <aws/common/logging.h>
#include <aws/common/ref_count.h>
#include <aws/common/string.h>
#include <aws/sdkutils/aws_profile.h>

#define PROPERTIES_TABLE_DEFAULT_SIZE 4
#define PROFILE_TABLE_DEFAULT_SIZE 5

struct aws_profile_property {
    struct aws_allocator *allocator;
    struct aws_string *name;
    struct aws_string *value;
    struct aws_hash_table sub_properties;
    bool is_empty_valued;
};

struct aws_profile {
    struct aws_allocator *allocator;
    struct aws_string *name;
    struct aws_hash_table properties;
    bool has_profile_prefix;
};

struct aws_profile_collection {
    struct aws_allocator *allocator;
    enum aws_profile_source_type profile_source;
    /*
     * Array of aws_hash_table for each section type.
     * Each table is a map from section identifier to aws_profile.
     * key: struct aws_string*
     * value: struct aws_profile*
     */
    struct aws_hash_table sections[AWS_PROFILE_SECTION_TYPE_COUNT];
    struct aws_ref_count ref_count;
};

/*
 * Character-based profile parse helper functions
 */
static bool s_is_assignment_operator(uint8_t value) {
    return (char)value == '=';
}

static bool s_is_not_assignment_operator(uint8_t value) {
    return !s_is_assignment_operator(value);
}

static bool s_is_identifier(uint8_t value) {
    char value_as_char = (char)value;

    if ((value_as_char >= 'A' && value_as_char <= 'Z') || (value_as_char >= 'a' && value_as_char <= 'z') ||
        (value_as_char >= '0' && value_as_char <= '9') || value_as_char == '\\' || value_as_char == '_' ||
        value_as_char == '-') {
        return true;
    }

    return false;
}

static bool s_is_whitespace(uint8_t value) {
    char value_as_char = (char)value;

    switch (value_as_char) {
        case '\t':
        case '\n':
        case '\r':
        case ' ':
            return true;

        default:
            return false;
    }
}

static bool s_is_comment_token(uint8_t value) {
    char char_value = (char)value;

    return char_value == '#' || char_value == ';';
}

static bool s_is_not_comment_token(uint8_t value) {
    return !s_is_comment_token(value);
}

static bool s_is_profile_start(uint8_t value) {
    return (char)value == '[';
}

static bool s_is_not_profile_end(uint8_t value) {
    return (char)value != ']';
}

static bool s_is_carriage_return(uint8_t value) {
    return (char)value == '\r';
}

/*
 * Line and string based parse helper functions
 */
static bool s_is_comment_line(const struct aws_byte_cursor *line_cursor) {
    char first_char = *line_cursor->ptr;
    return first_char == '#' || first_char == ';';
}

static bool s_is_whitespace_line(const struct aws_byte_cursor *line_cursor) {
    return aws_byte_cursor_left_trim_pred(line_cursor, s_is_whitespace).len == 0;
}

AWS_STATIC_STRING_FROM_LITERAL(s_default_profile_name, "default");

static bool s_is_default_profile_name(const struct aws_byte_cursor *profile_name) {
    return aws_string_eq_byte_cursor(s_default_profile_name, profile_name);
}

/*
 * Consume helpers
 */

/*
 * Consumes characters as long as a predicate is satisfied.  "parsed" is optional and contains the consumed range as
 * output. Returns true if anything was consumed.
 *
 * On success, start is updated to the new position.
 */
static bool s_parse_by_character_predicate(
    struct aws_byte_cursor *start,
    aws_byte_predicate_fn *predicate,
    struct aws_byte_cursor *parsed,
    size_t maximum_allowed) {

    uint8_t *current_ptr = start->ptr;
    uint8_t *end_ptr = start->ptr + start->len;
    if (maximum_allowed > 0 && maximum_allowed < start->len) {
        end_ptr = start->ptr + maximum_allowed;
    }

    while (current_ptr < end_ptr) {
        if (!predicate(*current_ptr)) {
            break;
        }

        ++current_ptr;
    }

    size_t consumed = current_ptr - start->ptr;
    if (parsed != NULL) {
        parsed->ptr = start->ptr;
        parsed->len = consumed;
    }

    aws_byte_cursor_advance(start, consumed);

    return consumed > 0;
}

/*
 * Consumes characters if they match a token string.  "parsed" is optional and contains the consumed range as output.
 * Returns true if anything was consumed.
 *
 * On success, start is updated to the new position.
 */
static bool s_parse_by_token(
    struct aws_byte_cursor *start,
    const struct aws_string *token,
    struct aws_byte_cursor *parsed) {

    bool matched = false;

    if (token->len <= start->len) {
        matched = strncmp((const char *)start->ptr, aws_string_c_str(token), token->len) == 0;
    }

    if (parsed != NULL) {
        parsed->ptr = start->ptr;
        parsed->len = matched ? token->len : 0;
    }

    if (matched) {
        aws_byte_cursor_advance(start, token->len);
    }

    return matched;
}

/*
 * Parse context and logging
 */

struct profile_file_parse_context {
    const struct aws_string *source_file_path;
    struct aws_profile_collection *profile_collection;
    struct aws_profile *current_profile;
    struct aws_profile_property *current_property;
    struct aws_byte_cursor current_line;
    int parse_error;
    int current_line_number;
    bool has_seen_profile;
};

AWS_STATIC_STRING_FROM_LITERAL(s_none_string, "<None>");

static void s_log_parse_context(enum aws_log_level log_level, const struct profile_file_parse_context *context) {
    AWS_LOGF(
        log_level,
        AWS_LS_SDKUTILS_PROFILE,
        "Profile Parse context:\n Source File:%s\n Line: %d\n Current Profile: %s\n Current Property: %s",
        context->source_file_path ? context->source_file_path->bytes : s_none_string->bytes,
        context->current_line_number,
        context->current_profile ? context->current_profile->name->bytes : s_none_string->bytes,
        context->current_property ? context->current_property->name->bytes : s_none_string->bytes);
}

/*
 * aws_profile_property APIs
 */

static void s_profile_property_destroy(struct aws_profile_property *property) {
    if (property == NULL) {
        return;
    }

    aws_string_destroy(property->name);
    aws_string_destroy(property->value);

    aws_hash_table_clean_up(&property->sub_properties);

    aws_mem_release(property->allocator, property);
}

struct aws_profile_property *aws_profile_property_new(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *name,
    const struct aws_byte_cursor *value) {

    struct aws_profile_property *property =
        (struct aws_profile_property *)aws_mem_acquire(allocator, sizeof(struct aws_profile_property));
    if (property == NULL) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*property);
    property->allocator = allocator;

    if (aws_hash_table_init(
            &property->sub_properties,
            allocator,
            0,
            aws_hash_string,
            aws_hash_callback_string_eq,
            aws_hash_callback_string_destroy,
            aws_hash_callback_string_destroy)) {
        goto on_error;
    }

    property->value = aws_string_new_from_array(allocator, value->ptr, value->len);
    if (property->value == NULL) {
        goto on_error;
    }

    property->name = aws_string_new_from_array(allocator, name->ptr, name->len);
    if (property->name == NULL) {
        goto on_error;
    }

    property->is_empty_valued = value->len == 0;

    return property;

on_error:
    s_profile_property_destroy(property);

    return NULL;
}

AWS_STATIC_STRING_FROM_LITERAL(s_newline, "\n");

/*
 * Continuations are applied to the property value by concatenating the old value and the new value, with a '\n'
 * in between.
 */
static int s_profile_property_add_continuation(
    struct aws_profile_property *property,
    const struct aws_byte_cursor *continuation_value) {

    int result = AWS_OP_ERR;
    struct aws_byte_buf concatenation;
    if (aws_byte_buf_init(&concatenation, property->allocator, property->value->len + continuation_value->len + 1)) {
        return result;
    }

    struct aws_byte_cursor old_value = aws_byte_cursor_from_string(property->value);
    if (aws_byte_buf_append(&concatenation, &old_value)) {
        goto on_generic_failure;
    }

    struct aws_byte_cursor newline = aws_byte_cursor_from_string(s_newline);
    if (aws_byte_buf_append(&concatenation, &newline)) {
        goto on_generic_failure;
    }

    if (aws_byte_buf_append(&concatenation, continuation_value)) {
        goto on_generic_failure;
    }

    struct aws_string *new_value =
        aws_string_new_from_array(property->allocator, concatenation.buffer, concatenation.len);
    if (new_value == NULL) {
        goto on_generic_failure;
    }

    result = AWS_OP_SUCCESS;
    aws_string_destroy(property->value);
    property->value = new_value;

on_generic_failure:
    aws_byte_buf_clean_up(&concatenation);

    return result;
}

static int s_profile_property_add_sub_property(
    struct aws_profile_property *property,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *value,
    const struct profile_file_parse_context *context) {

    struct aws_string *key_string = aws_string_new_from_array(property->allocator, key->ptr, key->len);
    if (key_string == NULL) {
        return AWS_OP_ERR;
    }

    struct aws_string *value_string = aws_string_new_from_array(property->allocator, value->ptr, value->len);
    if (value_string == NULL) {
        goto on_failure;
    }

    int was_present = 0;
    aws_hash_table_remove(&property->sub_properties, key_string, NULL, &was_present);
    if (was_present) {
        AWS_LOGF_DEBUG(
            AWS_LS_SDKUTILS_PROFILE,
            "subproperty \"%s\" of property \"%s\" had value overridden with new value",
            key_string->bytes,
            property->name->bytes);
        s_log_parse_context(AWS_LL_WARN, context);
    }

    if (aws_hash_table_put(&property->sub_properties, key_string, value_string, NULL)) {
        goto on_failure;
    }

    return AWS_OP_SUCCESS;

on_failure:

    if (value_string) {
        aws_string_destroy(value_string);
    }

    aws_string_destroy(key_string);

    return AWS_OP_ERR;
}

static int s_profile_property_merge(struct aws_profile_property *dest, const struct aws_profile_property *source) {

    AWS_ASSERT(dest != NULL && source != NULL);

    /*
     * Source value overwrites any existing dest value
     */
    if (source->value) {
        struct aws_string *new_value = aws_string_new_from_string(dest->allocator, source->value);
        if (new_value == NULL) {
            return AWS_OP_ERR;
        }

        if (dest->value) {
            AWS_LOGF_DEBUG(
                AWS_LS_SDKUTILS_PROFILE,
                "property \"%s\" has value \"%s\" replaced during merge",
                dest->name->bytes,
                dest->value->bytes);
            aws_string_destroy(dest->value);
        }

        dest->value = new_value;
    }

    dest->is_empty_valued = source->is_empty_valued;

    /*
     * Iterate sub properties, stomping on conflicts
     */
    struct aws_hash_iter source_iter = aws_hash_iter_begin(&source->sub_properties);
    while (!aws_hash_iter_done(&source_iter)) {
        struct aws_string *source_sub_property = (struct aws_string *)source_iter.element.value;

        struct aws_string *dest_key =
            aws_string_new_from_string(dest->allocator, (struct aws_string *)source_iter.element.key);
        if (dest_key == NULL) {
            return AWS_OP_ERR;
        }

        struct aws_string *dest_sub_property = aws_string_new_from_string(dest->allocator, source_sub_property);
        if (dest_sub_property == NULL) {
            aws_string_destroy(dest_key);
            return AWS_OP_ERR;
        }

        int was_present = 0;
        aws_hash_table_remove(&dest->sub_properties, dest_key, NULL, &was_present);
        if (was_present) {
            AWS_LOGF_DEBUG(
                AWS_LS_SDKUTILS_PROFILE,
                "subproperty \"%s\" of property \"%s\" had value overridden during property merge",
                dest_key->bytes,
                dest->name->bytes);
        }

        if (aws_hash_table_put(&dest->sub_properties, dest_key, dest_sub_property, NULL)) {
            aws_string_destroy(dest_sub_property);
            aws_string_destroy(dest_key);
            return AWS_OP_ERR;
        }

        aws_hash_iter_next(&source_iter);
    }

    return AWS_OP_SUCCESS;
}

/*
 * Helper destroy function for aws_profile's hash table of properties
 */
static void s_property_hash_table_value_destroy(void *value) {
    s_profile_property_destroy((struct aws_profile_property *)value);
}

/*
 * aws_profile APIs
 */

void aws_profile_destroy(struct aws_profile *profile) {
    if (profile == NULL) {
        return;
    }

    aws_string_destroy(profile->name);

    aws_hash_table_clean_up(&profile->properties);

    aws_mem_release(profile->allocator, profile);
}

struct aws_profile *aws_profile_new(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *name,
    bool has_profile_prefix) {

    struct aws_profile *profile = (struct aws_profile *)aws_mem_acquire(allocator, sizeof(struct aws_profile));
    if (profile == NULL) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*profile);

    profile->name = aws_string_new_from_array(allocator, name->ptr, name->len);
    if (profile->name == NULL) {
        goto cleanup;
    }

    if (aws_hash_table_init(
            &profile->properties,
            allocator,
            PROPERTIES_TABLE_DEFAULT_SIZE,
            aws_hash_string,
            aws_hash_callback_string_eq,
            NULL, /* The key is owned by the value (and destroy cleans it up), so we don't have to */
            s_property_hash_table_value_destroy)) {

        goto cleanup;
    }

    profile->allocator = allocator;
    profile->has_profile_prefix = has_profile_prefix;

    return profile;

cleanup:
    aws_profile_destroy(profile);

    return NULL;
}

/*
 * Adds a property to a profile.
 *
 * If a property already exists then the old one is removed and replaced by the
 * new one.
 */
static struct aws_profile_property *s_profile_add_property(
    struct aws_profile *profile,
    const struct aws_byte_cursor *key_cursor,
    const struct aws_byte_cursor *value_cursor) {

    struct aws_profile_property *property = aws_profile_property_new(profile->allocator, key_cursor, value_cursor);
    if (property == NULL) {
        goto on_property_new_failure;
    }

    if (aws_hash_table_put(&profile->properties, property->name, property, NULL)) {
        goto on_hash_table_put_failure;
    }

    return property;

on_hash_table_put_failure:
    s_profile_property_destroy(property);

on_property_new_failure:
    return NULL;
}

const struct aws_profile_property *aws_profile_get_property(
    const struct aws_profile *profile,
    const struct aws_string *property_name) {

    struct aws_hash_element *element = NULL;
    aws_hash_table_find(&profile->properties, property_name, &element);

    if (element == NULL) {
        return NULL;
    }

    return element->value;
}

const struct aws_string *aws_profile_property_get_value(const struct aws_profile_property *property) {
    AWS_PRECONDITION(property);
    return property->value;
}

static int s_profile_merge(struct aws_profile *dest_profile, const struct aws_profile *source_profile) {

    AWS_ASSERT(dest_profile != NULL && source_profile != NULL);

    dest_profile->has_profile_prefix = source_profile->has_profile_prefix;

    struct aws_hash_iter source_iter = aws_hash_iter_begin(&source_profile->properties);
    while (!aws_hash_iter_done(&source_iter)) {
        struct aws_profile_property *source_property = (struct aws_profile_property *)source_iter.element.value;
        struct aws_profile_property *dest_property = (struct aws_profile_property *)aws_profile_get_property(
            dest_profile, (struct aws_string *)source_iter.element.key);
        if (dest_property == NULL) {

            struct aws_byte_cursor empty_value;
            AWS_ZERO_STRUCT(empty_value);

            struct aws_byte_cursor property_name = aws_byte_cursor_from_string(source_iter.element.key);
            dest_property = aws_profile_property_new(dest_profile->allocator, &property_name, &empty_value);
            if (dest_property == NULL) {
                return AWS_OP_ERR;
            }

            if (aws_hash_table_put(&dest_profile->properties, dest_property->name, dest_property, NULL)) {
                s_profile_property_destroy(dest_property);
                return AWS_OP_ERR;
            }
        }

        if (s_profile_property_merge(dest_property, source_property)) {
            return AWS_OP_ERR;
        }

        aws_hash_iter_next(&source_iter);
    }

    return AWS_OP_SUCCESS;
}

/*
 * Hash table destroy helper for profile collection's profiles member
 */
static void s_profile_hash_table_value_destroy(void *value) {
    aws_profile_destroy((struct aws_profile *)value);
}

/*
 * aws_profile_collection APIs
 */

void aws_profile_collection_destroy(struct aws_profile_collection *profile_collection) {
    aws_profile_collection_release(profile_collection);
}

static void s_aws_profile_collection_destroy_internal(struct aws_profile_collection *profile_collection) {
    for (int i = 0; i < AWS_PROFILE_SECTION_TYPE_COUNT; i++) {
        aws_hash_table_clean_up(&profile_collection->sections[i]);
    }
    aws_mem_release(profile_collection->allocator, profile_collection);
}

AWS_STATIC_STRING_FROM_LITERAL(s_profile_token, "profile");
AWS_STATIC_STRING_FROM_LITERAL(s_sso_session_token, "sso-session");

const struct aws_profile *aws_profile_collection_get_profile(
    const struct aws_profile_collection *profile_collection,
    const struct aws_string *profile_name) {
    return aws_profile_collection_get_section(profile_collection, AWS_PROFILE_SECTION_TYPE_PROFILE, profile_name);
}

const struct aws_profile *aws_profile_collection_get_section(
    const struct aws_profile_collection *profile_collection,
    const enum aws_profile_section_type section_type,
    const struct aws_string *section_name) {
    struct aws_hash_element *element = NULL;
    aws_hash_table_find(&profile_collection->sections[section_type], section_name, &element);
    if (element == NULL) {
        return NULL;
    }
    return element->value;
}

static int s_profile_collection_add_profile(
    struct aws_profile_collection *profile_collection,
    const enum aws_profile_section_type section_type,
    const struct aws_byte_cursor *profile_name,
    bool has_prefix,
    const struct profile_file_parse_context *context,
    struct aws_profile **current_profile_out) {

    *current_profile_out = NULL;
    struct aws_string *key =
        aws_string_new_from_array(profile_collection->allocator, profile_name->ptr, profile_name->len);
    if (key == NULL) {
        return AWS_OP_ERR;
    }

    struct aws_profile *existing_profile = NULL;
    struct aws_hash_element *element = NULL;
    aws_hash_table_find(&profile_collection->sections[section_type], key, &element);
    if (element != NULL) {
        existing_profile = element->value;
    }

    aws_string_destroy(key);

    if (section_type == AWS_PROFILE_SECTION_TYPE_PROFILE && profile_collection->profile_source == AWS_PST_CONFIG &&
        s_is_default_profile_name(profile_name)) {
        /*
         *  In a config file, "profile default" always supercedes "default"
         */
        if (!has_prefix && existing_profile && existing_profile->has_profile_prefix) {
            /*
             * existing one supercedes: ignore this (and its properties) completely by failing the add
             * which sets the current profile to NULL
             */
            AWS_LOGF_DEBUG(
                AWS_LS_SDKUTILS_PROFILE,
                "Existing prefixed default config profile supercedes unprefixed default profile");
            s_log_parse_context(AWS_LL_WARN, context);

            return AWS_OP_SUCCESS;
        }

        if (has_prefix && existing_profile && !existing_profile->has_profile_prefix) {
            /*
             * stomp over existing: remove it, then proceed with add
             * element destroy function will clean up the profile and key
             */
            AWS_LOGF_DEBUG(
                AWS_LS_SDKUTILS_PROFILE, "Prefixed default config profile replacing unprefixed default profile");
            s_log_parse_context(AWS_LL_WARN, context);

            aws_hash_table_remove(&profile_collection->sections[section_type], element->key, NULL, NULL);
            existing_profile = NULL;
        }
    }

    if (existing_profile) {
        *current_profile_out = existing_profile;
        return AWS_OP_SUCCESS;
    }

    struct aws_profile *new_profile = aws_profile_new(profile_collection->allocator, profile_name, has_prefix);
    if (new_profile == NULL) {
        goto on_aws_profile_new_failure;
    }

    if (aws_hash_table_put(&profile_collection->sections[section_type], new_profile->name, new_profile, NULL)) {
        goto on_hash_table_put_failure;
    }

    *current_profile_out = new_profile;
    return AWS_OP_SUCCESS;

on_hash_table_put_failure:
    aws_profile_destroy(new_profile);

on_aws_profile_new_failure:
    return AWS_OP_ERR;
}

static int s_profile_collection_merge(
    struct aws_profile_collection *dest_collection,
    const struct aws_profile_collection *source_collection) {

    AWS_ASSERT(dest_collection != NULL && source_collection);
    for (int i = 0; i < AWS_PROFILE_SECTION_TYPE_COUNT; i++) {
        struct aws_hash_iter source_iter = aws_hash_iter_begin(&source_collection->sections[i]);
        while (!aws_hash_iter_done(&source_iter)) {
            struct aws_profile *source_profile = (struct aws_profile *)source_iter.element.value;
            struct aws_profile *dest_profile = (struct aws_profile *)aws_profile_collection_get_profile(
                dest_collection, (struct aws_string *)source_iter.element.key);

            if (dest_profile == NULL) {

                struct aws_byte_cursor name_cursor = aws_byte_cursor_from_string(source_iter.element.key);
                dest_profile =
                    aws_profile_new(dest_collection->allocator, &name_cursor, source_profile->has_profile_prefix);
                if (dest_profile == NULL) {
                    return AWS_OP_ERR;
                }

                if (aws_hash_table_put(&dest_collection->sections[i], dest_profile->name, dest_profile, NULL)) {
                    aws_profile_destroy(dest_profile);
                    return AWS_OP_ERR;
                }
            }

            if (s_profile_merge(dest_profile, source_profile)) {
                return AWS_OP_ERR;
            }

            aws_hash_iter_next(&source_iter);
        }
    }

    return AWS_OP_SUCCESS;
}

struct aws_profile_collection *aws_profile_collection_new_from_merge(
    struct aws_allocator *allocator,
    const struct aws_profile_collection *config_profiles,
    const struct aws_profile_collection *credentials_profiles) {

    struct aws_profile_collection *merged =
        (struct aws_profile_collection *)(aws_mem_acquire(allocator, sizeof(struct aws_profile_collection)));
    if (merged == NULL) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*merged);
    aws_ref_count_init(
        &merged->ref_count, merged, (aws_simple_completion_callback *)s_aws_profile_collection_destroy_internal);
    for (int i = 0; i < AWS_PROFILE_SECTION_TYPE_COUNT; i++) {
        size_t max_profiles = 0;
        if (config_profiles != NULL) {
            max_profiles += aws_hash_table_get_entry_count(&config_profiles->sections[i]);
        }
        if (credentials_profiles != NULL) {
            max_profiles += aws_hash_table_get_entry_count(&credentials_profiles->sections[i]);
        }

        merged->allocator = allocator;
        merged->profile_source = AWS_PST_NONE;

        if (aws_hash_table_init(
                &merged->sections[i],
                allocator,
                max_profiles,
                aws_hash_string,
                aws_hash_callback_string_eq,
                NULL,
                s_profile_hash_table_value_destroy)) {
            goto cleanup;
        }
    }

    if (config_profiles != NULL) {
        if (s_profile_collection_merge(merged, config_profiles)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_PROFILE, "Failed to merge config profile set");
            goto cleanup;
        }
    }

    if (credentials_profiles != NULL) {
        if (s_profile_collection_merge(merged, credentials_profiles)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_PROFILE, "Failed to merge credentials profile set");
            goto cleanup;
        }
    }

    return merged;

cleanup:
    s_aws_profile_collection_destroy_internal(merged);

    return NULL;
}

/*
 * Profile parsing
 */

/*
 * The comment situation in config files is messy.  Some line types require a comment to have at least one
 * whitespace in front of it, while other line types only require a comment token (;, #)  On top of that, some
 * line types do not allow comments at all (get folded into the value).
 *
 */

/*
 * a trailing comment is started by ';' or '#'
 * Only certain types of lines allow comments without prefixing whitespace
 */
static struct aws_byte_cursor s_trim_trailing_comment(const struct aws_byte_cursor *line) {

    struct aws_byte_cursor line_copy = *line;
    struct aws_byte_cursor trimmed;
    s_parse_by_character_predicate(&line_copy, s_is_not_comment_token, &trimmed, 0);

    return trimmed;
}

/*
 * A trailing whitespace comment is started by " ;", " #", "\t;", or "\t#"
 * Certain types of lines require comments be whitespace-prefixed
 */
static struct aws_byte_cursor s_trim_trailing_whitespace_comment(const struct aws_byte_cursor *line) {
    struct aws_byte_cursor trimmed;
    trimmed.ptr = line->ptr;

    uint8_t *current_ptr = line->ptr;
    uint8_t *end_ptr = line->ptr + line->len;

    while (current_ptr < end_ptr) {
        if (s_is_whitespace(*current_ptr)) {
            /*
             * Look ahead 1
             */
            if (current_ptr + 1 < end_ptr && s_is_comment_token(*(current_ptr + 1))) {
                break;
            }
        }

        current_ptr++;
    }

    trimmed.len = current_ptr - line->ptr;

    return trimmed;
}

/**
 * Attempts to parse profile declaration lines
 *
 * Return false if this is not a profile declaration, true otherwise (stop parsing the line)
 */
static bool s_parse_profile_declaration(
    const struct aws_byte_cursor *line_cursor,
    struct profile_file_parse_context *context) {

    /*
     * Strip comment and right-side whitespace
     */
    struct aws_byte_cursor profile_line_cursor = s_trim_trailing_comment(line_cursor);
    struct aws_byte_cursor profile_cursor = aws_byte_cursor_right_trim_pred(&profile_line_cursor, s_is_whitespace);

    /*
     * "[" + <whitespace>? + <"profile ">? + <profile name = identifier> + <whitespace>? + "]"
     */
    if (!s_parse_by_character_predicate(&profile_cursor, s_is_profile_start, NULL, 1)) {
        /*
         * This isn't a profile declaration, try something else
         */
        return false;
    }

    context->has_seen_profile = true;
    context->current_profile = NULL;
    context->current_property = NULL;

    s_parse_by_character_predicate(&profile_cursor, s_is_whitespace, NULL, 0);
    enum aws_profile_section_type section_type = AWS_PROFILE_SECTION_TYPE_PROFILE;

    /*
     * Check if the profile name starts with the 'profile' keyword.  We need to check for
     * "profile" and at least one whitespace character.  A partial match
     * ("[profilefoo]" for example) should rewind and use the whole name properly.
     */
    struct aws_byte_cursor backtrack_cursor = profile_cursor;
    bool has_profile_prefix = s_parse_by_token(&profile_cursor, s_profile_token, NULL) &&
                              s_parse_by_character_predicate(&profile_cursor, s_is_whitespace, NULL, 1);
    bool has_sso_session_prefix = !has_profile_prefix && s_parse_by_token(&profile_cursor, s_sso_session_token, NULL) &&
                                  s_parse_by_character_predicate(&profile_cursor, s_is_whitespace, NULL, 1);

    if (has_profile_prefix) {
        if (context->profile_collection->profile_source == AWS_PST_CREDENTIALS) {
            AWS_LOGF_WARN(
                AWS_LS_SDKUTILS_PROFILE,
                "Profile declarations in credentials files are not allowed to begin with the \"profile\" keyword");
            s_log_parse_context(AWS_LL_WARN, context);

            context->parse_error = AWS_ERROR_SDKUTILS_PARSE_RECOVERABLE;
            return true;
        }

        s_parse_by_character_predicate(&profile_cursor, s_is_whitespace, NULL, 0);
    } else if (has_sso_session_prefix) {
        if (context->profile_collection->profile_source == AWS_PST_CREDENTIALS) {
            AWS_LOGF_WARN(AWS_LS_SDKUTILS_PROFILE, "sso-session declarations in credentials files are not allowed");
            s_log_parse_context(AWS_LL_WARN, context);

            context->parse_error = AWS_ERROR_SDKUTILS_PARSE_RECOVERABLE;
            return true;
        }
        section_type = AWS_PROFILE_SECTION_TYPE_SSO_SESSION;
        s_parse_by_character_predicate(&profile_cursor, s_is_whitespace, NULL, 0);
    } else {
        profile_cursor = backtrack_cursor;
    }

    struct aws_byte_cursor profile_name;
    if (!s_parse_by_character_predicate(&profile_cursor, s_is_identifier, &profile_name, 0)) {
        AWS_LOGF_WARN(AWS_LS_SDKUTILS_PROFILE, "Profile declarations must contain a valid identifier for a name");
        s_log_parse_context(AWS_LL_WARN, context);

        context->parse_error = AWS_ERROR_SDKUTILS_PARSE_RECOVERABLE;
        return true;
    }

    if (context->profile_collection->profile_source == AWS_PST_CONFIG && !has_profile_prefix &&
        !s_is_default_profile_name(&profile_name) && !has_sso_session_prefix) {
        AWS_LOGF_WARN(
            AWS_LS_SDKUTILS_PROFILE,
            "Non-default profile declarations in config files must use the \"profile\" keyword");
        s_log_parse_context(AWS_LL_WARN, context);

        context->parse_error = AWS_ERROR_SDKUTILS_PARSE_RECOVERABLE;
        return true;
    }

    s_parse_by_character_predicate(&profile_cursor, s_is_whitespace, NULL, 0);

    /*
     * Special case the right side bracket check.  We need to distinguish between a missing right bracket
     * (fatal error) and invalid profile name (spaces, non-identifier characters).
     *
     * Do so by consuming all non right-bracket characters.  If the remainder is empty it is missing,
     * otherwise it is an invalid profile name (non-empty invalid_chars) or a good definition
     * (empty invalid_chars cursor).
     */
    struct aws_byte_cursor invalid_chars;
    s_parse_by_character_predicate(&profile_cursor, s_is_not_profile_end, &invalid_chars, 0);
    if (profile_cursor.len == 0) {
        AWS_LOGF_WARN(AWS_LS_SDKUTILS_PROFILE, "Profile declaration missing required ending bracket");
        s_log_parse_context(AWS_LL_WARN, context);

        context->parse_error = AWS_ERROR_SDKUTILS_PARSE_FATAL;
        return true;
    }

    if (invalid_chars.len > 0) {
        AWS_LOGF_WARN(
            AWS_LS_SDKUTILS_PROFILE,
            "Profile declaration contains invalid characters: \"" PRInSTR "\"",
            AWS_BYTE_CURSOR_PRI(invalid_chars));
        s_log_parse_context(AWS_LL_WARN, context);

        context->parse_error = AWS_ERROR_SDKUTILS_PARSE_RECOVERABLE;
        return true;
    }

    /*
     * Apply to the profile collection
     */
    if (s_profile_collection_add_profile(
            context->profile_collection,
            section_type,
            &profile_name,
            has_profile_prefix,
            context,
            &context->current_profile)) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_PROFILE, "Failed to add profile to profile collection");
        s_log_parse_context(AWS_LL_ERROR, context);

        context->parse_error = AWS_ERROR_SDKUTILS_PARSE_FATAL;
        return true;
    }

    return true;
}

/**
 * Attempts to parse property continuation lines
 *
 * Return false if this is not a property continuation line, true otherwise (stop parsing the line)
 */
static bool s_parse_property_continuation(
    const struct aws_byte_cursor *line_cursor,
    struct profile_file_parse_context *context) {

    /*
     * Strip right-side whitespace only.  Comments cannot be made on continuation lines.  They
     * get folded into the value.
     */
    struct aws_byte_cursor continuation_cursor = aws_byte_cursor_right_trim_pred(line_cursor, s_is_whitespace);

    /*
     * Can't be a continuation without at least one whitespace on the left
     */
    if (!s_parse_by_character_predicate(&continuation_cursor, s_is_whitespace, NULL, 0)) {
        return false;
    }

    /*
     * This should never happen since it should have been caught as a whitespace line
     */
    if (continuation_cursor.len == 0) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_PROFILE, "Property continuation internal parsing error");
        s_log_parse_context(AWS_LL_ERROR, context);

        context->parse_error = AWS_ERROR_SDKUTILS_PARSE_RECOVERABLE;
        return true;
    }

    /*
     * A continuation without a current property is bad
     */
    if (context->current_profile == NULL || context->current_property == NULL) {
        AWS_LOGF_ERROR(AWS_LS_SDKUTILS_PROFILE, "Property continuation seen outside of a current property");
        s_log_parse_context(AWS_LL_WARN, context);

        context->parse_error = AWS_ERROR_SDKUTILS_PARSE_FATAL;
        return true;
    }

    if (s_profile_property_add_continuation(context->current_property, &continuation_cursor)) {
        AWS_LOGF_DEBUG(AWS_LS_SDKUTILS_PROFILE, "Property continuation could not be applied to the current property");
        s_log_parse_context(AWS_LL_WARN, context);

        context->parse_error = AWS_ERROR_SDKUTILS_PARSE_RECOVERABLE;
        return true;
    }

    if (context->current_property->is_empty_valued) {

        struct aws_byte_cursor key_cursor;
        if (!s_parse_by_character_predicate(&continuation_cursor, s_is_not_assignment_operator, &key_cursor, 0)) {
            AWS_LOGF_WARN(
                AWS_LS_SDKUTILS_PROFILE, "Empty-valued property continuation must contain the assignment operator");
            s_log_parse_context(AWS_LL_WARN, context);

            context->parse_error = AWS_ERROR_SDKUTILS_PARSE_FATAL;
            return true;
        }

        if (!s_parse_by_character_predicate(&continuation_cursor, s_is_assignment_operator, NULL, 1)) {
            AWS_LOGF_WARN(
                AWS_LS_SDKUTILS_PROFILE, "Empty-valued property continuation must contain the assignment operator");
            s_log_parse_context(AWS_LL_WARN, context);

            context->parse_error = AWS_ERROR_SDKUTILS_PARSE_FATAL;
            return true;
        }

        struct aws_byte_cursor trimmed_key_cursor = aws_byte_cursor_right_trim_pred(&key_cursor, s_is_whitespace);
        struct aws_byte_cursor id_check_cursor = aws_byte_cursor_trim_pred(&trimmed_key_cursor, s_is_identifier);
        if (id_check_cursor.len > 0) {
            AWS_LOGF_WARN(
                AWS_LS_SDKUTILS_PROFILE,
                "Empty-valued property continuation must have a valid identifier to the left of the assignment");
            s_log_parse_context(AWS_LL_WARN, context);

            context->parse_error = AWS_ERROR_SDKUTILS_PARSE_RECOVERABLE;
            return true;
        }

        s_parse_by_character_predicate(&continuation_cursor, s_is_whitespace, NULL, 0);

        /*
         * everything left in the continuation_cursor is the sub property value
         */
        if (s_profile_property_add_sub_property(
                context->current_property, &trimmed_key_cursor, &continuation_cursor, context)) {
            AWS_LOGF_ERROR(AWS_LS_SDKUTILS_PROFILE, "Internal error adding sub property to current property");
            s_log_parse_context(AWS_LL_ERROR, context);

            context->parse_error = AWS_ERROR_SDKUTILS_PARSE_FATAL;
        }
    }

    return true;
}

/**
 * Attempts to parse property lines
 *
 * Return false if this is not a property line, true otherwise (stop parsing the line)
 */
static bool s_parse_property(const struct aws_byte_cursor *line_cursor, struct profile_file_parse_context *context) {

    /*
     * Strip whitespace-prefixed comment and right-side whitespace
     */
    struct aws_byte_cursor property_line_cursor = s_trim_trailing_whitespace_comment(line_cursor);
    struct aws_byte_cursor property_cursor = aws_byte_cursor_right_trim_pred(&property_line_cursor, s_is_whitespace);

    context->current_property = NULL;

    struct aws_byte_cursor key_cursor;
    if (!s_parse_by_character_predicate(&property_cursor, s_is_not_assignment_operator, &key_cursor, 0)) {
        AWS_LOGF_WARN(AWS_LS_SDKUTILS_PROFILE, "Property definition does not contain the assignment operator");
        s_log_parse_context(AWS_LL_WARN, context);

        context->parse_error = AWS_ERROR_SDKUTILS_PARSE_FATAL;
        return true;
    }

    struct aws_byte_cursor trimmed_key_cursor = aws_byte_cursor_right_trim_pred(&key_cursor, s_is_whitespace);
    struct aws_byte_cursor id_check_cursor = aws_byte_cursor_trim_pred(&trimmed_key_cursor, s_is_identifier);
    if (id_check_cursor.len > 0) {
        AWS_LOGF_WARN(AWS_LS_SDKUTILS_PROFILE, "Property definition does not begin with a valid identifier");
        s_log_parse_context(AWS_LL_WARN, context);

        context->parse_error = AWS_ERROR_SDKUTILS_PARSE_RECOVERABLE;
        return true;
    }

    if (!s_parse_by_character_predicate(&property_cursor, s_is_assignment_operator, NULL, 1)) {
        AWS_LOGF_WARN(AWS_LS_SDKUTILS_PROFILE, "Property definition does not contain the assignment operator");
        s_log_parse_context(AWS_LL_WARN, context);

        context->parse_error = AWS_ERROR_SDKUTILS_PARSE_FATAL;
        return true;
    }

    s_parse_by_character_predicate(&property_cursor, s_is_whitespace, NULL, 0);

    /*
     * If appropriate, apply to the profile collection, property_cursor contains the trimmed value, if one exists
     */
    if (context->current_profile != NULL) {
        context->current_property =
            s_profile_add_property(context->current_profile, &trimmed_key_cursor, &property_cursor);
        if (context->current_property == NULL) {
            AWS_LOGF_ERROR(
                AWS_LS_SDKUTILS_PROFILE,
                "Failed to add property \"" PRInSTR "\" to current profile \"%s\"",
                AWS_BYTE_CURSOR_PRI(trimmed_key_cursor),
                context->current_profile->name->bytes);
            s_log_parse_context(AWS_LL_ERROR, context);

            context->parse_error = AWS_ERROR_SDKUTILS_PARSE_FATAL;
        }
    } else {
        /*
         * By definition, if we haven't seen any profiles yet, this is a fatal error
         */
        if (context->has_seen_profile) {
            AWS_LOGF_WARN(AWS_LS_SDKUTILS_PROFILE, "Property definition seen outside a profile");
            s_log_parse_context(AWS_LL_WARN, context);

            context->parse_error = AWS_ERROR_SDKUTILS_PARSE_RECOVERABLE;
        } else {
            AWS_LOGF_WARN(AWS_LS_SDKUTILS_PROFILE, "Property definition seen before any profiles");
            s_log_parse_context(AWS_LL_WARN, context);

            context->parse_error = AWS_ERROR_SDKUTILS_PARSE_FATAL;
        }
    }

    return true;
}

static void s_parse_and_apply_line_to_profile_collection(
    struct profile_file_parse_context *context,
    const struct aws_byte_cursor *line_cursor) {

    /*
     * Ignore line feed on windows
     */
    struct aws_byte_cursor line = aws_byte_cursor_right_trim_pred(line_cursor, s_is_carriage_return);
    if (line.len == 0 || s_is_comment_line(&line) || s_is_whitespace_line(&line)) {
        return;
    }

    AWS_LOGF_TRACE(
        AWS_LS_SDKUTILS_PROFILE,
        "Parsing aws profile line in profile \"%s\", current property: \"%s\"",
        context->current_profile ? context->current_profile->name->bytes : s_none_string->bytes,
        context->current_property ? context->current_property->name->bytes : s_none_string->bytes);

    if (s_parse_profile_declaration(&line, context)) {
        return;
    }

    if (s_parse_property_continuation(&line, context)) {
        return;
    }

    if (s_parse_property(&line, context)) {
        return;
    }

    AWS_LOGF_ERROR(AWS_LS_SDKUTILS_PROFILE, "Unidentifiable line type encountered while parsing profile file");
    s_log_parse_context(AWS_LL_WARN, context);

    context->parse_error = AWS_ERROR_SDKUTILS_PARSE_FATAL;
}

static struct aws_profile_collection *s_aws_profile_collection_new_internal(
    struct aws_allocator *allocator,
    const struct aws_byte_buf *buffer,
    enum aws_profile_source_type source,
    const struct aws_string *path) {

    struct aws_profile_collection *profile_collection =
        (struct aws_profile_collection *)aws_mem_acquire(allocator, sizeof(struct aws_profile_collection));
    if (profile_collection == NULL) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*profile_collection);

    profile_collection->profile_source = source;
    profile_collection->allocator = allocator;

    aws_ref_count_init(
        &profile_collection->ref_count,
        profile_collection,
        (aws_simple_completion_callback *)s_aws_profile_collection_destroy_internal);

    for (int i = 0; i < AWS_PROFILE_SECTION_TYPE_COUNT; i++) {
        if (aws_hash_table_init(
                &profile_collection->sections[i],
                allocator,
                PROFILE_TABLE_DEFAULT_SIZE,
                aws_hash_string,
                aws_hash_callback_string_eq,
                NULL, /* The key is owned by the value (and destroy cleans it up), so we don't have to */
                s_profile_hash_table_value_destroy)) {
            goto cleanup;
        }
    }

    struct aws_byte_cursor current_position = aws_byte_cursor_from_buf(buffer);

    if (current_position.len > 0) {
        struct aws_byte_cursor line_cursor;
        AWS_ZERO_STRUCT(line_cursor);

        struct profile_file_parse_context context;
        AWS_ZERO_STRUCT(context);
        context.current_line_number = 1;
        context.profile_collection = profile_collection;
        context.source_file_path = path;

        while (aws_byte_cursor_next_split(&current_position, '\n', &line_cursor)) {
            context.current_line = line_cursor;

            s_parse_and_apply_line_to_profile_collection(&context, &line_cursor);
            if (context.parse_error == AWS_ERROR_SDKUTILS_PARSE_FATAL) {
                AWS_LOGF_WARN(AWS_LS_SDKUTILS_PROFILE, "Fatal error while parsing aws profile collection");
                goto cleanup;
            }

            aws_byte_cursor_advance(&current_position, line_cursor.len + 1);
            ++context.current_line_number;
        }
    }

    return profile_collection;

cleanup:
    s_aws_profile_collection_destroy_internal(profile_collection);

    return NULL;
}

struct aws_profile_collection *aws_profile_collection_acquire(struct aws_profile_collection *collection) {
    if (collection != NULL) {
        aws_ref_count_acquire(&collection->ref_count);
    }

    return collection;
}

struct aws_profile_collection *aws_profile_collection_release(struct aws_profile_collection *collection) {
    if (collection != NULL) {
        aws_ref_count_release(&collection->ref_count);
    }

    return NULL;
}

struct aws_profile_collection *aws_profile_collection_new_from_file(
    struct aws_allocator *allocator,
    const struct aws_string *file_path,
    enum aws_profile_source_type source) {

    struct aws_byte_buf file_contents;
    AWS_ZERO_STRUCT(file_contents);

    AWS_LOGF_DEBUG(AWS_LS_SDKUTILS_PROFILE, "Creating profile collection from file at \"%s\"", file_path->bytes);

    if (aws_byte_buf_init_from_file(&file_contents, allocator, aws_string_c_str(file_path)) != 0) {
        AWS_LOGF_DEBUG(AWS_LS_SDKUTILS_PROFILE, "Failed to read file at \"%s\"", file_path->bytes);
        return NULL;
    }

    struct aws_profile_collection *profile_collection =
        s_aws_profile_collection_new_internal(allocator, &file_contents, source, file_path);

    aws_byte_buf_clean_up(&file_contents);

    return profile_collection;
}

struct aws_profile_collection *aws_profile_collection_new_from_buffer(
    struct aws_allocator *allocator,
    const struct aws_byte_buf *buffer,
    enum aws_profile_source_type source) {

    return s_aws_profile_collection_new_internal(allocator, buffer, source, NULL);
}

static struct aws_string *s_process_profile_file_path(struct aws_allocator *allocator, const struct aws_string *path) {
    struct aws_string *final_path = NULL;

    /*
     * Make a copy to mess with
     */
    struct aws_string *path_copy = aws_string_new_from_string(allocator, path);
    if (path_copy == NULL) {
        return NULL;
    }

    struct aws_string *home_directory = NULL;

    /*
     * Fake directory cursor for final directory construction
     */
    char local_platform_separator = aws_get_platform_directory_separator();
    struct aws_byte_cursor separator_cursor;
    AWS_ZERO_STRUCT(separator_cursor);
    separator_cursor.ptr = (uint8_t *)&local_platform_separator;
    separator_cursor.len = 1;

    for (size_t i = 0; i < path_copy->len; ++i) {
        char value = path_copy->bytes[i];
        if (aws_is_any_directory_separator(value)) {
            ((char *)(path_copy->bytes))[i] = local_platform_separator;
        }
    }

    /*
     * Process a split on the local separator, which we now know is the only one present in the string.
     *
     * While this does not conform fully to the SEP governing profile file path resolution, it covers
     * a useful, cross-platform subset of functionality that the full implementation will be backwards compatible with.
     */
    struct aws_array_list path_segments;
    if (aws_array_list_init_dynamic(&path_segments, allocator, 10, sizeof(struct aws_byte_cursor))) {
        goto on_array_list_init_failure;
    }

    struct aws_byte_cursor path_cursor = aws_byte_cursor_from_string(path_copy);
    if (aws_byte_cursor_split_on_char(&path_cursor, local_platform_separator, &path_segments)) {
        goto on_split_failure;
    }

    size_t final_string_length = 0;
    size_t path_segment_count = aws_array_list_length(&path_segments);
    for (size_t i = 0; i < path_segment_count; ++i) {
        struct aws_byte_cursor segment_cursor;
        AWS_ZERO_STRUCT(segment_cursor);

        if (aws_array_list_get_at(&path_segments, &segment_cursor, i)) {
            continue;
        }

        /*
         * Current support: if and only if the first segment is just '~' then replace it
         * with the current home directory based on SEP home directory resolution rules.
         *
         * Support for (pathological but proper) paths with embedded ~ ("../../~/etc...") and
         * cross-user ~ ("~someone/.aws/credentials") can come later.  As it stands, they will
         * potentially succeed on unix platforms but not Windows.
         */
        if (i == 0 && segment_cursor.len == 1 && *segment_cursor.ptr == '~') {
            if (home_directory == NULL) {
                home_directory = aws_get_home_directory(allocator);

                if (AWS_UNLIKELY(!home_directory)) {
                    goto on_empty_path;
                }
            }

            final_string_length += home_directory->len;
        } else {
            final_string_length += segment_cursor.len;
        }
    }

    if (path_segment_count > 1) {
        final_string_length += path_segment_count - 1;
    }

    if (final_string_length == 0) {
        goto on_empty_path;
    }

    /*
     * Build the final path from the split + a possible home directory resolution
     */
    struct aws_byte_buf result;
    aws_byte_buf_init(&result, allocator, final_string_length);
    for (size_t i = 0; i < path_segment_count; ++i) {
        struct aws_byte_cursor segment_cursor;
        AWS_ZERO_STRUCT(segment_cursor);

        if (aws_array_list_get_at(&path_segments, &segment_cursor, i)) {
            continue;
        }

        /*
         * See above for explanation
         */
        if (i == 0 && segment_cursor.len == 1 && *segment_cursor.ptr == '~') {
            if (home_directory == NULL) {
                goto on_home_directory_failure;
            }
            struct aws_byte_cursor home_cursor = aws_byte_cursor_from_string(home_directory);
            if (aws_byte_buf_append(&result, &home_cursor)) {
                goto on_byte_buf_write_failure;
            }
        } else {
            if (aws_byte_buf_append(&result, &segment_cursor)) {
                goto on_byte_buf_write_failure;
            }
        }

        /*
         * Add the separator after all but the last segment
         */
        if (i + 1 < path_segment_count) {
            if (aws_byte_buf_append(&result, &separator_cursor)) {
                goto on_byte_buf_write_failure;
            }
        }
    }

    final_path = aws_string_new_from_array(allocator, result.buffer, result.len);

/*
 * clean up
 */
on_byte_buf_write_failure:
    aws_byte_buf_clean_up(&result);

on_empty_path:
on_home_directory_failure:
on_split_failure:
    aws_array_list_clean_up(&path_segments);

on_array_list_init_failure:
    aws_string_destroy(path_copy);

    if (home_directory != NULL) {
        aws_string_destroy(home_directory);
    }

    return final_path;
}

AWS_STATIC_STRING_FROM_LITERAL(s_default_credentials_path, "~/.aws/credentials");
AWS_STATIC_STRING_FROM_LITERAL(s_credentials_file_path_env_variable_name, "AWS_SHARED_CREDENTIALS_FILE");

AWS_STATIC_STRING_FROM_LITERAL(s_default_config_path, "~/.aws/config");
AWS_STATIC_STRING_FROM_LITERAL(s_config_file_path_env_variable_name, "AWS_CONFIG_FILE");

static struct aws_string *s_get_raw_file_path(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *override_path,
    const struct aws_string *override_env_var_name,
    const struct aws_string *default_path) {

    if (override_path != NULL && override_path->ptr != NULL) {
        return aws_string_new_from_array(allocator, override_path->ptr, override_path->len);
    }

    struct aws_string *env_override_path = NULL;
    if (aws_get_environment_value(allocator, override_env_var_name, &env_override_path) == 0 &&
        env_override_path != NULL) {
        return env_override_path;
    }

    return aws_string_new_from_string(allocator, default_path);
}

struct aws_string *aws_get_credentials_file_path(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *override_path) {

    struct aws_string *raw_path = s_get_raw_file_path(
        allocator, override_path, s_credentials_file_path_env_variable_name, s_default_credentials_path);

    struct aws_string *final_path = s_process_profile_file_path(allocator, raw_path);

    aws_string_destroy(raw_path);

    return final_path;
}

struct aws_string *aws_get_config_file_path(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *override_path) {

    struct aws_string *raw_path =
        s_get_raw_file_path(allocator, override_path, s_config_file_path_env_variable_name, s_default_config_path);

    struct aws_string *final_path = s_process_profile_file_path(allocator, raw_path);

    aws_string_destroy(raw_path);

    return final_path;
}

AWS_STATIC_STRING_FROM_LITERAL(s_default_profile_env_variable_name, "AWS_PROFILE");

struct aws_string *aws_get_profile_name(struct aws_allocator *allocator, const struct aws_byte_cursor *override_name) {
    /**
     * Profile name is resolved in the following order.
     * 1. If the override_path variable is provided.
     * 2. Check `AWS_PROFILE` environment variable and use the value if it is not empty.
     * 3. Use "default". */
    struct aws_string *profile_name = NULL;
    if (override_name != NULL && override_name->ptr != NULL) {
        profile_name = aws_string_new_from_array(allocator, override_name->ptr, override_name->len);
    } else {
        /* Try to fetch profile from AWS_PROFILE environment variable */
        aws_get_environment_value(allocator, s_default_profile_env_variable_name, &profile_name);
        /* Use default profile if it doesn't exist. */
        if (profile_name == NULL) {
            profile_name = aws_string_new_from_string(allocator, s_default_profile_name);
        }
    }

    return profile_name;
}

size_t aws_profile_get_property_count(const struct aws_profile *profile) {
    return aws_hash_table_get_entry_count(&profile->properties);
}

size_t aws_profile_collection_get_profile_count(const struct aws_profile_collection *profile_collection) {
    return aws_hash_table_get_entry_count(&profile_collection->sections[AWS_PROFILE_SECTION_TYPE_PROFILE]);
}

size_t aws_profile_collection_get_section_count(
    const struct aws_profile_collection *profile_collection,
    const enum aws_profile_section_type section_type) {
    return aws_hash_table_get_entry_count(&profile_collection->sections[section_type]);
}

size_t aws_profile_property_get_sub_property_count(const struct aws_profile_property *property) {
    return aws_hash_table_get_entry_count(&property->sub_properties);
}

const struct aws_string *aws_profile_property_get_sub_property(
    const struct aws_profile_property *property,
    const struct aws_string *sub_property_name) {
    struct aws_hash_element *element = NULL;

    if (aws_hash_table_find(&property->sub_properties, sub_property_name, &element) || element == NULL) {
        return NULL;
    }

    return (const struct aws_string *)element->value;
}

const struct aws_string *aws_profile_get_name(const struct aws_profile *profile) {
    AWS_PRECONDITION(profile);
    return profile->name;
}
