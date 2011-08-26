/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#define	BDB	1			/* Berkeley DB header files */
#include "format.h"

void
bdb_startup(void)
{
	DB *db;
	DBC *dbc;
	DB_ENV *dbenv;

	assert(db_env_create(&dbenv, 0) == 0);
	dbenv->set_errpfx(dbenv, "bdb");
	dbenv->set_errfile(dbenv, stderr);
	assert(dbenv->mutex_set_max(dbenv, 10000) == 0);
	assert(dbenv->set_cachesize(dbenv, 0, 50 * 1024 * 1024, 1) == 0);
	assert(dbenv->open(dbenv, NULL,
	    DB_CREATE |
	    (g.c_delete_pct == 0 && g.c_insert_pct == 0 && g.c_write_pct == 0 ?
	    0 : DB_INIT_LOCK) |
	    DB_INIT_MPOOL | DB_PRIVATE, 0) == 0);
	assert(db_create(&db, dbenv, 0) == 0);

	assert(db->open(db, NULL, "__bdb", NULL, DB_BTREE, DB_CREATE, 0) == 0);
	g.bdb = db;
	assert(db->cursor(db, NULL, &dbc, 0) == 0);
	g.dbc = dbc;
}

void
bdb_teardown(void)
{
	DB *db;
	DBC *dbc;
	DB_ENV *dbenv;

	dbc = g.dbc;
	db = g.bdb;
	dbenv = db->dbenv;
	assert(dbc->close(dbc) == 0);
	assert(db->close(db, 0) == 0);
	assert(dbenv->close(dbenv, 0) == 0);
}

void
bdb_insert(
    const void *key_data, uint32_t key_size,
    const void *value_data, uint32_t value_size)
{
	static DBT key, value;
	DBC *dbc;

	key.data = (void *)key_data;
	key.size = key_size;
	value.data = (void *)value_data;
	value.size = value_size;

	dbc = g.dbc;

	assert(dbc->put(dbc, &key, &value, DB_KEYFIRST) == 0);
}

int
bdb_np(int next,
    void *keyp, uint32_t *keysizep,
    void *valuep, uint32_t *valuesizep, int *notfoundp)
{
	static DBT key, value;
	DB *db = g.bdb;
	DBC *dbc = g.dbc;
	int ret;

	*notfoundp = 0;

	if ((ret =
	    dbc->get(dbc, &key, &value, next ? DB_NEXT : DB_PREV)) != 0) {
		if (ret == DB_NOTFOUND) {
			*notfoundp = 1;
			return (0);
		}
		db->err(db, ret,
		    "dbc->get: %s: {%.*s}",
		    next ? "DB_NEXT" : "DB_PREV",
		    (int)key.size, (char *)key.data);
		return (1);
	}
	*(void **)keyp = key.data;
	*keysizep = key.size;
	*(void **)valuep = value.data;
	*valuesizep = value.size;
	return (0);
}

int
bdb_read(uint64_t keyno, void *valuep, uint32_t *valuesizep, int *notfoundp)
{
	static DBT key, value;
	DB *db = g.bdb;
	DBC *dbc = g.dbc;
	int ret;

	*notfoundp = 0;

	key_gen(&key.data, &key.size, keyno, 0);

	if ((ret = dbc->get(dbc, &key, &value, DB_SET)) != 0) {
		if (ret == DB_NOTFOUND) {
			*notfoundp = 1;
			return (0);
		}
		db->err(db, ret,
		    "dbc->get: DB_SET: {%.*s}",
		    (int)key.size, (char *)key.data);
		return (1);
	}
	*(void **)valuep = value.data;
	*valuesizep = value.size;
	return (0);
}

int
bdb_put(const void *arg_key, uint32_t arg_key_size,
    const void *arg_value, uint32_t arg_value_size, int *notfoundp)
{
	static DBT key, value;
	DB *db = g.bdb;
	DBC *dbc = g.dbc;
	int ret;

	*notfoundp = 0;

	key.data = (void *)arg_key;
	key.size = arg_key_size;
	value.data = (void *)arg_value;
	value.size = arg_value_size;

	if ((ret = dbc->put(dbc, &key, &value, DB_KEYFIRST)) != 0) {
		if (ret == DB_NOTFOUND) {
			*notfoundp = 1;
			return (0);
		}
		db->err(db, ret, "dbc->put: DB_KEYFIRST: {%.*s}{%.*s}",
		    (int)key.size, (char *)key.data,
		    (int)value.size, (char *)value.data);
		return (1);
	}
	return (0);
}

int
bdb_del(uint64_t keyno, int *notfoundp)
{
	static DBT value;
	static DBT key;
	DB *db = g.bdb;
	DBC *dbc = g.dbc;
	int ret;

	*notfoundp = 0;

	key_gen(&key.data, &key.size, keyno, 0);

	if ((ret = bdb_read(keyno, &value.data, &value.size, notfoundp)) != 0)
		return (1);
	if (*notfoundp)
		return (0);
	if ((ret = dbc->del(dbc, 0)) != 0) {
		if (ret == DB_NOTFOUND) {
			*notfoundp = 1;
			return (0);
		}
		db->err(db, ret,
		    "dbc->del: {%.*s}", (int)key.size, (char *)key.data);
		return (1);
	}
	return (0);
}
