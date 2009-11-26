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

static ENV *__env;

/*
 * wiredtiger_simple_setup --
 *	Standard setup for simple applications.
 */
int
wiredtiger_simple_setup(const char *progname, DB **dbp)
{
	DB *db;
	ENV *env;
	int ret;

	db = *dbp = NULL;

	if ((ret = wiredtiger_env_init(&env, 0)) != 0) {
		fprintf(stderr,
		    "%s: wiredtiger_env_init: %s\n",
		    progname, wt_strerror(ret));
		return (ret);
	}
	__env = env;

	if ((ret = env->open(env, NULL, 0, 0)) != 0) {
		fprintf(stderr,
		    "%s: Env.open: %s\n", progname, wt_strerror(ret));
		goto err;
	}
	if ((ret = env->db(env, 0, &db)) != 0) {
		fprintf(stderr, "%s: Env.db: %s\n", progname, wt_strerror(ret));
err:		wiredtiger_simple_teardown(progname, db);
		return (ret);
	}

	*dbp = db;
	return (EXIT_SUCCESS);
}

/*
 * wiredtiger_simple_teardown --
 *	Standard teardown for simple applications.
 */
int
wiredtiger_simple_teardown(const char *progname, DB *db)
{
	int ret, tret;

	ret = 0;
	if (db != NULL && (tret = db->close(db, 0)) != 0) {
		fprintf(stderr,
		    "%s: Db.close: %s\n", progname, wt_strerror(ret));
		if (ret == 0)
			ret = tret;
	}

	if (__env != NULL) {
		if ((tret = __env->close(__env, 0)) != 0) {
			fprintf(stderr,
			    "%s: Env.close: %s\n", progname, wt_strerror(ret));
			if (ret == 0)
				ret = tret;
		}
		__env = NULL;
	}

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
