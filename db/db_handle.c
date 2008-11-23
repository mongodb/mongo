/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * wt_db_create --
 *	DB constructor.
 */
wt_db_create(DB **dbp, ENV *env, u_int32_t flags)
{
	IENV *ienv;
	DB *db;
	IDB *idb;
	int ret;

	/*
	 * !!!
	 * We may not have been passed a valid IENV structure -- get one
	 * before doing anything else.
	 */
	if (env == NULL) {
		if ((ret = wt_env_create(&env, 0)) != 0)
			return (ret);
		F_SET(env, WT_ENV_IS_PRIVATE);
	}
	ienv = env->ienv;

	API_FLAG_CHK(ienv, "db_create", flags, WT_APIMASK_WT_DB_CREATE);

	if ((ret = __wt_calloc(ienv, 1, sizeof(IDB), &idb)) != 0)
		return (ret);
	if ((ret = __wt_calloc(ienv, 1, sizeof(DB), &db)) != 0) {
		__wt_free(NULL, idb);
		return (ret);
	}

	/* Connect everything together. */
	db->idb = idb;
	idb->db = db;
	db->env = env;
	db->ienv = ienv;

	/* Initialize handle methods. */
	db->destroy = __wt_db_destroy;
	db->get_errcall = __wt_db_get_errcall;
	db->get_errfile = __wt_db_get_errfile;
	db->get_errpfx = __wt_db_get_errpfx;
	db->set_errcall = __wt_db_set_errcall;
	db->set_errfile = __wt_db_set_errfile;
	db->set_errpfx = __wt_db_set_errpfx;

	*dbp = db;
	return (0);
}

/*
 * __wt_db_destroy --
 *	DB->destroy method (DB destructor).
 */
int
__wt_db_destroy(DB *db, u_int32_t flags)
{
	ENV *env;
	IENV *ienv;
	int is_private;

	env = db->env;
	ienv = env->ienv;

	API_FLAG_CHK_NOTFATAL(ienv, "Db.destroy", flags, WT_APIMASK_DB_DESTROY);

	/* We have to destroy the environment too, if it was private. */
	is_private = F_ISSET(db, WT_ENV_IS_PRIVATE);

	__wt_free(ienv, db->idb);
	__wt_free(ienv, db);

	if (is_private)
		(void)env->destroy(env, 0);

	return (0);
}

/*
 * __wt_db_get_errcall --
 *	DB->get_errcall.
 */
void
__wt_db_get_errcall(DB *db, void (**cbp)(const DB *, const char *))
{
	*cbp = db->errcall;
}

/*
 * __wt_db_set_errcall --
 *	DB->set_errcall.
 */
void
__wt_db_set_errcall(DB *db, void (*cb)(const DB *, const char *))
{
	db->errcall = cb;
}

/*
 * __wt_db_get_errfile --
 *	DB->get_errfile.
 */
void
__wt_db_get_errfile(DB *db, FILE **fpp)
{
	*fpp = db->errfile;
}

/*
 * __wt_db_set_errfile --
 *	DB->set_errfile.
 */
void
__wt_db_set_errfile(DB *db, FILE *fp)
{
	db->errfile = fp;
}

/*
 * __wt_db_get_errpfx --
 *	DB->get_errpfx.
 */
void
__wt_db_get_errpfx(DB *db, const char **pfxp)
{
	*pfxp = db->errpfx;
}

/*
 * __wt_db_set_errpfx --
 *	DB->set_errpfx.
 */
void
__wt_db_set_errpfx(DB *db, const char *pfx)
{
	db->errpfx = pfx;
}

