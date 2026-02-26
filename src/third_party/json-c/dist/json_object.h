/*
 * $Id: json_object.h,v 1.12 2006/01/30 23:07:57 mclark Exp $
 *
 * Copyright (c) 2004, 2005 Metaparadigm Pte. Ltd.
 * Michael Clark <michael@metaparadigm.com>
 * Copyright (c) 2009 Hewlett-Packard Development Company, L.P.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See COPYING for details.
 *
 */

/**
 * @file
 * @brief Core json-c API.  Start here, or with json_tokener.h
 */
#ifndef _json_object_h_
#define _json_object_h_

#ifdef __GNUC__
#define JSON_C_CONST_FUNCTION(func) func __attribute__((const))
#else
#define JSON_C_CONST_FUNCTION(func) func
#endif

#include "json_inttypes.h"
#include "json_types.h"
#include "printbuf.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JSON_OBJECT_DEF_HASH_ENTRIES 16

/**
 * A flag for the json_object_to_json_string_ext() and
 * json_object_to_file_ext() functions which causes the output
 * to have no extra whitespace or formatting applied.
 */
#define JSON_C_TO_STRING_PLAIN 0
/**
 * A flag for the json_object_to_json_string_ext() and
 * json_object_to_file_ext() functions which causes the output to have
 * minimal whitespace inserted to make things slightly more readable.
 */
#define JSON_C_TO_STRING_SPACED (1 << 0)
/**
 * A flag for the json_object_to_json_string_ext() and
 * json_object_to_file_ext() functions which causes
 * the output to be formatted.
 *
 * See the "Two Space Tab" option at https://jsonformatter.curiousconcept.com/
 * for an example of the format.
 */
#define JSON_C_TO_STRING_PRETTY (1 << 1)
/**
 * A flag for the json_object_to_json_string_ext() and
 * json_object_to_file_ext() functions which causes
 * the output to be formatted.
 *
 * Instead of a "Two Space Tab" this gives a single tab character.
 */
#define JSON_C_TO_STRING_PRETTY_TAB (1 << 3)
/**
 * A flag to drop trailing zero for float values
 */
#define JSON_C_TO_STRING_NOZERO (1 << 2)

/**
 * Don't escape forward slashes.
 */
#define JSON_C_TO_STRING_NOSLASHESCAPE (1 << 4)

/**
 * A flag for the json_object_to_json_string_ext() and
 * json_object_to_file_ext() functions which causes
 * the output to be formatted.
 *
 * Use color for printing json.
 */
#define JSON_C_TO_STRING_COLOR (1 << 5)

/**
 * A flag for the json_object_object_add_ex function which
 * causes the value to be added without a check if it already exists.
 * Note: it is the responsibility of the caller to ensure that no
 * key is added multiple times. If this is done, results are
 * unpredictable. While this option is somewhat dangerous, it
 * permits potentially large performance savings in code that
 * knows for sure the key values are unique (e.g. because the
 * code adds a well-known set of constant key values).
 */
#define JSON_C_OBJECT_ADD_KEY_IS_NEW (1 << 1)
/**
 * A flag for the json_object_object_add_ex function which
 * flags the key as being constant memory. This means that
 * the key will NOT be copied via strdup(), resulting in a
 * potentially huge performance win (malloc, strdup and
 * free are usually performance hogs). It is acceptable to
 * use this flag for keys in non-constant memory blocks if
 * the caller ensure that the memory holding the key lives
 * longer than the corresponding json object. However, this
 * is somewhat dangerous and should only be done if really
 * justified.
 * The general use-case for this flag is cases where the
 * key is given as a real constant value in the function
 * call, e.g. as in
 *   json_object_object_add_ex(obj, "ip", json,
 *       JSON_C_OBJECT_ADD_CONSTANT_KEY);
 */
#define JSON_C_OBJECT_ADD_CONSTANT_KEY (1 << 2)
/**
 * This flag is an alias to JSON_C_OBJECT_ADD_CONSTANT_KEY.
 * Historically, this flag was used first and the new name
 * JSON_C_OBJECT_ADD_CONSTANT_KEY was introduced for version
 * 0.16.00 in order to have regular naming.
 * Use of this flag is now legacy.
 */
#define JSON_C_OBJECT_KEY_IS_CONSTANT  JSON_C_OBJECT_ADD_CONSTANT_KEY

/**
 * Set the global value of an option, which will apply to all
 * current and future threads that have not set a thread-local value.
 *
 * @see json_c_set_serialization_double_format
 */
#define JSON_C_OPTION_GLOBAL (0)
/**
 * Set a thread-local value of an option, overriding the global value.
 * This will fail if json-c is not compiled with threading enabled, and
 * with the __thread specifier (or equivalent) available.
 *
 * @see json_c_set_serialization_double_format
 */
#define JSON_C_OPTION_THREAD (1)

/* reference counting functions */

