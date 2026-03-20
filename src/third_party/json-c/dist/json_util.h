/*
 * $Id: json_util.h,v 1.4 2006/01/30 23:07:57 mclark Exp $
 *
 * Copyright (c) 2004, 2005 Metaparadigm Pte. Ltd.
 * Michael Clark <michael@metaparadigm.com>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See COPYING for details.
 *
 */

/**
 * @file
 * @brief Miscllaneous utility functions and macros.
 */
#ifndef _json_util_h_
#define _json_util_h_

#include "json_object.h"

#ifndef json_min
#define json_min(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef json_max
#define json_max(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define JSON_FILE_BUF_SIZE 4096

/* utility functions */
/**
 * Read the full contents of the given file, then convert it to a
 * json_object using json_tokener_parse().
 *
 * Returns NULL on failure.  See json_util_get_last_err() for details.
 */
JSON_EXPORT struct json_object *json_object_from_file(const char *filename);

/**
 * Create a JSON object from already opened file descriptor.
 *
 * This function can be helpful, when you opened the file already,
 * e.g. when you have a temp file.
 * Note, that the fd must be readable at the actual position, i.e.
 * use lseek(fd, 0, SEEK_SET) before.
 *
 * The depth argument specifies the maximum object depth to pass to
 * json_tokener_new_ex().  When depth == -1, JSON_TOKENER_DEFAULT_DEPTH
 * is used instead.
 *
 * Returns NULL on failure.  See json_util_get_last_err() for details.
 */
JSON_EXPORT struct json_object *json_object_from_fd_ex(int fd, int depth);

/**
 * Create a JSON object from an already opened file descriptor, using
 * the default maximum object depth. (JSON_TOKENER_DEFAULT_DEPTH)
 *
 * See json_object_from_fd_ex() for details.
 */
JSON_EXPORT struct json_object *json_object_from_fd(int fd);

/**
 * Equivalent to:
 *   json_object_to_file_ext(filename, obj, JSON_C_TO_STRING_PLAIN);
 *
 * Returns -1 if something fails.  See json_util_get_last_err() for details.
 */
JSON_EXPORT int json_object_to_file(const char *filename, struct json_object *obj);

/**
 * Open and truncate the given file, creating it if necessary, then
 * convert the json_object to a string and write it to the file.
 *
 * Returns -1 if something fails.  See json_util_get_last_err() for details.
 */
JSON_EXPORT int json_object_to_file_ext(const char *filename, struct json_object *obj, int flags);

/**
 * Convert the json_object to a string and write it to the file descriptor.
 * Handles partial writes and will keep writing until done, or an error
 * occurs.
 *
 * @param fd an open, writable file descriptor to write to
 * @param obj the object to serializer and write
 * @param flags flags to pass to json_object_to_json_string_ext()
 * @return -1 if something fails.  See json_util_get_last_err() for details.
 */
JSON_EXPORT int json_object_to_fd(int fd, struct json_object *obj, int flags);

/**
 * Return the last error from various json-c functions, including:
 * json_object_to_file{,_ext}, json_object_to_fd() or
 * json_object_from_{file,fd}, or NULL if there is none.
 */
JSON_EXPORT const char *json_util_get_last_err(void);

/**
 * A parsing helper for integer values.  Returns 0 on success,
 * with the parsed value assigned to *retval.  Overflow/underflow
 * are NOT considered errors, but errno will be set to ERANGE,
 * just like the strtol/strtoll functions do.
 */
JSON_EXPORT int json_parse_int64(const char *buf, int64_t *retval);
/**
 * A parsing help for integer values, providing one extra bit of 
 * magnitude beyond json_parse_int64().
 */
JSON_EXPORT int json_parse_uint64(const char *buf, uint64_t *retval);
/**
 * @deprecated
 */
JSON_EXPORT int json_parse_double(const char *buf, double *retval);

/**
 * Return a string describing the type of the object.
 * e.g. "int", or "object", etc...
 */
JSON_EXPORT const char *json_type_to_name(enum json_type o_type);

#ifdef __cplusplus
}
#endif

#endif
