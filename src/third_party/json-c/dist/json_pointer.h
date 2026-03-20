/*
 * Copyright (c) 2016 Alexadru Ardelean.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See COPYING for details.
 *
 */

/**
 * @file
 * @brief JSON Pointer (RFC 6901) implementation for retrieving
 *        objects from a json-c object tree.
 */
#ifndef _json_pointer_h_
#define _json_pointer_h_

#include "json_object.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Retrieves a JSON sub-object from inside another JSON object
 * using the JSON pointer notation as defined in RFC 6901
 *   https://tools.ietf.org/html/rfc6901
 *
 * The returned JSON sub-object is equivalent to parsing manually the
 * 'obj' JSON tree ; i.e. it's not a new object that is created, but rather
 * a pointer inside the JSON tree.
 *
 * Internally, this is equivalent to doing a series of 'json_object_object_get()'
 * and 'json_object_array_get_idx()' along the given 'path'.
 *
 * @param obj the json_object instance/tree from where to retrieve sub-objects
 * @param path a (RFC6901) string notation for the sub-object to retrieve
 * @param res a pointer that stores a reference to the json_object
 *              associated with the given path
 *
 * @return negative if an error (or not found), or 0 if succeeded
 */
JSON_EXPORT int json_pointer_get(struct json_object *obj, const char *path,
                                 struct json_object **res);

/**
 * This is a variant of 'json_pointer_get()' that supports printf() style arguments.
 *
 * Variable arguments go after the 'path_fmt' parameter.
 *
 * Example: json_pointer_getf(obj, res, "/foo/%d/%s", 0, "bar")
 * This also means that you need to escape '%' with '%%' (just like in printf())
 *
 * Please take into consideration all recommended 'printf()' format security
 * aspects when using this function.
 *
 * @param obj the json_object instance/tree to which to add a sub-object
 * @param res a pointer that stores a reference to the json_object
 *              associated with the given path
 * @param path_fmt a printf() style format for the path
 *
 * @return negative if an error (or not found), or 0 if succeeded
 */
JSON_EXPORT int json_pointer_getf(struct json_object *obj, struct json_object **res,
                                  const char *path_fmt, ...);

/**
 * Sets JSON object 'value' in the 'obj' tree at the location specified
 * by the 'path'. 'path' is JSON pointer notation as defined in RFC 6901
 *   https://tools.ietf.org/html/rfc6901
 *
 * Note that 'obj' is a double pointer, mostly for the "" (empty string)
 * case, where the entire JSON object would be replaced by 'value'.
 * In the case of the "" path, the object at '*obj' will have it's refcount
 * decremented with 'json_object_put()' and the 'value' object will be assigned to it.
 *
 * For other cases (JSON sub-objects) ownership of 'value' will be transferred into
 * '*obj' via 'json_object_object_add()' & 'json_object_array_put_idx()', so the
 * only time the refcount should be decremented for 'value' is when the return value of
 * 'json_pointer_set()' is negative (meaning the 'value' object did not get set into '*obj').
 *
 * That also implies that 'json_pointer_set()' does not do any refcount incrementing.
 * (Just that single decrement that was mentioned above).
 *
 * @param obj the json_object instance/tree to which to add a sub-object
 * @param path a (RFC6901) string notation for the sub-object to set in the tree
 * @param value object to set at path
 *
 * @return negative if an error (or not found), or 0 if succeeded
 */
JSON_EXPORT int json_pointer_set(struct json_object **obj, const char *path,
                                 struct json_object *value);

/**
 * This is a variant of 'json_pointer_set()' that supports printf() style arguments.
 *
 * Variable arguments go after the 'path_fmt' parameter.
 *
 * Example: json_pointer_setf(obj, value, "/foo/%d/%s", 0, "bar")
 * This also means that you need to escape '%' with '%%' (just like in printf())
 *
 * Please take into consideration all recommended 'printf()' format security
 * aspects when using this function.
 *
 * @param obj the json_object instance/tree to which to add a sub-object
 * @param value object to set at path
 * @param path_fmt a printf() style format for the path
 *
 * @return negative if an error (or not found), or 0 if succeeded
 */
JSON_EXPORT int json_pointer_setf(struct json_object **obj, struct json_object *value,
                                  const char *path_fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