/**
 * Increment the reference count of json_object, thereby taking ownership of it.
 *
 * Cases where you might need to increase the refcount include:
 * - Using an object field or array index (retrieved through
 *    `json_object_object_get()` or `json_object_array_get_idx()`)
 *    beyond the lifetime of the parent object.
 * - Detaching an object field or array index from its parent object
 *    (using `json_object_object_del()` or `json_object_array_del_idx()`)
 * - Sharing a json_object with multiple (not necessarily parallel) threads
 *    of execution that all expect to free it (with `json_object_put()`) when
 *    they're done.
 *
 * @param obj the json_object instance
 * @see json_object_put()
 * @see json_object_object_get()
 * @see json_object_array_get_idx()
 */
JSON_EXPORT struct json_object *json_object_get(struct json_object *obj);

/**
 * Decrement the reference count of json_object and free if it reaches zero.
 *
 * You must have ownership of obj prior to doing this or you will cause an
 * imbalance in the reference count, leading to a classic use-after-free bug.
 * In particular, you normally do not need to call `json_object_put()` on the
 * json_object returned by `json_object_object_get()` or `json_object_array_get_idx()`.
 *
 * Just like after calling `free()` on a block of memory, you must not use
 * `obj` after calling `json_object_put()` on it or any object that it
 * is a member of (unless you know you've called `json_object_get(obj)` to
 * explicitly increment the refcount).
 *
 * NULL may be passed, which which case this is a no-op.
 *
 * @param obj the json_object instance
 * @returns 1 if the object was freed.
 * @see json_object_get()
 */
JSON_EXPORT int json_object_put(struct json_object *obj);

/**
 * Check if the json_object is of a given type
 * @param obj the json_object instance
 * @param type one of:
     json_type_null (i.e. obj == NULL),
     json_type_boolean,
     json_type_double,
     json_type_int,
     json_type_object,
     json_type_array,
     json_type_string
 */
JSON_EXPORT int json_object_is_type(const struct json_object *obj, enum json_type type);

/**
 * Get the type of the json_object.  See also json_type_to_name() to turn this
 * into a string suitable, for instance, for logging.
 *
 * @param obj the json_object instance
 * @returns type being one of:
     json_type_null (i.e. obj == NULL),
     json_type_boolean,
     json_type_double,
     json_type_int,
     json_type_object,
     json_type_array,
     json_type_string
 */
JSON_EXPORT enum json_type json_object_get_type(const struct json_object *obj);

/** Stringify object to json format.
 * Equivalent to json_object_to_json_string_ext(obj, JSON_C_TO_STRING_SPACED)
 * The pointer you get is an internal of your json object. You don't
 * have to free it, later use of json_object_put() should be sufficient.
 * If you can not ensure there's no concurrent access to *obj use
 * strdup().
 * @param obj the json_object instance
 * @returns a string in JSON format
 */
JSON_EXPORT const char *json_object_to_json_string(struct json_object *obj);

/** Stringify object to json format
 * @see json_object_to_json_string() for details on how to free string.
 * @param obj the json_object instance
 * @param flags formatting options, see JSON_C_TO_STRING_PRETTY and other constants
 * @returns a string in JSON format
 */
JSON_EXPORT const char *json_object_to_json_string_ext(struct json_object *obj, int flags);

/** Stringify object to json format
 * @see json_object_to_json_string() for details on how to free string.
 * @param obj the json_object instance
 * @param flags formatting options, see JSON_C_TO_STRING_PRETTY and other constants
 * @param length a pointer where, if not NULL, the length (without null) is stored
 * @returns a string in JSON format and the length if not NULL
 */
JSON_EXPORT const char *json_object_to_json_string_length(struct json_object *obj, int flags,
                                                          size_t *length);

/**
 * Returns the userdata set by json_object_set_userdata() or
 * json_object_set_serializer()
 *
 * @param jso the object to return the userdata for
 */
JSON_EXPORT void *json_object_get_userdata(json_object *jso);

/**
 * Set an opaque userdata value for an object
 *
 * The userdata can be retrieved using json_object_get_userdata().
 *
 * If custom userdata is already set on this object, any existing user_delete
 * function is called before the new one is set.
 *
 * The user_delete parameter is optional and may be passed as NULL, even if
 * the userdata parameter is non-NULL.  It will be called just before the
 * json_object is deleted, after it's reference count goes to zero
 * (see json_object_put()).
 * If this is not provided, it is up to the caller to free the userdata at
 * an appropriate time. (i.e. after the json_object is deleted)
 *
 * Note: Objects created by parsing strings may have custom serializers set
 * which expect the userdata to contain specific data (due to use of
 * json_object_new_double_s()). In this case, json_object_set_serialiser() with
 * NULL as to_string_func should be used instead to set the userdata and reset
 * the serializer to its default value.
 *
 * @param jso the object to set the userdata for
 * @param userdata an optional opaque cookie
 * @param user_delete an optional function from freeing userdata
 */
JSON_EXPORT void json_object_set_userdata(json_object *jso, void *userdata,
                                          json_object_delete_fn *user_delete);

