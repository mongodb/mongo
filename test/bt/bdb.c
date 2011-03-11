/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#define	BDB	1			/* Berkeley DB header files */
#include "wts.h"

void
bdb_startup(void)
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
	    (g.c_delete_pct == 0 && g.c_insert_pct == 0 && g.c_write_pct == 0 ?
	    0 : DB_INIT_LOCK) |
	    DB_INIT_MPOOL | DB_PRIVATE, 0) == 0);
	assert(db_create(&db, dbenv, 0) == 0);

	p = fname("bdb");
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

	assert(db->close(db, 0) == 0);
	assert(dbenv->close(dbenv, 0) == 0);
}

void
bdb_insert(
    const void *key_data, u_int32_t key_size,
    const void *value_data, u_int32_t value_size)
{
	static DBT key, value;
	DB *db;

	key.data = (void *)key_data;
	key.size = key_size;
	value.data = (void *)value_data;
	value.size = value_size;

	db = g.bdb_db;

	assert(db->put(db, NULL, &key, &value, 0) == 0);
}

int
bdb_read(uint64_t keyno, void *valuep, uint32_t *sizep, int *notfoundp)
{
	static DBT key, value;
	DB *db;
	int ret;

	db = g.bdb_db;
	*notfoundp = 0;

	key_gen(&key.data, &key.size, keyno, 0);

	if ((ret = db->get(db, NULL, &key, &value, 0)) != 0) {
		if (ret == DB_NOTFOUND) {
			*notfoundp = 1;
			return (0);
		}
		db->err(db, ret,
		    "bdb_read_key: {%.*s}", (int)key.size, (char *)key.data);
		return (1);
	}
	*(void **)valuep = value.data;
	*sizep = value.size;
	return (0);
}

int
bdb_put(const void *arg_key, uint32_t arg_key_size,
    const void *arg_value, uint32_t arg_value_size, int *notfoundp)
{
	static DBT key, value;
	DB *db;
	int ret;

	db = g.bdb_db;
	*notfoundp = 0;

	key.data = (void *)arg_key;
	key.size = arg_key_size;
	value.data = (void *)arg_value;
	value.size = arg_value_size;

	if ((ret = db->put(db, NULL, &key, &value, 0)) != 0) {
		if (ret == DB_NOTFOUND) {
			*notfoundp = 1;
			return (0);
		}
		db->err(db, ret, "bdb_put: {%.*s}{%.*s}",
		    (int)key.size, (char *)key.data,
		    (int)value.size, (char *)value.data);
		return (1);
	}
	return (0);
}

int
bdb_del(uint64_t keyno, int *notfoundp)
{
	static DBT key;
	DB *db;
	int ret;

	db = g.bdb_db;
	*notfoundp = 0;

	key_gen(&key.data, &key.size, keyno, 0);

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
