/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wts.h"

void
bdb_setup(void)
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
	    (g.c_write_ops != 0 ? DB_INIT_LOCK : 0) |
	    DB_INIT_MPOOL | DB_PRIVATE, 0) == 0);
	assert(db_create(&db, dbenv, 0) == 0);
	assert(db->set_flags(db, DB_RECNUM) == 0);

	p = fname(BDB_PREFIX, "db");
	(void)remove(p);
	assert(db->open(db, NULL, p, NULL, DB_BTREE, DB_CREATE, 0) == 0);

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
	DBT key, data;
	DB *db;

	memset(&key, 0, sizeof(DBT));
	key.data = key_data;
	key.size = key_size;
	memset(&data, 0, sizeof(DBT));
	data.data = data_data;
	data.size = data_size;

	db = g.bdb_db;

	assert(db->put(db, NULL, &key, &data, 0) == 0);
}

int
bdb_read_key(void *key_data, u_int32_t key_size, void *datap, u_int32_t *sizep)
{
	DBT key, data;
	DB *db;
	int ret;

	memset(&key, 0, sizeof(DBT));
	key.data = key_data;
	key.size = key_size;
	memset(&data, 0, sizeof(DBT));

	db = g.bdb_db;

	if ((ret = db->get(db, NULL, &key, &data, 0)) != 0) {
		db->err(db, ret,
		    "bdb_read_key: {%.*s}", (int)key_size, (char *)key_data);
		return (1);
	}
	*(void **)datap = data.data;
	*sizep = data.size;
	return (0);
}

int
bdb_read_recno(u_int64_t arg_recno,
    void *keyp, u_int32_t *key_sizep, void *datap, u_int32_t *data_sizep)
{
	DBT key, data;
	DB *db;
	u_int32_t recno;
	int ret;

	memset(&key, 0, sizeof(DBT));
	recno = (u_int32_t)arg_recno;
	key.data = &recno;
	key.size = sizeof(recno);
	memset(&data, 0, sizeof(DBT));

	db = g.bdb_db;

	if ((ret = db->get(db, NULL, &key, &data, DB_SET_RECNO)) != 0) {
		db->err(db, ret, "bdb_read_recno: %llu", arg_recno);
		return (1);
	}
	*(void **)keyp = key.data;
	*key_sizep = key.size;
	*(void **)datap = data.data;
	*data_sizep = data.size;
	return (0);
}