/**
 * Set a custom serialization function to be used when this particular object
 * is converted to a string by json_object_to_json_string.
 *
 * If custom userdata is already set on this object, any existing user_delete
 * function is called before the new one is set.
 *
 * If to_string_func is NULL the default behaviour is reset (but the userdata
 * and user_delete fields are still set).
 *
 * The userdata parameter is optional and may be passed as NULL. It can be used
 * to provide additional data for to_string_func to use. This parameter may
 * be NULL even if user_delete is non-NULL.
 *
 * The user_delete parameter is optional and may be passed as NULL, even if
 * the userdata parameter is non-NULL.  It will be called just before the
 * json_object is deleted, after it's reference count goes to zero
 * (see json_object_put()).
 * If this is not provided, it is up to the caller to free the userdata at
 * an appropriate time. (i.e. after the json_object is deleted)
 *
 * Note that the userdata is the same as set by json_object_set_userdata(), so
 * care must be taken not to overwrite the value when both a custom serializer
 * and json_object_set_userdata() are used.
 *
 * @param jso the object to customize
 * @param to_string_func the custom serialization function
 * @param userdata an optional opaque cookie
 * @param user_delete an optional function from freeing userdata
 */
JSON_EXPORT void json_object_set_serializer(json_object *jso,
                                            json_object_to_json_string_fn *to_string_func,
                                            void *userdata, json_object_delete_fn *user_delete);

#ifdef __clang__
/*
 * Clang doesn't pay attention to the parameters defined in the
 * function typedefs used here, so turn off spurious doc warnings.
 * {
 */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation"
#endif

/**
 * Simply call free on the userdata pointer.
 * Can be used with json_object_set_serializer().
 *
 * @param jso unused
 * @param userdata the pointer that is passed to free().
 */
JSON_EXPORT json_object_delete_fn json_object_free_userdata;

/**
 * Copy the jso->_userdata string over to pb as-is.
 * Can be used with json_object_set_serializer().
 *
 * @param jso The object whose _userdata is used.
 * @param pb The destination buffer.
 * @param level Ignored.
 * @param flags Ignored.
 */
JSON_EXPORT json_object_to_json_string_fn json_object_userdata_to_json_string;

#ifdef __clang__
/* } */
#pragma clang diagnostic pop
#endif

/* object type methods */

/** Create a new empty object with a reference count of 1.  The caller of
 * this object initially has sole ownership.  Remember, when using
 * json_object_object_add or json_object_array_put_idx, ownership will
 * transfer to the object/array.  Call json_object_get if you want to maintain
 * shared ownership or also add this object as a child of multiple objects or
 * arrays.  Any ownerships you acquired but did not transfer must be released
 * through json_object_put.
 *
 * @returns a json_object of type json_type_object
 */
JSON_EXPORT struct json_object *json_object_new_object(void);

/** Get the hashtable of a json_object of type json_type_object
 * @param obj the json_object instance
 * @returns a linkhash
 */
JSON_EXPORT struct lh_table *json_object_get_object(const struct json_object *obj);

/** Get the size of an object in terms of the number of fields it has.
 * @param obj the json_object whose length to return
 */
JSON_EXPORT int json_object_object_length(const struct json_object *obj);

/** Get the sizeof (struct json_object).
 * @returns a size_t with the sizeof (struct json_object)
 */
JSON_C_CONST_FUNCTION(JSON_EXPORT size_t json_c_object_sizeof(void));

/** Add an object field to a json_object of type json_type_object
 *
 * The reference count of `val` will *not* be incremented, in effect
 * transferring ownership that object to `obj`, and thus `val` will be
 * freed when `obj` is.  (i.e. through `json_object_put(obj)`)
 *
 * If you want to retain a reference to the added object, independent
 * of the lifetime of obj, you must increment the refcount with
 * `json_object_get(val)` (and later release it with json_object_put()).
 *
 * Since ownership transfers to `obj`, you must make sure
 * that you do in fact have ownership over `val`.  For instance,
 * json_object_new_object() will give you ownership until you transfer it,
 * whereas json_object_object_get() does not.
 *
 * Any previous object stored under `key` in `obj` will have its refcount
 * decremented, and be freed normally if that drops to zero.
 *
 * @param obj the json_object instance
 * @param key the object field name (a private copy will be duplicated)
 * @param val a json_object or NULL member to associate with the given field
 *
 * @return On success, <code>0</code> is returned.
 * 	On error, a negative value is returned.
 */
JSON_EXPORT int json_object_object_add(struct json_object *obj, const char *key,
                                       struct json_object *val);

/** Add an object field to a json_object of type json_type_object
 *
 * The semantics are identical to json_object_object_add, except that an
 * additional flag fields gives you more control over some detail aspects
 * of processing. See the description of JSON_C_OBJECT_ADD_* flags for more
 * details.
 *
 * @param obj the json_object instance
 * @param key the object field name (a private copy will be duplicated)
 * @param val a json_object or NULL member to associate with the given field
 * @param opts process-modifying options. To specify multiple options, use
 *             (OPT1|OPT2)
 */
JSON_EXPORT int json_object_object_add_ex(struct json_object *obj, const char *const key,
                                          struct json_object *const val, const unsigned opts);

