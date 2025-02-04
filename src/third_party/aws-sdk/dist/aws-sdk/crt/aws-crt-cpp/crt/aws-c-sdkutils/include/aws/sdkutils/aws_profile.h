/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_SDKUTILS_AWS_PROFILE_H
#define AWS_SDKUTILS_AWS_PROFILE_H
#include <aws/sdkutils/sdkutils.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_allocator;
struct aws_string;
struct aws_byte_buf;
struct aws_byte_cursor;

/*
 * A set of data types that model the aws profile specification
 *
 * A profile collection is a collection of zero or more named profiles
 * Each profile is a set of properties (named key-value pairs)
 * Empty-valued properties may have sub properties (named key-value pairs)
 *
 * Resolution rules exist to determine what profile to use, what files to
 * read profile collections from, and what types of credentials have priority.
 *
 * The profile specification is informally defined as "what the aws cli does" and
 * formally defined in internal aws documents.
 */
struct aws_profile_property;
struct aws_profile;
struct aws_profile_collection;

/**
 * The profile specification has rule exceptions based on what file
 * the profile collection comes from.
 */
enum aws_profile_source_type { AWS_PST_NONE, AWS_PST_CONFIG, AWS_PST_CREDENTIALS };

/*
 * The collection can hold different types of sections.
 */
enum aws_profile_section_type {
    AWS_PROFILE_SECTION_TYPE_PROFILE,
    AWS_PROFILE_SECTION_TYPE_SSO_SESSION,

    AWS_PROFILE_SECTION_TYPE_COUNT,
};

AWS_EXTERN_C_BEGIN

/*************************
 * Profile collection APIs
 *************************/

/**
 * Increments the reference count on the profile collection, allowing the caller to take a reference to it.
 *
 * Returns the same profile collection passed in.
 */
AWS_SDKUTILS_API
struct aws_profile_collection *aws_profile_collection_acquire(struct aws_profile_collection *collection);

/**
 * Decrements a profile collection's ref count.  When the ref count drops to zero, the collection will be destroyed.
 * Returns NULL.
 */
AWS_SDKUTILS_API
struct aws_profile_collection *aws_profile_collection_release(struct aws_profile_collection *collection);

/**
 * @Deprecated This is equivalent to aws_profile_collection_release.
 */
AWS_SDKUTILS_API
void aws_profile_collection_destroy(struct aws_profile_collection *profile_collection);

/**
 * Create a new profile collection by parsing a file with the specified path
 */
AWS_SDKUTILS_API
struct aws_profile_collection *aws_profile_collection_new_from_file(
    struct aws_allocator *allocator,
    const struct aws_string *file_path,
    enum aws_profile_source_type source);

/**
 * Create a new profile collection by merging a config-file-based profile
 * collection and a credentials-file-based profile collection
 */
AWS_SDKUTILS_API
struct aws_profile_collection *aws_profile_collection_new_from_merge(
    struct aws_allocator *allocator,
    const struct aws_profile_collection *config_profiles,
    const struct aws_profile_collection *credentials_profiles);

/**
 * Create a new profile collection by parsing text in a buffer.  Primarily
 * for testing.
 */
AWS_SDKUTILS_API
struct aws_profile_collection *aws_profile_collection_new_from_buffer(
    struct aws_allocator *allocator,
    const struct aws_byte_buf *buffer,
    enum aws_profile_source_type source);

/**
 * Retrieves a reference to a profile with the specified name, if it exists, from the profile collection
 */
AWS_SDKUTILS_API
const struct aws_profile *aws_profile_collection_get_profile(
    const struct aws_profile_collection *profile_collection,
    const struct aws_string *profile_name);

/*
 * Retrieves a reference to a section with the specified name and type, if it exists, from the profile collection.
 * You can get the "default" profile or credentials file sections by passing `AWS_PROFILE_SECTION_TYPE_PROFILE`
 */
AWS_SDKUTILS_API
const struct aws_profile *aws_profile_collection_get_section(
    const struct aws_profile_collection *profile_collection,
    const enum aws_profile_section_type section_type,
    const struct aws_string *section_name);

/**
 * Returns the number of profiles in a collection
 */
AWS_SDKUTILS_API
size_t aws_profile_collection_get_profile_count(const struct aws_profile_collection *profile_collection);

/**
 * Returns the number of elements of the specified section in a collection.
 */
AWS_SDKUTILS_API
size_t aws_profile_collection_get_section_count(
    const struct aws_profile_collection *profile_collection,
    const enum aws_profile_section_type section_type);

/**
 * Returns a reference to the name of the provided profile
 */
AWS_SDKUTILS_API
const struct aws_string *aws_profile_get_name(const struct aws_profile *profile);

/**************
 * profile APIs
 **************/

/**
 * Retrieves a reference to a property with the specified name, if it exists, from a profile
 */
AWS_SDKUTILS_API
const struct aws_profile_property *aws_profile_get_property(
    const struct aws_profile *profile,
    const struct aws_string *property_name);

/**
 * Returns how many properties a profile holds
 */
AWS_SDKUTILS_API
size_t aws_profile_get_property_count(const struct aws_profile *profile);

/**
 * Returns a reference to the property's string value
 */
AWS_SDKUTILS_API
const struct aws_string *aws_profile_property_get_value(const struct aws_profile_property *property);

/***********************
 * profile property APIs
 ***********************/

/**
 * Returns a reference to the value of a sub property with the given name, if it exists, in the property
 */
AWS_SDKUTILS_API
const struct aws_string *aws_profile_property_get_sub_property(
    const struct aws_profile_property *property,
    const struct aws_string *sub_property_name);

/**
 * Returns how many sub properties the property holds
 */
AWS_SDKUTILS_API
size_t aws_profile_property_get_sub_property_count(const struct aws_profile_property *property);

/***********
 * Misc APIs
 ***********/

/**
 * Computes the final platform-specific path for the profile credentials file.  Does limited home directory
 * expansion/resolution.
 *
 * override_path, if not null, will be searched first instead of using the standard home directory config path
 */
AWS_SDKUTILS_API
struct aws_string *aws_get_credentials_file_path(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *override_path);

/**
 * Computes the final platform-specific path for the profile config file.  Does limited home directory
 * expansion/resolution.
 *
 * override_path, if not null, will be searched first instead of using the standard home directory config path
 */
AWS_SDKUTILS_API
struct aws_string *aws_get_config_file_path(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *override_path);

/**
 * Computes the profile to use for credentials lookups based on profile resolution rules
 */
AWS_SDKUTILS_API
struct aws_string *aws_get_profile_name(struct aws_allocator *allocator, const struct aws_byte_cursor *override_name);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_SDKUTILS_AWS_PROFILE_H */
