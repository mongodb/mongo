/*
 * Copyright (c) 2021 Alexadru Ardelean.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See COPYING for details.
 *
 */

/**
 * @file
 * @brief JSON Patch (RFC 6902) implementation for manipulating JSON objects
 */
#ifndef _json_patch_h_
#define _json_patch_h_

#include "json_pointer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Details of an error that occurred during json_patch_apply()
 */
struct json_patch_error {
	/**
	 * An errno value indicating what kind of error occurred.
	 * Possible values include:
	 * - ENOENT - A path referenced in the operation does not exist.
	 * - EINVAL - An invalid operation or with invalid path was attempted
	 * - ENOMEM - Unable to allocate memory
	 * - EFAULT - Invalid arguments were passed to json_patch_apply()
	 *             (i.e. a C API error, vs. a data error like EINVAL)
	 */
	int errno_code;

	/**
	 * The index into the patch array of the operation that failed,
	 * or SIZE_T_MAX for overall errors.
	 */
	size_t patch_failure_idx;

	/**
	 * A human readable error message.
	 * Allocated from static storage, does not need to be freed.
	 */
	const char *errmsg;
};

/**
 * Apply the JSON patch to the base object.
 * The patch object must be formatted as per RFC 6902, i.e.
 * a json_type_array containing patch operations.
 * If the patch is not correctly formatted, an error will
 * be returned.
 *
 * The json_object at *base will be modified in place.
 * Exactly one of *base or copy_from must be non-NULL.
 * If *base is NULL, a new copy of copy_from will allocated and populated
 * using json_object_deep_copy().  In this case json_object_put() _must_ be 
 * used to free *base even if the overall patching operation fails.
 *
 * If anything fails during patching a negative value will be returned,
 * and patch_error (if non-NULL) will be populated with error details.
 *
 * @param base a pointer to the JSON object which to patch
 * @param patch the JSON object that describes the patch to be applied
 * @param copy_from a JSON object to copy to *base
 * @param patch_error optional, details about errors
 *
 * @return negative if an error (or not found), or 0 if patch completely applied
 */
JSON_EXPORT int json_patch_apply(struct json_object *copy_from, struct json_object *patch,
                                 struct json_object **base, struct json_patch_error *patch_error);

#ifdef __cplusplus
}
#endif

#endif