/** Get the json_object associate with a given object field.
 * Deprecated/discouraged: used json_object_object_get_ex instead.
 *
 * This returns NULL if the field is found but its value is null, or if
 *  the field is not found, or if obj is not a json_type_object.  If you
 *  need to distinguish between these cases, use json_object_object_get_ex().
 *
 * *No* reference counts will be changed.  There is no need to manually adjust
 * reference counts through the json_object_put/json_object_get methods unless
 * you need to have the child (value) reference maintain a different lifetime
 * than the owning parent (obj). Ownership of the returned value is retained
 * by obj (do not do json_object_put unless you have done a json_object_get).
 * If you delete the value from obj (json_object_object_del) and wish to access
 * the returned reference afterwards, make sure you have first gotten shared
 * ownership through json_object_get (& don't forget to do a json_object_put
 * or transfer ownership to prevent a memory leak).
 *
 * @param obj the json_object instance
 * @param key the object field name
 * @returns the json_object associated with the given field name
 */
JSON_EXPORT struct json_object *json_object_object_get(const struct json_object *obj,
                                                       const char *key);

/** Get the json_object associated with a given object field.
 *
 * This returns true if the key is found, false in all other cases (including
 * if obj isn't a json_type_object).
 *
 * *No* reference counts will be changed.  There is no need to manually adjust
 * reference counts through the json_object_put/json_object_get methods unless
 * you need to have the child (value) reference maintain a different lifetime
 * than the owning parent (obj).  Ownership of value is retained by obj.
 *
 * @param obj the json_object instance
 * @param key the object field name
 * @param value a pointer where to store a reference to the json_object
 *              associated with the given field name.
 *
 *              It is safe to pass a NULL value.
 * @returns whether or not the key exists
 */
JSON_EXPORT json_bool json_object_object_get_ex(const struct json_object *obj, const char *key,
                                                struct json_object **value);

/** Delete the given json_object field
 *
 * The reference count will be decremented for the deleted object.  If there
 * are no more owners of the value represented by this key, then the value is
 * freed.  Otherwise, the reference to the value will remain in memory.
 *
 * @param obj the json_object instance
 * @param key the object field name
 */
JSON_EXPORT void json_object_object_del(struct json_object *obj, const char *key);

/**
 * Iterate through all keys and values of an object.
 *
 * Adding keys to the object while iterating is NOT allowed.
 *
 * Deleting an existing key, or replacing an existing key with a
 * new value IS allowed.
 *
 * @param obj the json_object instance
 * @param key the local name for the char* key variable defined in the body
 * @param val the local name for the json_object* object variable defined in
 *            the body
 */
#if defined(__GNUC__) && !defined(__STRICT_ANSI__) && (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L)

