/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
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
struct wt_extension_api {
/* !!! To maintain backwards compatibility, this structure is append-only. */
	/*! Put an error message on the WiredTiger error stream.  */
	void (*err_printf)(WT_SESSION *, const char *fmt, ...);
#define	wiredtiger_err_printf	wt_api->err_printf

	/*! Allocate short-term use scratch memory. */
	void *(*scr_alloc)(WT_SESSION *, size_t);
#define	wiredtiger_scr_alloc	wt_api->scr_alloc

	/*! Free short-term use scratch memory. */
	void (*scr_free)(WT_SESSION *, void *);
#define	wiredtiger_scr_free	wt_api->scr_free
};

/*! This global variable must appear in each extension module. */
extern WT_EXTENSION_API	*wt_api;

/*! @} */

#if defined(__cplusplus)
}
#endif
#endif /* __WIREDTIGER_EXT_H_ */
