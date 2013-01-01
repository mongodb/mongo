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
 * This structure is used to avoid the need to link extension modules with the
 * library.
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
#define	wiredtiger_err_printf	wt_api->err_printf

	/*! Allocate short-term use scratch memory.
	 *
	 * @param session the session handle
	 * @param bytes the number of bytes of memory needed
	 * @returns A valid memory reference on success or NULL on error
	 */
	void *(*scr_alloc)(WT_SESSION *, size_t bytes);
#define	wiredtiger_scr_alloc	wt_api->scr_alloc

	/*! Free short-term use scratch memory.
	 *
	 * @param session the session handle
	 * @param ref a memory reference returned by WT_EXTENSION_API::scr_alloc
	 */
	void (*scr_free)(WT_SESSION *, void *ref);
#define	wiredtiger_scr_free	wt_api->scr_free
};

/*! This global variable must appear in each extension module. */
extern WT_EXTENSION_API	*wt_api;

/*! @} */

#if defined(__cplusplus)
}
#endif
#endif /* __WIREDTIGER_EXT_H_ */
