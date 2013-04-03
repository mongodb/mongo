/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/stat.h>
#include <sys/time.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <build_unix/db.h>
#include <wiredtiger.h>

typedef struct __data_source DATA_SOURCE;	/* XXX: May not need? */
struct __data_source {
	WT_DATA_SOURCE dsrc;			/* Must come first */
};

typedef struct __cursor_source CURSOR_SOURCE;
struct __cursor_source {
	WT_CURSOR cursor;			/* Must come first */

	DB	*db;
	DBC	*dbc;
	DBT	 key, value;

	int	 append;			/* DB_APPEND flag */
	int	 recno;				/* DB_RECNO type */
};

static DATA_SOURCE ds;
static DB_ENV *dbenv;
static pthread_rwlock_t rwlock;
static int open_cursors = 0;			/* Cursor count */

static const char *
kvs_error_map(int v)
{
	/* Berkeley DB owns errors between -30,800 and -30,999. */
	return (v >= -30999 && v <= -30800 ?
	    db_strerror(v) : wiredtiger_strerror(v));
}

void	 die(int, const char *, ...);
#define	ERR(f) do {							\
	int __ret = (f);						\
	if (__ret != 0)							\
		die(0, "%s: %s\n", #f, kvs_error_map(__ret));		\
} while (0)

static void
lock(void)
{
	ERR(pthread_rwlock_trywrlock(&rwlock));
}

static void
unlock(void)
{
	ERR(pthread_rwlock_unlock(&rwlock));
}

static const char *
uri2name(const char *uri)
{
	const char *name;

	if ((name = strchr(uri, ':')) == NULL)
		return (NULL);
	if (*++name == '\0')
		return (NULL);
	return (name);
}

static int
cfg_parse_bool(const char *cfg[], const char *match, int *valuep)
{
	const char *p;

	*valuep = 0;
	for (; (p = *cfg) != NULL; ++cfg)
		if ((p = strstr(p, match)) != NULL && p[strlen(match)] == '=') {
			*valuep = p[strlen(match) + 1] == '1' ? 1 : 0;
			return (0);
		}
	return (0);
}

static int
cfg_parse_str(const char *cfg[], const char *match, char **valuep)
{
	char *t;
	const char *p;

	*valuep = NULL;
	for (; (p = *cfg) != NULL; ++cfg)
		if ((p = strstr(p, match)) != NULL && p[strlen(match)] == '=') {
			/* Copy the configuration string's value. */
			p += strlen(match) + 1;
			if ((*valuep = strdup(p)) == NULL)
				return (ENOMEM);

			/* nul-terminate the string's value. */
			for (t = *valuep; *t != '\0' && *t != ','; ++t)
				;
			*t = '\0';
			return (0);
		}
	return (0);
}

static void
bdb_dump(WT_CURSOR *wt_cursor, const char *tag)
{
	CURSOR_SOURCE *cursor;
	DB *db;
	DBC *dbc;
	DBT *key, *value;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	db = cursor->db;
	key = &cursor->key;
	value = &cursor->value;

	ERR(db->cursor(db, NULL, &dbc, 0));  

	printf("==> %s\n", tag);
	while (dbc->get(dbc, key, value, DB_NEXT) == 0)
		if (cursor->recno)
			printf("\t%llu/%.*s\n",
			    (unsigned long long)*(db_recno_t *)key->data,
			    (int)value->size, (char *)value->data);
		else
			printf("\t%.*s/%.*s\n",
			    (int)key->size, (char *)key->data,
			    (int)value->size, (char *)value->data);
}

static int
kvs_cursor_next(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = dbc->get(dbc, key, value, DB_NEXT)) == 0)  {
		if (cursor->recno)
			wt_cursor->recno = *(db_recno_t *)key->data;
		else {
			wt_cursor->key.data = key->data;
			wt_cursor->key.size = key->size;
		}
		wt_cursor->value.data = value->data;
		wt_cursor->value.size = value->size;
	}
	return (ret == DB_NOTFOUND ? WT_NOTFOUND : ret);
}

static int
kvs_cursor_prev(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = dbc->get(dbc, key, value, DB_PREV)) == 0)  {
		if (cursor->recno)
			wt_cursor->recno = *(db_recno_t *)key->data;
		else {
			wt_cursor->key.data = key->data;
			wt_cursor->key.size = key->size;
		}
		wt_cursor->value.data = value->data;
		wt_cursor->value.size = value->size;
	}
	return (ret == DB_NOTFOUND ? WT_NOTFOUND : ret);
}

static int
kvs_cursor_reset(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DB *db;
	DBC *dbc;

	cursor = (CURSOR_SOURCE *)wt_cursor;

	/* Close and re-open the Berkeley DB cursor. */
	db = cursor->db;
	dbc = cursor->dbc;
	if (dbc != NULL) {
		ERR(dbc->close(dbc));
		ERR(db->cursor(db, NULL, &cursor->dbc, 0));
	}
	return (0);
}

