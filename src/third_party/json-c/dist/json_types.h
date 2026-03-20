/*
 * Copyright (c) 2020 Eric Hawicz
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See COPYING for details.
 */

#ifndef _json_types_h_
#define _json_types_h_

/**
 * @file
 * @brief Basic types used in a few places in json-c, but you should include "json_object.h" instead.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef JSON_EXPORT
#if defined(_MSC_VER) && defined(JSON_C_DLL)
#define JSON_EXPORT __declspec(dllexport)
#else
#define JSON_EXPORT extern
#endif
#endif

struct printbuf;

/**
 * A structure to use with json_object_object_foreachC() loops.
 * Contains key, val and entry members.
 */
struct json_object_iter
{
	char *key;
	struct json_object *val;
	struct lh_entry *entry;
};
typedef struct json_object_iter json_object_iter;

typedef int json_bool;

/**
 * @brief The core type for all type of JSON objects handled by json-c
 */
typedef struct json_object json_object;

/**
 * Type of custom user delete functions.  See json_object_set_serializer.
 */
typedef void(json_object_delete_fn)(struct json_object *jso, void *userdata);

/**
 * Type of a custom serialization function.  See json_object_set_serializer.
 */
typedef int(json_object_to_json_string_fn)(struct json_object *jso, struct printbuf *pb, int level,
                                           int flags);

/* supported object types */

typedef enum json_type
{
	/* If you change this, be sure to update json_type_to_name() too */
	json_type_null,
	json_type_boolean,
	json_type_double,
	json_type_int,
	json_type_object,
	json_type_array,
	json_type_string
} json_type;

#ifdef __cplusplus
}
#endif

#endif
