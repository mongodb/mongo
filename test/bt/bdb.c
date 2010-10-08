/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#define	BDB	1			/* Berkeley DB header files */
#include "wts.h"

void
bdb_setup(int reopen)
{
	DB *db;
	DB_ENV *dbenv;
	char *p;

	assert(db_env_create(&dbenv, 0) == 0);
	dbenv->set_errpfx(dbenv, "bdb");
	dbenv->set_errfile(dbenv, stderr);
	assert(dbenv->mutex_set_max(dbenv, 10000) == 0);
	assert(dbenv->set_cachesize(dbenv, 0, 50 * 1024 * 1024, 1) == 0);
	assert(dbenv->open(dbenv, NULL,
	    DB_CREATE |
	    (g.c_read_pct == 100 ? 0 : DB_INIT_LOCK) |
	    DB_INIT_MPOOL | DB_PRIVATE, 0) == 0);
	assert(db_create(&db, dbenv, 0) == 0);
	if (g.c_duplicates_pct)
		assert(db->set_flags(db, DB_DUP) == 0);

	p = fname(BDB_PREFIX, "db");
	if (!reopen)
		(void)remove(p);
	assert(db->open(db, NULL,
	    p, NULL, DB_BTREE, reopen ? 0 : DB_CREATE, 0) == 0);

	g.bdb_db = db;
}

void
bdb_teardown(void)
{
	DB *db;
	DB_ENV *dbenv;

	db = g.bdb_db;
	dbenv = db->dbenv;

	assert(db->close(db, DB_NOSYNC) == 0);
	assert(dbenv->close(dbenv, 0) == 0);
}

void
bdb_insert(
    void *key_data, u_int32_t key_size, void *data_data, u_int32_t data_size)
{
	static DBT key, data;
	DB *db;

	key.data = key_data;
	key.size = key_size;
	data.data = data_data;
	data.size = data_size;

	db = g.bdb_db;

	assert(db->put(db, NULL, &key, &data, 0) == 0);
}

int
bdb_read(u_int64_t keyno, void *datap, u_int32_t *sizep, int *notfoundp)
{
	static DBT key, data;
	DB *db;
	int ret;

	db = g.bdb_db;
	*notfoundp = 0;

	key_gen(&key, keyno);

	if ((ret = db->get(db, NULL, &key, &data, 0)) != 0) {
		if (ret == DB_NOTFOUND) {
			*notfoundp = 1;
			return (0);
		}
		db->err(db, ret,
		    "bdb_read_key: {%.*s}", (int)key.size, (char *)key.data);
		return (1);
	}
	*(void **)datap = data.data;
	*sizep = data.size;
	return (0);
}

int
bdb_put(u_int64_t keyno, void *arg_data, u_int32_t arg_size, int *notfoundp)
{
	static DBT key, data;
	DB *db;
	int ret;

	db = g.bdb_db;
	*notfoundp = 0;

	key_gen(&key, keyno);
	data.data = arg_data;
	data.size = arg_size;

	if ((ret = db->put(db, NULL, &key, &data, 0)) != 0) {
		if (ret == DB_NOTFOUND) {
			*notfoundp = 1;
			return (0);
		}
		db->err(db, ret, "bdb_put: {%.*s}{%.*s}",
		    (int)key.size, (char *)key.data,
		    (int)data.size, (char *)data.data);
		return (1);
	}
	return (0);
}

int
bdb_del(u_int64_t keyno, int *notfoundp)
{
	static DBT key;
	DB *db;
	int ret;

	db = g.bdb_db;
	*notfoundp = 0;

	key_gen(&key, keyno);

	if ((ret = db->del(db, NULL, &key, 0)) != 0) {
		if (ret == DB_NOTFOUND) {
			*notfoundp = 1;
			return (0);
		}
		db->err(db, ret,
		    "bdb_del: {%.*s}", (int)key.size, (char *)key.data);
		return (1);
	}
	return (0);
}
