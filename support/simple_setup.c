/*
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2009 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

extern const char *progname;

/*
 * __wt_single_thread_setup --
 *	Standard setup for single-threaded applications.
 */
int
__wt_single_thread_setup(WT_TOC **toc, DB **db)
{
	int ret;

	*toc = NULL;
	*db = NULL;

	if ((ret = wt_start(WT_SINGLE_THREADED)) != 0) {
		fprintf(stderr,
		    "%s: wt_start: %s\n", progname, wt_strerror(ret));
		return (ret);
	}
	if ((ret = wt_toc_create(toc, 0)) != 0) {
		fprintf(stderr,
		    "%s: wt_toc_create: %s\n", progname, wt_strerror(ret));
		return (ret);
	}
	if ((ret = wt_db_create(db, *toc, NULL, 0)) != 0) {
		fprintf(stderr,
		    "%s: wt_db_create: %s\n", progname, wt_strerror(ret));
		return (ret);
	}
	return (0);
}

/*
 * __wt_single_thread_teardown --
 *	Standard teardown for single-threaded applications.
 */
int
__wt_single_thread_teardown(WT_TOC *toc, DB *db)
{
	int ret, tret;

	ret = 0;
	if (db != NULL) {
		if ((tret = db->close(db, toc, 0)) != 0) {
			fprintf(stderr,
			    "%s: Db.close: %s\n", progname, wt_strerror(ret));
			if (ret == 0)
				ret = tret;
		}
		if ((tret = db->destroy(db, toc, 0)) != 0) {
			fprintf(stderr,
			    "%s: Db.destroy: %s\n", progname, wt_strerror(ret));
			if (ret == 0)
				ret = tret;
		}
	}
	if (toc != NULL)
		if ((tret = toc->destroy(toc, 0)) != 0) {
			fprintf(stderr,
			    "%s: Toc.destroy: %s\n",
			    progname, wt_strerror(ret));
			if (ret == 0)
				ret = tret;
		}
	if ((tret = wt_stop(0)) != 0) {
		fprintf(stderr,
		    "%s: wt_stop: %s\n", progname, wt_strerror(ret));
		if (ret == 0)
			ret = tret;
	}
	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
