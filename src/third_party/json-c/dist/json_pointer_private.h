/*
 * Copyright (c) 2023 Eric Hawicz
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See COPYING for details.
 */

/**
 * @file
 * @brief Do not use, json-c internal, may be changed or removed at any time.
 */
#ifndef _json_pointer_private_h_
#define _json_pointer_private_h_

#ifdef __cplusplus
extern "C" {
#endif

struct json_pointer_get_result {
	struct json_object *parent;
	struct json_object *obj;
	// The key of the found object; only valid when parent is json_type_object
	// Caution: re-uses tail end of the `path` argument to json_pointer_get_internal
	const char *key_in_parent;
	// the index of the found object; only valid when parent is json_type_array
	uint32_t index_in_parent;
};

int json_pointer_get_internal(struct json_object *obj, const char *path,
                              struct json_pointer_get_result *res);

typedef int(*json_pointer_array_set_cb)(json_object *parent, size_t idx,
                                        json_object *value, void *priv);

int json_pointer_set_with_array_cb(struct json_object **obj, const char *path,
                                   struct json_object *value,
                                   json_pointer_array_set_cb array_set_cb, void *priv);

#ifdef __cplusplus
}
#endif

#endif
