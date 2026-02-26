/*
 * Copyright (c) 2016 Alexandru Ardelean.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See COPYING for details.
 *
 */

#include "config.h"

#include "strerror_override.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_object_private.h"
#include "json_pointer.h"
#include "json_pointer_private.h"
#include "strdup_compat.h"
#include "vasprintf_compat.h"

/* Avoid ctype.h and locale overhead */
#define is_plain_digit(c) ((c) >= '0' && (c) <= '9')

/**
 * JavaScript Object Notation (JSON) Pointer
 *   RFC 6901 - https://tools.ietf.org/html/rfc6901
 */

static void string_replace_all_occurrences_with_char(char *s, const char *occur, char repl_char)
{
	size_t slen = strlen(s);
	size_t skip = strlen(occur) - 1; /* length of the occurrence, minus the char we're replacing */
	char *p = s;
	while ((p = strstr(p, occur)))
	{
		*p = repl_char;
		p++;
		slen -= skip;
		memmove(p, (p + skip), slen - (p - s) + 1); /* includes null char too */
	}
}

static int is_valid_index(const char *path, size_t *idx)
{
	size_t i, len = strlen(path);
	/* this code-path optimizes a bit, for when we reference the 0-9 index range
	 * in a JSON array and because leading zeros not allowed
	 */
	if (len == 1)
	{
		if (is_plain_digit(path[0]))
		{
			*idx = (path[0] - '0');
			return 1;
		}
		errno = EINVAL;
		return 0;
	}
	/* leading zeros not allowed per RFC */
	if (path[0] == '0')
	{
		errno = EINVAL;
		return 0;
	}
	/* RFC states base-10 decimals */
	for (i = 0; i < len; i++)
	{
		if (!is_plain_digit(path[i]))
		{
			errno = EINVAL;
			return 0;
		}
	}

	// We know it's all digits, so the only error case here is overflow,
	// but ULLONG_MAX will be longer than any array length so that's ok.
	*idx = strtoull(path, NULL, 10);

	return 1;
}

static int json_pointer_get_single_path(struct json_object *obj, char *path,
                                        struct json_object **value, size_t *idx)
{
	if (json_object_is_type(obj, json_type_array))
	{
		if (!is_valid_index(path, idx))
			return -1;
		if (*idx >= json_object_array_length(obj))
		{
			errno = ENOENT;
			return -1;
		}

		obj = json_object_array_get_idx(obj, *idx);
		if (obj)
		{
			if (value)
				*value = obj;
			return 0;
		}
		/* Entry not found */
		errno = ENOENT;
		return -1;
	}

	/* RFC states that we first must eval all ~1 then all ~0 */
	string_replace_all_occurrences_with_char(path, "~1", '/');
	string_replace_all_occurrences_with_char(path, "~0", '~');

	if (!json_object_object_get_ex(obj, path, value))
	{
		errno = ENOENT;
		return -1;
	}

	return 0;
}

static int json_object_array_put_idx_cb(struct json_object *parent, size_t idx,
					struct json_object *value, void *priv)
{
	return json_object_array_put_idx(parent, idx, value);
}

static int json_pointer_set_single_path(struct json_object *parent, const char *path,
                                        struct json_object *value,
					json_pointer_array_set_cb array_set_cb, void *priv)
{
	if (json_object_is_type(parent, json_type_array))
	{
		size_t idx;
		/* RFC (Chapter 4) states that '-' may be used to add new elements to an array */
		if (path[0] == '-' && path[1] == '\0')
			return json_object_array_add(parent, value);
		if (!is_valid_index(path, &idx))
			return -1;
		return array_set_cb(parent, idx, value, priv);
	}

	/* path replacements should have been done in json_pointer_get_single_path(),
	 * and we should still be good here
	 */
	if (json_object_is_type(parent, json_type_object))
		return json_object_object_add(parent, path, value);

	/* Getting here means that we tried to "dereference" a primitive JSON type
	 * (like string, int, bool).i.e. add a sub-object to it
	 */
	errno = ENOENT;
	return -1;
}

static int json_pointer_result_get_recursive(struct json_object *obj, char *path,
                                             struct json_pointer_get_result *res)
{
	struct json_object *parent_obj = obj;
	size_t idx;
	char *endp;
	int rc;

	/* All paths (on each recursion level must have a leading '/' */
	if (path[0] != '/')
	{
		errno = EINVAL;
		return -1;
	}
	path++;

	endp = strchr(path, '/');
	if (endp)
		*endp = '\0';

	/* If we err-ed here, return here */
	if ((rc = json_pointer_get_single_path(obj, path, &obj, &idx)))
		return rc;

	if (endp)
	{
		/* Put the slash back, so that the sanity check passes on next recursion level */
		*endp = '/';
		return json_pointer_result_get_recursive(obj, endp, res);
	}

	/* We should be at the end of the recursion here */
	if (res) {
		res->parent = parent_obj;
		res->obj = obj;
		if (json_object_is_type(res->parent, json_type_array))
			res->index_in_parent = idx;
		else
			res->key_in_parent = path;
	}

