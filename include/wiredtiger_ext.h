/* Copyright (c) 2010 WiredTiger, Inc.  All rights reserved. */

/* vim: set filetype=c.doxygen : */

#ifndef _WIREDTIGER_EXT_H_
#define _WIREDTIGER_EXT_H_

/*! @defgroup wt_ext WiredTiger Extension API
 * The functions and interfaces that applications use to customize and extend
 * the behavior of WiredTiger.
 *
 * @{
 */

#include "wiredtiger.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The interface implemented by applications to provide custom ordering of
 * records.  Applications register their implementation with WiredTiger by
 * calling WT_CONNECTION::add_collator.
 */
struct WT_COLLATOR {
	/*! Callback to compare keys or order duplicate values.
	 *
	 * @returns -1 if <code>value1 < value2</code>,
	 * 	     0 if <code>value1 == value2</code>,
	 * 	     1 if <code>value1 > value2</code>.
	 */
	int (*compare)(WT_SESSION *session, WT_COLLATOR *collator,
	    const WT_ITEM *value1, const WT_ITEM *value2);
};

/*!
 * Applications can extend WiredTiger by providing new implementation of the
 * WT_CURSOR class.  This is done by implementing the WT_CURSOR_FACTORY
 * interface, then calling WT_CONNECTION::add_cursor_factory.
 *
 * <b>Thread safety:</b> WiredTiger may invoke methods on the WT_CURSOR_FACTORY
 * interface from multiple threads concurrently.  It is the responsibility of
 * the implementation to protect any shared data.
 */
struct WT_CURSOR_FACTORY {
	/*! Callback to determine how much space to allocate for a cursor.
	 *
	 * If the callback is NULL, no additional space is allocated in the
	 * WT_CURSOR implementation.
	 *
	 * @errors
	 */
	int (*cursor_size)(WT_CURSOR_FACTORY *factory,
	    const char *obj, size_t *sizep);

	/*! Callback to initialize a cursor. */
	int (*init_cursor)(WT_CURSOR_FACTORY *factory,
	    WT_SESSION *session, const char *obj, WT_CURSOR *cursor);

	/*! Callback to duplicate a cursor.
	 *
	 * @errors
	 */
	int (*dup_cursor)(WT_CURSOR_FACTORY *factory,
	    WT_SESSION *session, WT_CURSOR *old_cursor, WT_CURSOR *new_cursor);
};

/*!
 * The interface implemented by applications in order to handle errors.
 */
struct WT_ERROR_HANDLER {
	/*! Callback to handle errors within the session. */
	int (*handle_error)(WT_ERROR_HANDLER *handler,
	    int err, const char *errmsg);

	/*! Optional callback to retrieve buffered messages. */
	int (*get_messages)(WT_ERROR_HANDLER *handler, const char **errmsgp);

	/*! Optional callback to clear buffered messages. */
	int (*clear_messages)(WT_ERROR_HANDLER *handler);
};

/*!
 * The interface implemented by applications to provide custom extraction of
 * index keys or column set values.  Applications register their implementation
 * with WiredTiger by calling WT_CONNECTION::add_extractor.
 */
struct WT_EXTRACTOR {
	/*! Callback to extract a value for an index or column set.
	 *
	 * @errors
	 */
	int (*extract)(WT_SESSION *session, WT_EXTRACTOR *extractor,
	    const WT_ITEM *key, const WT_ITEM *value, WT_ITEM *result);
};

/*! Entry point to an extension, implemented by loadable modules.
 *
 * @param connection the connection handle
 * @configempty
 * @errors
 */
extern int wiredtiger_extension_init(WT_CONNECTION *connection,
    const char *config);

#ifdef __cplusplus
}
#endif

/*! @} */

#endif /* _WIREDTIGER_EXT_H_ */
