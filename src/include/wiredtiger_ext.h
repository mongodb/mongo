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
 * @snippet nop_compress.c Declare WT_EXTENSION_API
 * @snippet nop_compress.c Initialize WT_EXTENSION_API
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
 * @snippet ex_all.c Declare WT_EXTENSION_API
 * @snippet ex_all.c Initialize WT_EXTENSION_API
 */
struct __wt_extension_api {
/* !!! To maintain backwards compatibility, this structure is append-only. */
	/*! Insert an error message into the WiredTiger error stream.
	 *
	 * @param session the session handle
	 * @param fmt a printf-like format specification
	 * @errors
	 */
	int (*err_printf)(WT_SESSION *, const char *fmt, ...);

	/*! Allocate short-term use scratch memory.
	 *
	 * @param session the session handle
	 * @param bytes the number of bytes of memory needed
	 * @returns A valid memory reference on success or NULL on error
	 */
	void *(*scr_alloc)(WT_SESSION *, size_t bytes);

	/*! Free short-term use scratch memory.
	 *
	 * @param session the session handle
	 * @param ref a memory reference returned by WT_EXTENSION_API::scr_alloc
	 */
	void (*scr_free)(WT_SESSION *, void *ref);

	/*! Insert a message into the WiredTiger message stream.
	 *
	 * @param session the session handle
	 * @param fmt a printf-like format specification
	 * @errors
	 */
	int (*msg_printf)(WT_SESSION *, const char *fmt, ...);
};

/*! @} */

#if defined(__cplusplus)
}
#endif
#endif /* __WIREDTIGER_EXT_H_ */
