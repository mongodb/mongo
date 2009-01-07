/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_db_config_default(DB *);

/*
 * wt_db_create --
 *	DB constructor.
 */
int
wt_db_create(DB **dbp, ENV *env, u_int32_t flags)
{
	DB *db;
	IDB *idb;
	int ret;

	db = NULL;
	idb = NULL;

	/*
	 * !!!
	 * We may not have been passed a valid ENV structure -- get one
	 * before doing anything else.
	 */
	if (env == NULL) {
		if ((ret = wt_env_create(&env, 0)) != 0)
			return (ret);
		F_SET(env, WT_PRIVATE_ENV);
		if ((ret = env->open(env, NULL, 0, 0)) != 0)
			goto err;
	}

	/*
	 * !!!
	 * Get valid DB and IDB structures next, then everything should
	 * work.
	 */
	if ((ret = __wt_calloc(env, 1, sizeof(DB), &db)) != 0 ||
	    (ret = __wt_calloc(env, 1, sizeof(IDB), &idb)) != 0)
		goto err;

	/* Connect everything together. */
	db->idb = idb;
	idb->db = db;
	db->env = env;
	db->ienv = env->ienv;
	TAILQ_INSERT_TAIL(&env->dbqh, db, q);

	DB_FLAG_CHK_NOTFATAL(
	    db, "wt_db_create", flags, WT_APIMASK_WT_DB_CREATE, ret);
	if (ret != 0)
		goto err;

	__wt_db_config_methods(db);

	if ((ret = __wt_stat_alloc_db(env, &db->stats)) != 0)
		goto err;
	if ((ret = __wt_db_config_default(db)) != 0)
		goto err;

	*dbp = db;
	return (0);

err:	if (idb != NULL)
		__wt_free(env, idb);
	if (db != NULL)
		__wt_free(env, db);
	if (env != NULL && F_ISSET(env, WT_PRIVATE_ENV))
		(void)env->destroy(env, 0);
	return (ret);
}

/*
 * __wt_db_destroy --
 *	Db.destroy method (DB destructor).
 */
int
__wt_db_destroy(DB *db, u_int32_t flags)
{
	ENV *env;
	int is_private, ret;

	env = db->env;
	ret = 0;

	DB_FLAG_CHK_NOTFATAL(
	    db, "Db.destroy", flags, WT_APIMASK_DB_DESTROY, ret);

	/* We have to destroy the environment too, if it was private. */
	is_private = F_ISSET(db, WT_PRIVATE_ENV);

	/* Discard the underlying IDB structure. */
	__wt_idb_destroy(db, 0);

	/* Free any allocated memory. */
	__wt_free(env, db->stats);

	/* Disconnect from the list. */
	TAILQ_REMOVE(&env->dbqh, db, q);

	/* Free the DB structure. */
	memset(db, OVERWRITE_BYTE, sizeof(db));
	__wt_free(env, db);

	if (is_private)
		(void)env->destroy(env, 0);

	return (ret);
}

/*
 * __wt_idb_destroy --
 *	Destroy the DB's underlying IDB structure.
 */
void
__wt_idb_destroy(DB *db, int refresh)
{
	ENV *env;
	IDB *idb;

	env = db->env;
	idb = db->idb;

	/* Free the actual structure. */
	__wt_free(env, db->idb);
	db->idb = NULL;

	if (!refresh)
		return;

	/*
	 * Allocate a new IDB structure on request.
	 *
	 * This is the guts of the split between the public/private, DB/IDB
	 * handles.  If a Db.open fails for any reason, the user may use the
	 * DB structure again, but the IDB structure may have been modified
	 * in the attempt.  So, we swap out the IDB structure for a new one.
	 * This requires three things:
	 *
	 * 1)	the IDB structure is never touched by any DB configuration,
	 *	we'd lose it here;
	 * 2)	if this fails for any reason, there's no way back, kill the
	 *	DB handle itself;
	 * 3)	our caller can't depend on an IDB handle existing after we
	 *	return, so this only gets called in a few, nasty error paths,
	 *	immediately before returning to the user.
	 */
	if (__wt_calloc(env, 1, sizeof(IDB), &idb) != 0)
		__wt_db_config_methods_lockout(db);
	else {
		db->idb = idb;
		idb->db = db;
	}
}

/*
 * __wt_db_config_default --
 *	Set default configuration for a just-created DB handle.
 */
static int
__wt_db_config_default(DB *db)
{
	int ret;

	if ((ret = db->set_pagesize(db,
	    WT_PAGE_DEFAULT_SIZE, WT_FRAG_DEFAULT_SIZE, 0, 0)) != 0)
		return (ret);

	db->btree_compare = db->dup_compare = __wt_bt_lex_compare;

	return (0);
}

int
__wt_db_lockout_err(DB *db)
{
	__wt_db_errx(db,
	    "This Db handle has failed for some reason, and can no longer "
	    "be used; the only method permitted on it is Db.destroy");
	return (WT_ERROR);
}

int
__wt_db_lockout_open(DB *db)
{
	__wt_db_errx(db,
	    "This method may not be called until after the Db.open method has "
	    "been called");
	return (WT_ERROR);
}
