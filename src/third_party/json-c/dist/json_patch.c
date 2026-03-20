/*
 * Copyright (c) 2021 Alexandru Ardelean.
 * Copyright (c) 2023 Eric Hawicz
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See COPYING for details.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "json_patch.h"
#include "json_object_private.h"
#include "json_pointer_private.h"

#include <limits.h>
#ifndef SIZE_T_MAX
#if SIZEOF_SIZE_T == SIZEOF_INT
#define SIZE_T_MAX UINT_MAX
#elif SIZEOF_SIZE_T == SIZEOF_LONG
#define SIZE_T_MAX ULONG_MAX
#elif SIZEOF_SIZE_T == SIZEOF_LONG_LONG
#define SIZE_T_MAX ULLONG_MAX
#else
#error Unable to determine size of size_t
#endif
#endif

#define _set_err(_errval, _errmsg) do { \
	patch_error->errno_code = (_errval); \
	patch_error->errmsg = (_errmsg); \
	errno = 0;  /* To avoid confusion */ \
} while (0)

#define _set_err_from_ptrget(_errval, _fieldname) do { \
	patch_error->errno_code = (_errval); \
	patch_error->errmsg = (_errval) == ENOENT ? \
		"Did not find element referenced by " _fieldname " field" : \
		"Invalid " _fieldname " field"; \
	errno = 0;  /* To avoid confusion */ \
} while(0)

/**
 * JavaScript Object Notation (JSON) Patch
 *   RFC 6902 - https://tools.ietf.org/html/rfc6902
 */

static int json_patch_apply_test(struct json_object **res,
                                 struct json_object *patch_elem,
                                 const char *path, struct json_patch_error *patch_error)
{
	struct json_object *value1, *value2;

	if (!json_object_object_get_ex(patch_elem, "value", &value1)) {
		_set_err(EINVAL, "Patch object does not contain a 'value' field");
		return -1;
	}

	if (json_pointer_get(*res, path, &value2))
	{
		_set_err_from_ptrget(errno, "path");
		return -1;
	}

	if (!json_object_equal(value1, value2)) {
		_set_err(ENOENT, "Value of element referenced by 'path' field did not match 'value' field");
		return -1;
	}

	return 0;
}

static int __json_patch_apply_remove(struct json_pointer_get_result *jpres)
{
	if (json_object_is_type(jpres->parent, json_type_array)) {
		return json_object_array_del_idx(jpres->parent, jpres->index_in_parent, 1);
	} else if (jpres->parent && jpres->key_in_parent) {
		json_object_object_del(jpres->parent, jpres->key_in_parent);
		return 0;
	} else {
		// We're removing the root object
		(void)json_object_put(jpres->obj);
		jpres->obj = NULL;
		return 0;
	}
}

static int json_patch_apply_remove(struct json_object **res, const char *path, struct json_patch_error *patch_error)
{
	struct json_pointer_get_result jpres;
	int rc;

	if (json_pointer_get_internal(*res, path, &jpres))
	{
		_set_err_from_ptrget(errno, "path");
		return -1;
	}

	rc = __json_patch_apply_remove(&jpres);
	if (rc < 0)
		_set_err(EINVAL, "Unable to remove path referenced by 'path' field");
	// This means we removed and freed the root object, i.e. *res
	if (jpres.parent == NULL)
		*res = NULL;
	return rc;
}

// callback for json_pointer_set_with_array_cb()
static int json_object_array_insert_idx_cb(struct json_object *parent, size_t idx,
                                           struct json_object *value, void *priv)
{
	int rc;
	int *add = priv;

	if (idx > json_object_array_length(parent))
	{
		// Note: will propagate back out through json_pointer_set_with_array_cb()
		errno = EINVAL;
		return -1;
	}

	if (*add)
		rc = json_object_array_insert_idx(parent, idx, value);
	else
		rc = json_object_array_put_idx(parent, idx, value);
	if (rc < 0)
		errno = EINVAL;
	return rc;
}

static int json_patch_apply_add_replace(struct json_object **res,
                                        struct json_object *patch_elem,
                                        const char *path, int add, struct json_patch_error *patch_error)
{
	struct json_object *value;
	int rc;

	if (!json_object_object_get_ex(patch_elem, "value", &value)) {
		_set_err(EINVAL, "Patch object does not contain a 'value' field");
		return -1;
	}
	/* if this is a replace op, then we need to make sure it exists before replacing */
	if (!add && json_pointer_get(*res, path, NULL)) {
		_set_err_from_ptrget(errno, "path");
		return -1;
	}

	rc = json_pointer_set_with_array_cb(res, path, json_object_get(value),
					    json_object_array_insert_idx_cb, &add);
	if (rc)
	{
		_set_err(errno, "Failed to set value at path referenced by 'path' field");
		json_object_put(value);
	}

	return rc;
}

// callback for json_pointer_set_with_array_cb()
static int json_object_array_move_cb(struct json_object *parent, size_t idx,
                                     struct json_object *value, void *priv)
{
	int rc;
	struct json_pointer_get_result *from = priv;
	size_t len = json_object_array_length(parent);

