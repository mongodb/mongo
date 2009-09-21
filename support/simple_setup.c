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

static ENV *env;

/*
 * __wt_simple_setup --
 *	Standard setup for simple applications.
 */
int
__wt_simple_setup(
    const char *progname, int singlethread, WT_TOC **tocp, DB **dbp)
{
	DB *db;
	WT_TOC *toc;
	int ret;

	env = NULL;
	*tocp = toc = NULL;
	*dbp = db = NULL;

	if ((ret = wt_env_create(0, &env)) != 0) {
		fprintf(stderr,
		    "%s: wt_env_create: %s\n", progname, wt_strerror(ret));
		return (ret);
	}
	if ((ret = env->start(
	    env, singlethread ? WT_SINGLE_THREADED : 0)) != 0) {
		fprintf(stderr,
		    "%s: Env.start: %s\n", progname, wt_strerror(ret));
		goto err;
	}
	if ((ret = env->toc_create(env, 0, &toc)) != 0) {
		fprintf(stderr,
		    "%s: Env.toc_create: %s\n", progname, wt_strerror(ret));
		return (ret);
	}
	if ((ret = env->db_create(env, toc, 0, &db)) != 0) {
		fprintf(stderr,
		    "%s: Env.db_create: %s\n", progname, wt_strerror(ret));
		return (ret);
	}

	*tocp = toc;
	*dbp = db;
	return (EXIT_SUCCESS);

err:	(void)__wt_simple_teardown(progname, toc, db);
	return (EXIT_FAILURE);
}

/*
 * __wt_simple_teardown --
 *	Standard teardown for simple applications.
 */
int
__wt_simple_teardown(const char *progname, WT_TOC *toc, DB *db)
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
	if ((tret = env->stop(env, 0)) != 0) {
		fprintf(stderr,
		    "%s: Env.stop: %s\n", progname, wt_strerror(ret));
		if (ret == 0)
			ret = tret;
	}
	if ((tret = env->destroy(env, 0)) != 0) {
		fprintf(stderr,
		    "%s: Env.destroy: %s\n", progname, wt_strerror(ret));
		if (ret == 0)
			ret = tret;
	}
	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