static int
kvs_cursor_search(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	db_recno_t recno;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if (cursor->recno) {
		recno = (uint32_t)wt_cursor->recno;
		key->data = &recno;
		key->size = sizeof(db_recno_t);
	} else {
		key->data = (char *)wt_cursor->key.data;
		key->size = wt_cursor->key.size;
	}

	if ((ret = dbc->get(dbc, key, value, DB_SET)) != 0)
		return (ret == DB_NOTFOUND ? WT_NOTFOUND : ret);

	if (cursor->recno)
		wt_cursor->recno = *(db_recno_t *)key->data;
	else {
		wt_cursor->key.data = key->data;
		wt_cursor->key.size = key->size;
	}
	wt_cursor->value.data = value->data;
	wt_cursor->value.size = value->size;
	return (0);
}

static int
kvs_cursor_search_near(WT_CURSOR *wt_cursor, int *exact)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	db_recno_t recno;
	int ret;

	(void)exact;				/* Unused parameters. */

	cursor = (CURSOR_SOURCE *)wt_cursor;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if (cursor->recno) {
		recno = (uint32_t)wt_cursor->recno;
		key->data = &recno;
		key->size = sizeof(db_recno_t);
	} else {
		key->data = (char *)wt_cursor->key.data;
		key->size = wt_cursor->key.size;
	}

	if ((ret = dbc->get(dbc, key, value, DB_SET_RANGE)) != 0)
		return (ret == DB_NOTFOUND ? WT_NOTFOUND : ret);

	if (cursor->recno)
		wt_cursor->recno = *(db_recno_t *)key->data;
	else {
		wt_cursor->key.data = key->data;
		wt_cursor->key.size = key->size;
	}
	wt_cursor->value.data = value->data;
	wt_cursor->value.size = value->size;
	return (0);
}

static int
kvs_cursor_insert(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	db_recno_t recno;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if (cursor->recno) {
		recno = (uint32_t)wt_cursor->recno;
		key->data = &recno;
		key->size = sizeof(db_recno_t);
	} else {
		key->data = (char *)wt_cursor->key.data;
		key->size = wt_cursor->key.size;
	}
	value->data = (char *)wt_cursor->value.data;
	value->size = wt_cursor->value.size;

	ERR(dbc->put(dbc, key, value, DB_KEYFIRST));

	return (0);
}

static int
kvs_cursor_update(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	db_recno_t recno;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if (cursor->recno) {
		recno = (uint32_t)wt_cursor->recno;
		key->data = &recno;
		key->size = sizeof(db_recno_t);
	} else {
		key->data = (char *)wt_cursor->key.data;
		key->size = wt_cursor->key.size;
	}
	value->data = (char *)wt_cursor->value.data;
	value->size = wt_cursor->value.size;

	ERR(dbc->put(dbc, key, value, DB_KEYFIRST));

	return (0);
}

static int
kvs_cursor_remove(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	db_recno_t recno;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if (cursor->recno) {
		recno = (uint32_t)wt_cursor->recno;
		key->data = &recno;
		key->size = sizeof(db_recno_t);
	} else {
		key->data = (char *)wt_cursor->key.data;
		key->size = wt_cursor->key.size;
	}

	if ((ret = dbc->get(dbc, key, value, DB_SET)) == DB_NOTFOUND)
		return (WT_NOTFOUND);
	ERR(ret);
	ERR(dbc->del(dbc, 0));

	return (0);
}

static int
kvs_cursor_close(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;

	cursor = (CURSOR_SOURCE *)wt_cursor;

	ERR(cursor->dbc->close(cursor->dbc));
	ERR(cursor->db->close(cursor->db, 0));
	free(wt_cursor);

	--open_cursors;
	return (0);
}

static int
kvs_create(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, int exclusive, const char *config)
{
	DB *db;
	DBTYPE type;
	const char *cfg[2], *name;
	char *key_format;
	uint32_t flags;

	(void)dsrc;				/* Unused parameters. */
	(void)session;

	if ((name = uri2name(uri)) == NULL)	/* Get the object name. */
		return (EINVAL);

	cfg[0] = config;
	cfg[1] = NULL;
	ERR(cfg_parse_str(cfg, "key_format", &key_format));
	type = strcmp(key_format, "r") == 0 ? DB_RECNO : DB_BTREE;

	flags = DB_CREATE | (exclusive ? DB_EXCL : 0);

				/* Create the Berkeley DB table. */
	ERR(db_create(&db, dbenv, 0));
	ERR(db->open(db, NULL, name, NULL, type, flags, 0));
	ERR(db->close(db, 0));
	return (0);
}