	return 0;
}

static int json_pointer_object_get_recursive(struct json_object *obj, char *path,
                                             struct json_object **value)
{
	struct json_pointer_get_result res;
	int rc;

	rc = json_pointer_result_get_recursive(obj, path, &res);
	if (rc)
		return rc;

	if (value)
		*value = res.obj;

	return 0;
}

int json_pointer_get_internal(struct json_object *obj, const char *path,
                              struct json_pointer_get_result *res)
{
	char *path_copy = NULL;
	int rc;

	if (!obj || !path)
	{
		errno = EINVAL;
		return -1;
	}

	if (path[0] == '\0')
	{
		res->parent = NULL;
		res->obj = obj;
		res->key_in_parent = NULL;
		res->index_in_parent = -1;
		return 0;
	}

	/* pass a working copy to the recursive call */
	if (!(path_copy = strdup(path)))
	{
		errno = ENOMEM;
		return -1;
	}
	rc = json_pointer_result_get_recursive(obj, path_copy, res);
	/* re-map the path string to the const-path string */
	if (rc == 0 && json_object_is_type(res->parent, json_type_object) && res->key_in_parent)
		res->key_in_parent = path + (res->key_in_parent - path_copy);
	free(path_copy);

	return rc;
}

int json_pointer_get(struct json_object *obj, const char *path, struct json_object **res)
{
	struct json_pointer_get_result jpres;
	int rc;

	rc = json_pointer_get_internal(obj, path, &jpres);
	if (rc)
		return rc;

	if (res)
		*res = jpres.obj;

	return 0;
}

int json_pointer_getf(struct json_object *obj, struct json_object **res, const char *path_fmt, ...)
{
	char *path_copy = NULL;
	int rc = 0;
	va_list args;

	if (!obj || !path_fmt)
	{
		errno = EINVAL;
		return -1;
	}

	va_start(args, path_fmt);
	rc = vasprintf(&path_copy, path_fmt, args);
	va_end(args);

	if (rc < 0)
		return rc;

	if (path_copy[0] == '\0')
	{
		if (res)
			*res = obj;
		goto out;
	}

	rc = json_pointer_object_get_recursive(obj, path_copy, res);
out:
	free(path_copy);

	return rc;
}

int json_pointer_set_with_array_cb(struct json_object **obj, const char *path,
				   struct json_object *value,
				   json_pointer_array_set_cb array_set_cb, void *priv)
{
	const char *endp;
	char *path_copy = NULL;
	struct json_object *set = NULL;
	int rc;

	if (!obj || !path)
	{
		errno = EINVAL;
		return -1;
	}

	if (path[0] == '\0')
	{
		json_object_put(*obj);
		*obj = value;
		return 0;
	}

	if (path[0] != '/')
	{
		errno = EINVAL;
		return -1;
	}

	/* If there's only 1 level to set, stop here */
	if ((endp = strrchr(path, '/')) == path)
	{
		path++;
		return json_pointer_set_single_path(*obj, path, value, array_set_cb, priv);
	}

	/* pass a working copy to the recursive call */
	if (!(path_copy = strdup(path)))
	{
		errno = ENOMEM;
		return -1;
	}
	path_copy[endp - path] = '\0';
	rc = json_pointer_object_get_recursive(*obj, path_copy, &set);
	free(path_copy);

	if (rc)
		return rc;

	endp++;
	return json_pointer_set_single_path(set, endp, value, array_set_cb, priv);
}

int json_pointer_set(struct json_object **obj, const char *path, struct json_object *value)
{
	return json_pointer_set_with_array_cb(obj, path, value, json_object_array_put_idx_cb, NULL);
}

int json_pointer_setf(struct json_object **obj, struct json_object *value, const char *path_fmt,
                      ...)
{
	char *endp;
	char *path_copy = NULL;
	struct json_object *set = NULL;
	va_list args;
	int rc = 0;

	if (!obj || !path_fmt)
	{
		errno = EINVAL;
		return -1;
	}

	/* pass a working copy to the recursive call */
	va_start(args, path_fmt);
	rc = vasprintf(&path_copy, path_fmt, args);
	va_end(args);

	if (rc < 0)
		return rc;

	if (path_copy[0] == '\0')
	{
		json_object_put(*obj);
		*obj = value;
		goto out;
	}

	if (path_copy[0] != '/')
	{
		errno = EINVAL;
		rc = -1;
		goto out;
	}

	/* If there's only 1 level to set, stop here */
	if ((endp = strrchr(path_copy, '/')) == path_copy)
	{
		set = *obj;
		goto set_single_path;
	}

	*endp = '\0';
	rc = json_pointer_object_get_recursive(*obj, path_copy, &set);

	if (rc)
		goto out;

set_single_path:
	endp++;
	rc = json_pointer_set_single_path(set, endp, value,
					  json_object_array_put_idx_cb, NULL);
out:
	free(path_copy);
	return rc;
}
