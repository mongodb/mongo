/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

#define	WT_DB_ERR(db, error, fmt) {					\
	va_list __ap;							\
									\
	/* Application-specified callback function. */			\
	va_start(__ap, fmt);						\
	if ((db)->errcall != NULL)					\
		__wt_msg_call((db)->errcall, (db), (db)->errpfx,	\
		    (db)->idb == NULL ? NULL : (db)->idb->dbname,	\
		    error, fmt, __ap);					\
	va_end(__ap);							\
									\
	/*								\
	 * If the application set an error callback function but not an	\
	 * error stream, we're done.  Otherwise, write an error	stream.	\
	 */								\
	if ((db)->errcall != NULL && (db)->errfile == NULL)		\
			return;						\
									\
	va_start(__ap, fmt);						\
	__wt_msg_stream((db)->errfile, (db)->errpfx,			\
	    (db)->idb == NULL ? NULL : (db)->idb->dbname,		\
	    error, fmt, __ap);						\
	va_end(__ap);							\
}

/*
 * __wt_db_err --
 *	Db.err method.
 */
void
__wt_db_err(DB *db, int error, const char *fmt, ...)
{
	WT_DB_ERR(db, error, fmt);
}

/*
 * __wt_db_errx --
 *	Db.errx method.
 */
void
__wt_db_errx(DB *db, const char *fmt, ...)
{
	WT_DB_ERR(db, 0, fmt);
}