static int
kvs_drop(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *cfg[])
{
	DB *db;
	int ret;
	const char *name;

	(void)dsrc;				/* Unused parameters. */
	(void)session;
	(void)cfg;

	if ((name = uri2name(uri)) == NULL)	/* Get the object name. */
		return (EINVAL);

	lock();
	if (open_cursors == 0) {
		ERR(db_create(&db, dbenv, 0));
		ERR(db->remove(db, name, NULL, 0));
		/* db handle is dead */

		ret = 0;
	} else
		ret = EBUSY;
	unlock();

	return (ret);
}

static int
kvs_open_cursor(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *cfg[], WT_CURSOR **new_cursor)
{
	CURSOR_SOURCE *cursor;
	DB *db;
	const char *name;
	char *key_format;

	(void)dsrc;				/* Unused parameters. */
	(void)session;
	(void)cfg;

	if ((name = uri2name(uri)) == NULL)	/* Get the object name. */
		return (EINVAL);

						/* Parse configuration. */
	cursor = calloc(1, sizeof(CURSOR_SOURCE));
	ERR(cfg_parse_bool(cfg, "append", &cursor->append));
	ERR(cfg_parse_str(cfg, "key_format", &key_format));
	cursor->recno = strcmp(key_format, "r") == 0;
	free(key_format);

	lock();
				/* Open the Berkeley DB cursor. */
	ERR(db_create(&cursor->db, dbenv, 0));
	db = cursor->db;
	ERR(db->open(db, NULL,
	    name, NULL, cursor->recno ? DB_RECNO : DB_BTREE, DB_CREATE, 0));
	ERR(db->cursor(db, NULL, &cursor->dbc, 0));

				/* Initialize the methods. */
	cursor->cursor.next = kvs_cursor_next;
	cursor->cursor.prev = kvs_cursor_prev;
	cursor->cursor.reset = kvs_cursor_reset;
	cursor->cursor.search = kvs_cursor_search;
	cursor->cursor.search_near = kvs_cursor_search_near;
	cursor->cursor.insert = kvs_cursor_insert;
	cursor->cursor.update = kvs_cursor_update;
	cursor->cursor.remove = kvs_cursor_remove;
	cursor->cursor.close = kvs_cursor_close;

	*new_cursor = (WT_CURSOR *)cursor;

	++open_cursors;
	unlock();
	return (0);
}

static int
kvs_rename(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *newname, const char *cfg[])
{
	DB *db;
	int ret;
	const char *name;

	(void)dsrc;				/* Unused parameters. */
	(void)session;
	(void)cfg;

	if ((name = uri2name(uri)) == NULL)	/* Get the object name. */
		return (EINVAL);

	lock();
	if (open_cursors == 0) {
		ERR(db_create(&db, dbenv, 0));
		ERR(db->rename(db, name, NULL, newname, 0));
		/* db handle is dead */

		ret = 0;
	} else
		ret = EBUSY;

	unlock();
	return (ret);
}

static int
kvs_truncate(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *cfg[])
{
	DB *db;
	int ret;
	const char *name;

	(void)dsrc;				/* Unused parameters. */
	(void)session;
	(void)cfg;

	if ((name = uri2name(uri)) == NULL)	/* Get the object name. */
		return (EINVAL);

	lock();
	if (open_cursors == 0) {
		ERR(db_create(&db, dbenv, 0));
		ERR(db->open(db, NULL, name, NULL, DB_UNKNOWN, DB_TRUNCATE, 0));
		ERR(db->close(db, 0));
		ret = 0;
	} else
		ret = EBUSY;

	return (ret);
}

void
kvs_init(WT_CONNECTION *conn, const char *dir)
{
	int ret;

	if (pthread_rwlock_init(&rwlock, NULL) != 0)
		die(errno, "pthread_rwlock_init");

	if ((ret = db_env_create(&dbenv, 0)) != 0)
		die(errno, "db_env_create", kvs_error_map(ret));
	dbenv->set_errpfx(dbenv, "bdb");
	dbenv->set_errfile(dbenv, stderr);
	if ((ret = dbenv->open(dbenv, dir,
	    DB_CREATE | DB_INIT_LOCK | DB_INIT_MPOOL | DB_PRIVATE, 0)) != 0)
		die(0, "DB_ENV.open", kvs_error_map(ret));

	memset(&ds, 0, sizeof(ds));
	ds.dsrc.create = kvs_create;
	ds.dsrc.drop = kvs_drop;
	ds.dsrc.open_cursor = kvs_open_cursor;
	ds.dsrc.rename = kvs_rename;
	ds.dsrc.truncate = kvs_truncate;
	if ((ret =
	    conn->add_data_source(conn, "kvs:", &ds.dsrc, NULL)) != 0)
		die(ret, "WT_CONNECTION.add_data_source");
}

void
kvs_close(WT_CONNECTION *conn)
{
	int ret;

	(void)conn;				/* Unused parameters. */

	if ((ret = dbenv->close(dbenv, 0)) != 0)
		die(0, "DB_ENV.close", kvs_error_map(ret));
}