	/**
	 * If it's the same array parent, it means that we removed
	 * and element from it, so the length is temporarily reduced
	 * by 1, which means that if we try to move an element to
	 * the last position, we need to check the current length + 1
	 */
	if (parent == from->parent)
		len++;

	if (idx > len)
	{
		// Note: will propagate back out through json_pointer_set_with_array_cb()
		errno = EINVAL;
		return -1;
	}

	rc = json_object_array_insert_idx(parent, idx, value);
	if (rc < 0)
		errno = EINVAL;
	return rc;
}

static int json_patch_apply_move_copy(struct json_object **res,
                                      struct json_object *patch_elem,
                                      const char *path, int move, struct json_patch_error *patch_error)
{
	json_pointer_array_set_cb array_set_cb;
	struct json_pointer_get_result from;
	struct json_object *jfrom;
	const char *from_s;
	size_t from_s_len;
	int rc;

	if (!json_object_object_get_ex(patch_elem, "from", &jfrom)) {
		_set_err(EINVAL, "Patch does not contain a 'from' field");
		return -1;
	}

	from_s = json_object_get_string(jfrom);

	from_s_len = strlen(from_s);
	if (strncmp(from_s, path, from_s_len) == 0) {
		/**
		 * If lengths match, it's a noop, if they don't,
		 * then we're trying to move a parent under a child
		 * which is not allowed as per RFC 6902 section 4.4
		 *   The "from" location MUST NOT be a proper prefix of the "path"
		 *   location; i.e., a location cannot be moved into one of its children.
		 */
		if (from_s_len == strlen(path))
			return 0;
		_set_err(EINVAL, "Invalid attempt to move parent under a child");
		return -1;
	}

	rc = json_pointer_get_internal(*res, from_s, &from);
	if (rc)
	{
		_set_err_from_ptrget(errno, "from");
		return rc;
	}

	// Note: it's impossible for json_pointer to find the root obj, due
	// to the path check above, so from.parent is guaranteed non-NULL
	json_object_get(from.obj);

	if (!move) {
		array_set_cb = json_object_array_insert_idx_cb;
	} else {
		rc = __json_patch_apply_remove(&from);
		if (rc < 0) {
			json_object_put(from.obj);
			return rc;
		}
		array_set_cb = json_object_array_move_cb;
	}

	rc = json_pointer_set_with_array_cb(res, path, from.obj, array_set_cb, &from);
	if (rc)
	{
		_set_err(errno, "Failed to set value at path referenced by 'path' field");
		json_object_put(from.obj);
	}

	return rc;
}

int json_patch_apply(struct json_object *copy_from, struct json_object *patch,
                     struct json_object **base, struct json_patch_error *patch_error)
{
	size_t ii;
	int rc = 0;
	struct json_patch_error placeholder;

	if (!patch_error)
		patch_error = &placeholder;

	patch_error->patch_failure_idx = SIZE_T_MAX;
	patch_error->errno_code = 0;

	if (base == NULL|| 
	    (*base == NULL && copy_from == NULL) ||
	    (*base != NULL && copy_from != NULL))
	{
		_set_err(EFAULT, "Exactly one of *base or copy_from must be non-NULL");
		return -1;
	}
	    
	if (!json_object_is_type(patch, json_type_array)) {
		_set_err(EFAULT, "Patch object is not of type json_type_array");
		return -1;
	}

	if (copy_from != NULL)
	{
		if (json_object_deep_copy(copy_from, base, NULL) < 0)
		{
			_set_err(ENOMEM, "Unable to copy copy_from using json_object_deep_copy()");
			return -1;
		}
	}

	/* Go through all operations ; apply them on res */
	for (ii = 0; ii < json_object_array_length(patch); ii++) {
		struct json_object *jop, *jpath;
		struct json_object *patch_elem = json_object_array_get_idx(patch, ii);
		const char *op, *path;

		patch_error->patch_failure_idx = ii;

		if (!json_object_object_get_ex(patch_elem, "op", &jop)) {
			_set_err(EINVAL, "Patch object does not contain 'op' field");
			return -1;
		}
		op = json_object_get_string(jop);
		if (!json_object_object_get_ex(patch_elem, "path", &jpath)) {
			_set_err(EINVAL, "Patch object does not contain 'path' field");
			return -1;
		}
		path = json_object_get_string(jpath); // Note: empty string is ok!

		if (!strcmp(op, "test"))
			rc = json_patch_apply_test(base, patch_elem, path, patch_error);
		else if (!strcmp(op, "remove"))
			rc = json_patch_apply_remove(base, path, patch_error);
		else if (!strcmp(op, "add"))
			rc = json_patch_apply_add_replace(base, patch_elem, path, 1, patch_error);
		else if (!strcmp(op, "replace"))
			rc = json_patch_apply_add_replace(base, patch_elem, path, 0, patch_error);
		else if (!strcmp(op, "move"))
			rc = json_patch_apply_move_copy(base, patch_elem, path, 1, patch_error);
		else if (!strcmp(op, "copy"))
			rc = json_patch_apply_move_copy(base, patch_elem, path, 0, patch_error);
		else {
			_set_err(EINVAL, "Patch object has invalid 'op' field");
			return -1;
		}
		if (rc < 0)
			break;
	}

	return rc;
}