#define json_object_object_foreach(obj, key, val)                                \
	char *key = NULL;                                                        \
	struct json_object *val __attribute__((__unused__)) = NULL;              \
	for (struct lh_entry *entry##key = lh_table_head(json_object_get_object(obj)),    \
	                     *entry_next##key = NULL;                            \
	     ({                                                                  \
		     if (entry##key)                                             \
		     {                                                           \
			     key = (char *)lh_entry_k(entry##key);               \
			     val = (struct json_object *)lh_entry_v(entry##key); \
			     entry_next##key = lh_entry_next(entry##key);        \
		     };                                                          \
		     entry##key;                                                 \
	     });                                                                 \
	     entry##key = entry_next##key)

#else /* ANSI C or MSC */

#define json_object_object_foreach(obj, key, val)                              \
	char *key = NULL;                                                      \
	struct json_object *val = NULL;                                        \
	struct lh_entry *entry##key;                                           \
	struct lh_entry *entry_next##key = NULL;                               \
	for (entry##key = lh_table_head(json_object_get_object(obj));          \
	     (entry##key ? (key = (char *)lh_entry_k(entry##key),              \
	                   val = (struct json_object *)lh_entry_v(entry##key), \
	                   entry_next##key = lh_entry_next(entry##key), entry##key)     \
	                 : 0);                                                 \
	     entry##key = entry_next##key)

#endif /* defined(__GNUC__) && !defined(__STRICT_ANSI__) && (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) */

/** Iterate through all keys and values of an object (ANSI C Safe)
 * @param obj the json_object instance
 * @param iter the object iterator, use type json_object_iter
 */
#define json_object_object_foreachC(obj, iter)                                                  \
	for (iter.entry = lh_table_head(json_object_get_object(obj));                                    \
	     (iter.entry ? (iter.key = (char *)lh_entry_k(iter.entry),                          \
	                   iter.val = (struct json_object *)lh_entry_v(iter.entry), iter.entry) \
	                 : 0);                                                                  \
	     iter.entry = lh_entry_next(iter.entry))

/* Array type methods */

/** Create a new empty json_object of type json_type_array
 * with 32 slots allocated.
 * If you know the array size you'll need ahead of time, use
 * json_object_new_array_ext() instead.
 * @see json_object_new_array_ext()
 * @see json_object_array_shrink()
 * @returns a json_object of type json_type_array
 */
JSON_EXPORT struct json_object *json_object_new_array(void);

/** Create a new empty json_object of type json_type_array
 * with the desired number of slots allocated.
 * @see json_object_array_shrink()
 * @param initial_size the number of slots to allocate
 * @returns a json_object of type json_type_array
 */
JSON_EXPORT struct json_object *json_object_new_array_ext(int initial_size);

/** Get the arraylist of a json_object of type json_type_array
 * @param obj the json_object instance
 * @returns an arraylist
 */
JSON_EXPORT struct array_list *json_object_get_array(const struct json_object *obj);

/** Get the length of a json_object of type json_type_array
 * @param obj the json_object instance
 * @returns an int
 */
JSON_EXPORT size_t json_object_array_length(const struct json_object *obj);

/** Sorts the elements of jso of type json_type_array
*
* Pointers to the json_object pointers will be passed as the two arguments
* to sort_fn
*
* @param jso the json_object instance
* @param sort_fn a sorting function
*/
JSON_EXPORT void json_object_array_sort(struct json_object *jso,
                                        int (*sort_fn)(const void *, const void *));

/** Binary search a sorted array for a specified key object.
 *
 * It depends on your compare function what's sufficient as a key.
 * Usually you create some dummy object with the parameter compared in
 * it, to identify the right item you're actually looking for.
 *
 * @see json_object_array_sort() for hints on the compare function.
 *
 * @param key a dummy json_object with the right key
 * @param jso the array object we're searching
 * @param sort_fn the sort/compare function
 *
 * @return the wanted json_object instance
 */
JSON_EXPORT struct json_object *
json_object_array_bsearch(const struct json_object *key, const struct json_object *jso,
                          int (*sort_fn)(const void *, const void *));

/** Add an element to the end of a json_object of type json_type_array
 *
 * The reference count will *not* be incremented. This is to make adding
 * fields to objects in code more compact. If you want to retain a reference
 * to an added object you must wrap the passed object with json_object_get
 *
 * @param obj the json_object instance
 * @param val the json_object to be added
 */
JSON_EXPORT int json_object_array_add(struct json_object *obj, struct json_object *val);

/** Insert or replace an element at a specified index in an array (a json_object of type json_type_array)
 *
 * The reference count will *not* be incremented. This is to make adding
 * fields to objects in code more compact. If you want to retain a reference
 * to an added object you must wrap the passed object with json_object_get
 *
 * The reference count of a replaced object will be decremented.
 *
 * The array size will be automatically be expanded to the size of the
 * index if the index is larger than the current size.
 *
 * @param obj the json_object instance
 * @param idx the index to insert the element at
 * @param val the json_object to be added
 */
JSON_EXPORT int json_object_array_put_idx(struct json_object *obj, size_t idx,
                                          struct json_object *val);

/** Insert an element at a specified index in an array (a json_object of type json_type_array)
 *
 * The reference count will *not* be incremented. This is to make adding
 * fields to objects in code more compact. If you want to retain a reference
 * to an added object you must wrap the passed object with json_object_get
 *
 * The array size will be automatically be expanded to the size of the
 * index if the index is larger than the current size.
 * If the index is within the existing array limits, then the element will be
 * inserted and all elements will be shifted. This is the only difference between
 * this function and json_object_array_put_idx().
 *
 * @param obj the json_object instance
 * @param idx the index to insert the element at
 * @param val the json_object to be added
 */
JSON_EXPORT int json_object_array_insert_idx(struct json_object *obj, size_t idx,
                                             struct json_object *val);

/** Get the element at specified index of array `obj` (which must be a json_object of type json_type_array)
 *
 * *No* reference counts will be changed, and ownership of the returned
 * object remains with `obj`.  See json_object_object_get() for additional
 * implications of this behavior.
 *
 * Calling this with anything other than a json_type_array will trigger
 * an assert.
 *
 * @param obj the json_object instance
 * @param idx the index to get the element at
 * @returns the json_object at the specified index (or NULL)
 */
JSON_EXPORT struct json_object *json_object_array_get_idx(const struct json_object *obj,
                                                          size_t idx);

/** Delete an elements from a specified index in an array (a json_object of type json_type_array)
 *
 * The reference count will be decremented for each of the deleted objects.  If there
 * are no more owners of an element that is being deleted, then the value is
 * freed.  Otherwise, the reference to the value will remain in memory.
 *
 * @param obj the json_object instance
 * @param idx the index to start deleting elements at
 * @param count the number of elements to delete
 * @returns 0 if the elements were successfully deleted
 */
JSON_EXPORT int json_object_array_del_idx(struct json_object *obj, size_t idx, size_t count);

/**
 * Shrink the internal memory allocation of the array to just
 * enough to fit the number of elements in it, plus empty_slots.
 *
 * @param jso the json_object instance, must be json_type_array
 * @param empty_slots the number of empty slots to leave allocated
 */
JSON_EXPORT int json_object_array_shrink(struct json_object *jso, int empty_slots);

/* json_bool type methods */

/** Create a new empty json_object of type json_type_boolean
 * @param b a json_bool 1 or 0
 * @returns a json_object of type json_type_boolean
 */
JSON_EXPORT struct json_object *json_object_new_boolean(json_bool b);

/** Get the json_bool value of a json_object
 *
 * The type is coerced to a json_bool if the passed object is not a json_bool.
 * integer and double objects will return 0 if there value is zero
 * or 1 otherwise. If the passed object is a string it will return
 * 1 if it has a non zero length. 
 * If any other object type is passed 0 will be returned, even non-empty
 *  json_type_array and json_type_object objects.
 *
 * @param obj the json_object instance
 * @returns a json_bool
 */
JSON_EXPORT json_bool json_object_get_boolean(const struct json_object *obj);

/** Set the json_bool value of a json_object
 *
 * The type of obj is checked to be a json_type_boolean and 0 is returned
 * if it is not without any further actions. If type of obj is json_type_boolean
 * the object value is changed to new_value
 *
 * @param obj the json_object instance
 * @param new_value the value to be set
 * @returns 1 if value is set correctly, 0 otherwise
 */
JSON_EXPORT int json_object_set_boolean(struct json_object *obj, json_bool new_value);

/* int type methods */

/** Create a new empty json_object of type json_type_int
 * Note that values are stored as 64-bit values internally.
 * To ensure the full range is maintained, use json_object_new_int64 instead.
 * @param i the integer
 * @returns a json_object of type json_type_int
 */
JSON_EXPORT struct json_object *json_object_new_int(int32_t i);

/** Create a new empty json_object of type json_type_int
 * @param i the integer
 * @returns a json_object of type json_type_int
 */
JSON_EXPORT struct json_object *json_object_new_int64(int64_t i);

/** Create a new empty json_object of type json_type_uint
 * @param i the integer
 * @returns a json_object of type json_type_uint
 */
JSON_EXPORT struct json_object *json_object_new_uint64(uint64_t i);

/** Get the int value of a json_object
 *
 * The type is coerced to a int if the passed object is not a int.
 * double objects will return their integer conversion. Strings will be
 * parsed as an integer. If no conversion exists then 0 is returned
 * and errno is set to EINVAL. null is equivalent to 0 (no error values set)
 *
 * Note that integers are stored internally as 64-bit values.
 * If the value of too big or too small to fit into 32-bit, INT32_MAX or
 * INT32_MIN are returned, respectively.
 *
 * @param obj the json_object instance
 * @returns an int
 */
JSON_EXPORT int32_t json_object_get_int(const struct json_object *obj);

/** Set the int value of a json_object
 *
 * The type of obj is checked to be a json_type_int and 0 is returned
 * if it is not without any further actions. If type of obj is json_type_int
 * the object value is changed to new_value
 *
 * @param obj the json_object instance
 * @param new_value the value to be set
 * @returns 1 if value is set correctly, 0 otherwise
 */
JSON_EXPORT int json_object_set_int(struct json_object *obj, int new_value);

/** Increment a json_type_int object by the given amount, which may be negative.
 *
 * If the type of obj is not json_type_int then 0 is returned with no further
 * action taken.
 * If the addition would result in a overflow, the object value
 * is set to INT64_MAX.
 * If the addition would result in a underflow, the object value
 * is set to INT64_MIN.
 * Neither overflow nor underflow affect the return value.
 *
 * @param obj the json_object instance
 * @param val the value to add
 * @returns 1 if the increment succeeded, 0 otherwise
 */
JSON_EXPORT int json_object_int_inc(struct json_object *obj, int64_t val);

/** Get the int value of a json_object
 *
 * The type is coerced to a int64 if the passed object is not a int64.
 * double objects will return their int64 conversion. Strings will be
 * parsed as an int64. If no conversion exists then 0 is returned.
 *
 * NOTE: Set errno to 0 directly before a call to this function to determine
 * whether or not conversion was successful (it does not clear the value for
 * you).
 *
 * @param obj the json_object instance
 * @returns an int64
 */
JSON_EXPORT int64_t json_object_get_int64(const struct json_object *obj);

/** Get the uint value of a json_object
 *
 * The type is coerced to a uint64 if the passed object is not a uint64.
 * double objects will return their uint64 conversion. Strings will be
 * parsed as an uint64. If no conversion exists then 0 is returned.
 *
 * NOTE: Set errno to 0 directly before a call to this function to determine
 * whether or not conversion was successful (it does not clear the value for
 * you).
 *
 * @param obj the json_object instance
 * @returns an uint64
 */
JSON_EXPORT uint64_t json_object_get_uint64(const struct json_object *obj);

/** Set the int64_t value of a json_object
 *
 * The type of obj is checked to be a json_type_int and 0 is returned
 * if it is not without any further actions. If type of obj is json_type_int
 * the object value is changed to new_value
 *
 * @param obj the json_object instance
 * @param new_value the value to be set
 * @returns 1 if value is set correctly, 0 otherwise
 */
JSON_EXPORT int json_object_set_int64(struct json_object *obj, int64_t new_value);

/** Set the uint64_t value of a json_object
 *
 * The type of obj is checked to be a json_type_uint and 0 is returned
 * if it is not without any further actions. If type of obj is json_type_uint
 * the object value is changed to new_value
 *
 * @param obj the json_object instance
 * @param new_value the value to be set
 * @returns 1 if value is set correctly, 0 otherwise
 */
JSON_EXPORT int json_object_set_uint64(struct json_object *obj, uint64_t new_value);

/* double type methods */

/** Create a new empty json_object of type json_type_double
 *
 * @see json_object_double_to_json_string() for how to set a custom format string.
 *
 * @param d the double
 * @returns a json_object of type json_type_double
 */
JSON_EXPORT struct json_object *json_object_new_double(double d);

/**
 * Create a new json_object of type json_type_double, using
 * the exact serialized representation of the value.
 *
 * This allows for numbers that would otherwise get displayed
 * inefficiently (e.g. 12.3 => "12.300000000000001") to be
 * serialized with the more convenient form.
 *
 * Notes:
 *
 * This is used by json_tokener_parse_ex() to allow for
 * an exact re-serialization of a parsed object.
 *
 * The userdata field is used to store the string representation, so it
 * can't be used for other data if this function is used.
 *
 * A roughly equivalent sequence of calls, with the difference being that
 *  the serialization function won't be reset by json_object_set_double(), is:
 * @code
 *   jso = json_object_new_double(d);
 *   json_object_set_serializer(jso, json_object_userdata_to_json_string,
 *       strdup(ds), json_object_free_userdata);
 * @endcode
 *
 * @param d the numeric value of the double.
 * @param ds the string representation of the double.  This will be copied.
 */
JSON_EXPORT struct json_object *json_object_new_double_s(double d, const char *ds);

/**
 * Set a global or thread-local json-c option, depending on whether
 *  JSON_C_OPTION_GLOBAL or JSON_C_OPTION_THREAD is passed.
 * Thread-local options default to undefined, and inherit from the global
 *  value, even if the global value is changed after the thread is created.
 * Attempting to set thread-local options when threading is not compiled in
 *  will result in an error.  Be sure to check the return value.
 *
 * double_format is a "%g" printf format, such as "%.20g"
 *
 * @return -1 on errors, 0 on success.
 */
JSON_EXPORT int json_c_set_serialization_double_format(const char *double_format,
                                                       int global_or_thread);

/** Serialize a json_object of type json_type_double to a string.
 *
 * This function isn't meant to be called directly. Instead, you can set a
 * custom format string for the serialization of this double using the
 * following call (where "%.17g" actually is the default):
 *
 * @code
 *   jso = json_object_new_double(d);
 *   json_object_set_serializer(jso, json_object_double_to_json_string,
 *       "%.17g", NULL);
 * @endcode
 *
 * @see printf(3) man page for format strings
 *
 * @param jso The json_type_double object that is serialized.
 * @param pb The destination buffer.
 * @param level Ignored.
 * @param flags Ignored.
 */
JSON_EXPORT int json_object_double_to_json_string(struct json_object *jso, struct printbuf *pb,
                                                  int level, int flags);

/** Get the double floating point value of a json_object
 *
 * The type is coerced to a double if the passed object is not a double.
 * integer objects will return their double conversion. Strings will be
 * parsed as a double. If no conversion exists then 0.0 is returned and
 * errno is set to EINVAL. null is equivalent to 0 (no error values set)
 *
 * If the value is too big to fit in a double, then the value is set to
 * the closest infinity with errno set to ERANGE. If strings cannot be
 * converted to their double value, then EINVAL is set & NaN is returned.
 *
 * Arrays of length 0 are interpreted as 0 (with no error flags set).
 * Arrays of length 1 are effectively cast to the equivalent object and
 * converted using the above rules.  All other arrays set the error to
 * EINVAL & return NaN.
 *
 * NOTE: Set errno to 0 directly before a call to this function to
 * determine whether or not conversion was successful (it does not clear
 * the value for you).
 *
 * @param obj the json_object instance
 * @returns a double floating point number
 */
JSON_EXPORT double json_object_get_double(const struct json_object *obj);

/** Set the double value of a json_object
 *
 * The type of obj is checked to be a json_type_double and 0 is returned
 * if it is not without any further actions. If type of obj is json_type_double
 * the object value is changed to new_value
 *
 * If the object was created with json_object_new_double_s(), the serialization
 * function is reset to the default and the cached serialized value is cleared.
 *
 * @param obj the json_object instance
 * @param new_value the value to be set
 * @returns 1 if value is set correctly, 0 otherwise
 */
JSON_EXPORT int json_object_set_double(struct json_object *obj, double new_value);

/* string type methods */

/** Create a new empty json_object of type json_type_string
 *
 * A copy of the string is made and the memory is managed by the json_object
 *
 * @param s the string
 * @returns a json_object of type json_type_string
 * @see json_object_new_string_len()
 */
JSON_EXPORT struct json_object *json_object_new_string(const char *s);

/** Create a new empty json_object of type json_type_string and allocate
 * len characters for the new string.
 *
 * A copy of the string is made and the memory is managed by the json_object
 *
 * @param s the string
 * @param len max length of the new string
 * @returns a json_object of type json_type_string
 * @see json_object_new_string()
 */
JSON_EXPORT struct json_object *json_object_new_string_len(const char *s, const int len);

/** Get the string value of a json_object
 *
 * If the passed object is of type json_type_null (i.e. obj == NULL),
 * NULL is returned.
 *
 * If the passed object of type json_type_string, the string contents
 * are returned.
 *
 * Otherwise the JSON representation of the object is returned.
 *
 * The returned string memory is managed by the json_object and will
 * be freed when the reference count of the json_object drops to zero.
 *
 * @param obj the json_object instance
 * @returns a string or NULL
 */
JSON_EXPORT const char *json_object_get_string(struct json_object *obj);

/** Get the string length of a json_object
 *
 * If the passed object is not of type json_type_string then zero
 * will be returned.
 *
 * @param obj the json_object instance
 * @returns int
 */
JSON_EXPORT int json_object_get_string_len(const struct json_object *obj);

/** Set the string value of a json_object with zero terminated strings
 * equivalent to json_object_set_string_len (obj, new_value, strlen(new_value))
 * @returns 1 if value is set correctly, 0 otherwise
 */
JSON_EXPORT int json_object_set_string(json_object *obj, const char *new_value);

/** Set the string value of a json_object str
 *
 * The type of obj is checked to be a json_type_string and 0 is returned
 * if it is not without any further actions. If type of obj is json_type_string
 * the object value is changed to new_value
 *
 * @param obj the json_object instance
 * @param new_value the value to be set; Since string length is given in len this need not be zero terminated
 * @param len the length of new_value
 * @returns 1 if value is set correctly, 0 otherwise
 */
JSON_EXPORT int json_object_set_string_len(json_object *obj, const char *new_value, int len);

/** This method exists only to provide a complementary function
 * along the lines of the other json_object_new_* functions.
 * It always returns NULL, and it is entirely acceptable to simply use NULL directly.
 */
JSON_EXPORT struct json_object *json_object_new_null(void);

/** Check if two json_object's are equal
 *
 * If the passed objects are equal 1 will be returned.
 * Equality is defined as follows:
 * - json_objects of different types are never equal
 * - json_objects of the same primitive type are equal if the
 *   c-representation of their value is equal
 * - json-arrays are considered equal if all values at the same
 *   indices are equal (same order)
 * - Complex json_objects are considered equal if all
 *   contained objects referenced by their key are equal,
 *   regardless their order.
 *
 * @param obj1 the first json_object instance
 * @param obj2 the second json_object instance
 * @returns whether both objects are equal or not
 */
JSON_EXPORT int json_object_equal(struct json_object *obj1, struct json_object *obj2);

/**
 * Perform a shallow copy of src into *dst as part of an overall json_object_deep_copy().
 *
 * If src is part of a containing object or array, parent will be non-NULL,
 * and key or index will be provided.
 * When shallow_copy is called *dst will be NULL, and must be non-NULL when it returns.
 * src will never be NULL.
 *
 * If shallow_copy sets the serializer on an object, return 2 to indicate to
 *  json_object_deep_copy that it should not attempt to use the standard userdata
 *  copy function.
 *
 * @return On success 1 or 2, -1 on errors
 */
typedef int(json_c_shallow_copy_fn)(json_object *src, json_object *parent, const char *key,
                                    size_t index, json_object **dst);

/**
 * The default shallow copy implementation for use with json_object_deep_copy().
 * This simply calls the appropriate json_object_new_<type>() function and
 * copies over the serializer function (_to_json_string internal field of
 * the json_object structure) but not any _userdata or _user_delete values.
 *
 * If you're writing a custom shallow_copy function, perhaps because you're using
 * your own custom serializer, you can call this first to create the new object
 * before customizing it with json_object_set_serializer().
 *
 * @return 1 on success, -1 on errors, but never 2.
 */
JSON_EXPORT json_c_shallow_copy_fn json_c_shallow_copy_default;

/**
 * Copy the contents of the JSON object.
 * The destination object must be initialized to NULL,
 * to make sure this function won't overwrite an existing JSON object.
 *
 * This does roughly the same thing as
 * `json_tokener_parse(json_object_get_string(src))`.
 *
 * @param src source JSON object whose contents will be copied
 * @param dst pointer to the destination object where the contents of `src`;
 *            make sure this pointer is initialized to NULL
 * @param shallow_copy an optional function to copy individual objects, needed
 *                     when custom serializers are in use.  See also
 *                     json_object set_serializer.
 *
 * @returns 0 if the copy went well, -1 if an error occurred during copy
 *          or if the destination pointer is non-NULL
 */

JSON_EXPORT int json_object_deep_copy(struct json_object *src, struct json_object **dst,
                                      json_c_shallow_copy_fn *shallow_copy);
#ifdef __cplusplus
}
#endif

#endif
