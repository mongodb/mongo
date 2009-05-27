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
static int __wt_db_destroy_int(WT_TOC *, u_int32_t);
static int __wt_idb_config_default(DB *);

/*
 * wt_db_create --
 *	DB constructor.
 */
int
__wt_env_db_create(WT_TOC *toc)
{
	wt_args_env_db_create_unpack;
	DB *db;
	IDB *idb;
	int ret;

	db = NULL;
	idb = NULL;

	/*
	 * !!!
	 * Get valid DB and IDB structures next, then everything should
	 * work.
	 */
	if ((ret = __wt_calloc(env, 1, sizeof(DB), &db)) != 0 ||
	    (ret = __wt_calloc(env, 1, sizeof(IDB), &idb)) != 0)
		goto err;

	/* Connect everything together. */
	toc->db = db;
	db->idb = idb;
	idb->db = db;
	db->env = env;
	db->ienv = env->ienv;

	/* We have an environment -- check the API flags. */
	WT_DB_FCHK_NOTFATAL(
	    db, "wt_db_create", flags, WT_APIMASK_WT_DB_CREATE, ret);
	if (ret != 0)
		goto err;

	/* Configure the DB and the IDB. */
	if ((ret = __wt_db_config_default(db)) != 0)
		goto err;
	if ((ret = __wt_idb_config_default(db)) != 0)
		goto err;

	*dbp = db;
	return (0);

err:	(void)__wt_db_destroy_int(toc, 0);
	return (ret);
}

/*
 * __wt_db_destroy --
 *	Db.destroy method (DB destructor).
 */
int
__wt_db_destroy(WT_TOC *toc)
{
	wt_args_db_destroy_unpack;

	return (__wt_db_destroy_int(toc, flags));
}

/*
 * __wt_db_destroy_int --
 *	Db.destroy method (DB destructor), internal version.
 */
static int
__wt_db_destroy_int(WT_TOC *toc, u_int32_t flags)
{
	DB *db;
	ENV *env;
	int ret, tret;

	db = toc->db;
	env = toc->env;
	ret = 0;

	WT_DB_FCHK_NOTFATAL(
	    db, "Db.destroy", flags, WT_APIMASK_DB_DESTROY, ret);

	/* Discard the underlying IDB structure. */
	ret = __wt_idb_destroy(db, 0);

	/* Free any allocated memory. */
	WT_FREE_AND_CLEAR(env, db->hstats);
	WT_FREE_AND_CLEAR(env, db->dstats);

	/* Free the DB structure. */
	memset(db, OVERWRITE_BYTE, sizeof(db));
	__wt_free(env, db);

	return (ret);
}

/*
 * __wt_db_config_default --
 *	Set default configuration for a just-created DB handle.
 */
static int
__wt_db_config_default(DB *db)
{
	ENV *env;
	int ret;

	env = db->env;

	__wt_db_config_methods(db);

	db->btree_compare = db->btree_dup_compare = __wt_bt_lex_compare;

	if ((ret = __wt_stat_alloc_db_hstats(env, &db->hstats)) != 0)
		return (ret);
	if ((ret = __wt_stat_alloc_db_dstats(env, &db->dstats)) != 0)
		return (ret);

	return (0);
}

/*
 * __wt_idb_destroy --
 *	Destroy the DB's underlying IDB structure.
 */
int
__wt_idb_destroy(DB *db, int refresh)
{
	ENV *env;
	IDB *idb;
	int ret;

	env = db->env;
	idb = db->idb;

	/* Check that there's something to destroy. */
	if (idb == NULL)
		return (0);

	/* Free allocated memory. */
	WT_FREE_AND_CLEAR(env, idb->key.data);
	WT_FREE_AND_CLEAR(env, idb->data.data);
	WT_FREE_AND_CLEAR(env, idb->dbname);

	/* If we're truly done, discard the actual memory. */
	if (!refresh) {
		__wt_free(env, idb);
		db->idb = NULL;
		return (0);
	}

	/*
	 * This is the guts of the split between the public/private, DB/IDB
	 * handles.  If a Db.open fails for any reason, the user may use the
	 * DB structure again, but the IDB structure may have been modified
	 * in the attempt.  So, we overwrite the IDB structure, as if it was
	 * just allocated.  This requires the IDB structure never be modified
	 * by DB configuration, we'd lose that configuration here.
	 */
	memset(idb, 0, sizeof(idb));
	if ((ret = __wt_idb_config_default(db)) != 0)
		return (ret);

	return (0);
}

/*
 * __wt_idb_config_default --
 *	Set default configuration for a just-created IDB handle.
 */
static int
__wt_idb_config_default(DB *db)
{
	IDB *idb;

	idb = db->idb;

	WT_CLEAR(idb->key);
	WT_CLEAR(idb->data);

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
