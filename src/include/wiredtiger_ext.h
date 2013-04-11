/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifndef	__WIREDTIGER_EXT_H_
#define	__WIREDTIGER_EXT_H_

#include <wiredtiger.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*! The configuration information returned by the WiredTiger extension function
 * WT_EXTENSION_API::config.
 */
struct __wt_extension_config {
	/*! The value of a configuration string.
	 *
	 * Regardless of the type of the configuration string (boolean, int,
	 * list or string), the \c str field will reference the value of the
	 * configuration string.
	 *
	 * The bytes referenced by \c str may <b>not</b> be nul-terminated,
	 * use the \c len field instead of a terminating nul byte.
	 */
	const char *str;

	/*! The number of bytes in the value referenced by \c str. */
	size_t len;

	/*! The value of a configuration boolean or integer.
	 *
	 * If the configuration string's value is "true" or "false", the
	 * \c value field will be set to 1 (true), or 0 (false).
	 *
	 * If the configuration string can be legally interpreted as an integer,
	 * using the strtoll function rules as specified in ISO/IEC 9899:1990
	 * ("ISO C90"), that integer will be stored in the \c value field.
	 */
	int64_t value;

	/*! The value of a configuration list.
	 *
	 * If the configuration string type is of type list, the \c argv field
	 * will reference a NULL-terminated array of pointers to nul-terminated
	 * strings.
	 *
	 * The argv array, and each string it references, will be returned in
	 * allocated memory; both the strings and the array must be freed by
	 * the application to avoid memory leaks.
	 */
	 char **argv;
};

/*! @addtogroup wt_ext
 * @{
 */

/*! Table of WiredTiger extension functions.
 *
 * This structure is used to provide a set of WiredTiger functions to extension
 * modules without needing to link the modules with the WiredTiger library, for
 * example, a compression module configured using the
 * WT_CONNECTION::add_compressor method.
 *
 * To use these functions in extension modules not linked with the WiredTiger
 * library:
 * - include the wiredtiger_ext.h header file,
 * - declare a variable which references a WT_EXTENSION_API structure, and
 * - initialize the variable using the argument passed to the module's
 * initialization function.
 *
 * The following code is from the sample compression module:
 *
 * @snippet nop_compress.c WT_EXTENSION_API declaration
 * @snippet nop_compress.c WT_EXTENSION_API initialization
 *
 * The extension functions may also be used by modules that are linked with
 * the WiredTiger library, for example, a data source configured using the
 * WT_CONNECTION::add_data_source method.
 *
 * To use these functions in extension modules linked with the WiredTiger
 * library:
 * - include the wiredtiger_ext.h header file,
 * - declare a variable which references a WT_EXTENSION_API structure, and
 * - call the wiredtiger_extension_api function to initialize that variable.
 *
 * For example:
 *
 * @snippet ex_data_source.c WT_EXTENSION_API declaration
 */
struct __wt_extension_api {
/* !!! To maintain backwards compatibility, this structure is append-only. */
	/*! Insert an error message into the WiredTiger error stream.
	 *
	 * @param session the session handle
	 * @param fmt a printf-like format specification
	 * @errors
	 *
	 * @snippet ex_data_source.c WT_EXTENSION_API err_printf
	 */
	int (*err_printf)(WT_SESSION *session, const char *fmt, ...);

	/*! Allocate short-term use scratch memory.
	 *
	 * @param session the session handle
	 * @param bytes the number of bytes of memory needed
	 * @returns A valid memory reference on success or NULL on error
	 *
	 * @snippet ex_data_source.c WT_EXTENSION_API scr_alloc
	 */
	void *(*scr_alloc)(WT_SESSION *session, size_t bytes);

	/*! Free short-term use scratch memory.
	 *
	 * @param session the session handle
	 * @param ref a memory reference returned by WT_EXTENSION_API::scr_alloc
	 *
	 * @snippet ex_data_source.c WT_EXTENSION_API scr_free
	 */
	void (*scr_free)(WT_SESSION *session, void *ref);

	/*! Insert a message into the WiredTiger message stream.
	 *
	 * @param session the session handle
	 * @param fmt a printf-like format specification
	 * @errors
	 *
	 * @snippet ex_data_source.c WT_EXTENSION_API msg_printf
	 */
	int (*msg_printf)(WT_SESSION *session, const char *fmt, ...);

	/*! Return the value of a configuration string.
	 *
	 * @param session the session handle
	 * @param key configuration key string
	 * @param config the configuration information passed to a
	 * WT_DATA_SOURCE:: method
	 * @param value the returned value
	 * @errors
	 *
	 * @snippet ex_data_source.c WT_EXTENSION_CONFIG string
	 */
	int (*config)(WT_SESSION *session,
	    const char *key, void *config, WT_EXTENSION_CONFIG *value);
};

/*! @} */

#if defined(__cplusplus)
}
#endif
#endif /* __WIREDTIGER_EXT_H_ */
