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

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <db.h>
#include <wiredtiger.h>
#include <wiredtiger_ext.h>

#ifndef	UINT32_MAX                      	/* Maximum 32-bit unsigned */
#define	UINT32_MAX	4294967295U
#endif

static DB_ENV *dbenv;				/* Enclosing environment */
static WT_EXTENSION_API *wt_ext;		/* Extension functions */

/*
 * Macros to output an error message and set or return an error.
 * Requires local variables:
 *	int ret;
 */
#undef	ERET
#define	ERET(session, v, ...) do {					\
	(void)wt_ext->err_printf(wt_ext, session, __VA_ARGS__);		\
	return (v);							\
} while (0)
#undef	ESET
#define	ESET(session, v, ...) do {					\
	(void)wt_ext->err_printf(wt_ext, session, __VA_ARGS__);		\
	ret = v;							\
} while (0)

typedef struct __data_source DATA_SOURCE;
struct __data_source {
	WT_DATA_SOURCE dsrc;			/* Must come first */

	/*
	 * XXX
	 * This only works for a single object: if there were more than one
	 * object in test/format, cursor open would use the passed-in uri to
	 * find a { lock, cursor-count } pair to reference from each cursor
	 * object, and each session.XXX method call would have to use the
	 * appropriate { lock, cursor-count } pair based on their passed-in
	 * uri.
	 */
	pthread_rwlock_t rwlock;		/* Object's lock */
	int open_cursors;			/* Object's cursor count */
};
static DATA_SOURCE ds;

typedef struct __cursor_source CURSOR_SOURCE;
struct __cursor_source {
	WT_CURSOR cursor;			/* Must come first */

	WT_SESSION *session;			/* Enclosing session */

	WT_DATA_SOURCE *dsrc;			/* Enclosing data source */

	DB	*db;				/* Berkeley DB handles */
	DBC	*dbc;
	DBT	 key, value;
	db_recno_t recno;

	int	 config_append;			/* config "append" */
	int	 config_bitfield;		/* config "value_format=#t" */
	int	 config_overwrite;		/* config "overwrite" */
	int	 config_recno;			/* config "key_format=r" */
};

static void
lock(void)
{
	(void)pthread_rwlock_trywrlock(&ds.rwlock);
}

static void
unlock(void)
{
	(void)pthread_rwlock_unlock(&ds.rwlock);
}

static int
single_thread(WT_DATA_SOURCE *dsrc)
{
	DATA_SOURCE *data;

	data = (DATA_SOURCE *)dsrc;
	lock();
	if (data->open_cursors != 0) {
		unlock();
		return (EBUSY);
	}
	return (0);
}

static int
uri2name(WT_SESSION *session, const char *uri, const char **namep)
{
	const char *name;

	if ((name = strchr(uri, ':')) == NULL || *++name == '\0')
		ERET(session, EINVAL, "unsupported object: %s", uri);
	*namep = name;
	return (0);
}

static inline int
recno_convert(WT_CURSOR *wt_cursor, db_recno_t *recnop)
{
	CURSOR_SOURCE *cursor;
	WT_SESSION *session;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;

	if (wt_cursor->recno > UINT32_MAX)
		ERET(session, ERANGE, "record number %" PRIuMAX ": %s",
		    (uintmax_t)wt_cursor->recno, strerror(ERANGE));

	*recnop = (uint32_t)wt_cursor->recno;
	return (0);
}

static inline int
copyin_key(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBT *key;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	key = &cursor->key;

	if (cursor->config_recno) {
		if ((ret = recno_convert(wt_cursor, &cursor->recno)) != 0)
			return (ret);
		key->data = &cursor->recno;
		key->size = sizeof(db_recno_t);
	} else {
		key->data = (char *)wt_cursor->key.data;
		key->size = wt_cursor->key.size;
	}
	return (0);
}

static inline void
copyin_value(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBT *value;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	value = &cursor->value;

	value->data = (char *)wt_cursor->value.data;
	value->size = wt_cursor->value.size;
}

static inline void
copyout_key(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBT *key;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	key = &cursor->key;

	if (cursor->config_recno)
		wt_cursor->recno = *(db_recno_t *)key->data;
	else {
		wt_cursor->key.data = key->data;
		wt_cursor->key.size = key->size;
	}
}

static inline void
copyout_value(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBT *value;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	value = &cursor->value;

	wt_cursor->value.data = value->data;
	wt_cursor->value.size = value->size;
}

#ifdef HAVE_DIAGNOSTIC
static int
bdb_dump(WT_CURSOR *wt_cursor, WT_SESSION *session, const char *tag)
{
	CURSOR_SOURCE *cursor;
	DB *db;
	DBC *dbc;
	DBT *key, *value;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	db = cursor->db;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = db->cursor(db, NULL, &dbc, 0)) != 0)
		ERET(session, WT_ERROR, "Db.cursor: %s", db_strerror(ret));
	printf("==> %s\n", tag);
	while ((ret = dbc->get(dbc, key, value, DB_NEXT)) == 0)
		if (cursor->config_recno)
			printf("\t%llu/%.*s\n",
			    (unsigned long long)*(db_recno_t *)key->data,
			    (int)value->size, (char *)value->data);
		else
			printf("\t%.*s/%.*s\n",
			    (int)key->size, (char *)key->data,
			    (int)value->size, (char *)value->data);

	if (ret != DB_NOTFOUND)
		ERET(session, WT_ERROR, "DbCursor.get: %s", db_strerror(ret));

	return (0);
}
#endif

static int
kvs_cursor_next(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_SESSION *session;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = dbc->get(dbc, key, value, DB_NEXT)) == 0)  {
		copyout_key(wt_cursor);
		copyout_value(wt_cursor);
		return (0);
	}

	if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
		return (WT_NOTFOUND);
	ERET(session, WT_ERROR, "DbCursor.get: %s", db_strerror(ret));
}

static int
kvs_cursor_prev(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_SESSION *session;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = dbc->get(dbc, key, value, DB_PREV)) == 0)  {
		copyout_key(wt_cursor);
		copyout_value(wt_cursor);
		return (0);
	}

	if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
		return (WT_NOTFOUND);
	ERET(session, WT_ERROR, "DbCursor.get: %s", db_strerror(ret));
}

static int
kvs_cursor_reset(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	WT_SESSION *session;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;

	/* Close and re-open the Berkeley DB cursor */
	if ((dbc = cursor->dbc) != NULL) {
		cursor->dbc = NULL;
		if ((ret = dbc->close(dbc)) != 0)
			ERET(session, WT_ERROR,
			    "DbCursor.close: %s", db_strerror(ret));

		if ((ret = cursor->db->cursor(cursor->db, NULL, &dbc, 0)) != 0)
			ERET(session, WT_ERROR,
			    "Db.cursor: %s", db_strerror(ret));
		cursor->dbc = dbc;
	}
	return (0);
}

static int
kvs_cursor_search(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_SESSION *session;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = copyin_key(wt_cursor)) != 0)
		return (ret);

	if ((ret = dbc->get(dbc, key, value, DB_SET)) == 0) {
		copyout_key(wt_cursor);
		copyout_value(wt_cursor);
		return (0);
	}

	if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
		return (WT_NOTFOUND);
	ERET(session, WT_ERROR, "DbCursor.get: %s", db_strerror(ret));
}

static int
kvs_cursor_search_near(WT_CURSOR *wt_cursor, int *exact)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_SESSION *session;
	uint32_t len;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = copyin_key(wt_cursor)) != 0)
		return (ret);

retry:	if ((ret = dbc->get(dbc, key, value, DB_SET_RANGE)) == 0) {
		/*
		 * WiredTiger returns the logically adjacent key (which might
		 * be less than, equal to, or greater than the specified key),
		 * Berkeley DB returns a key equal to or greater than the
		 * specified key.  Check for an exact match, otherwise Berkeley
		 * DB must have returned a larger key than the one specified.
		 */
		if (key->size == wt_cursor->key.size &&
		    memcmp(key->data, wt_cursor->key.data, key->size) == 0)
			*exact = 0;
		else
			*exact = 1;
		copyout_key(wt_cursor);
		copyout_value(wt_cursor);
		return (0);
	}

	/*
	 * Berkeley DB only returns keys equal to or greater than the specified
	 * key, while WiredTiger returns adjacent keys, that is, if there's a
	 * key smaller than the specified key, it's supposed to be returned.  In
	 * other words, WiredTiger only fails if the store is empty.  Read the
	 * last key in the store, and see if it's less than the specified key,
	 * in which case we have the right key to return.  If it's not less than
	 * the specified key, we're racing with some other thread, throw up our
	 * hands and try again.
	 */
	if ((ret = dbc->get(dbc, key, value, DB_LAST)) == 0) {
		len = key->size < wt_cursor->key.size ?
		    key->size : wt_cursor->key.size;
		if (memcmp(key->data, wt_cursor->key.data, len) < 0) {
			*exact = -1;
			copyout_key(wt_cursor);
			copyout_value(wt_cursor);
			return (0);
		}
		goto retry;
	}

	if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
		return (WT_NOTFOUND);
	ERET(session, WT_ERROR, "DbCursor.get: %s", db_strerror(ret));
}

static int
kvs_cursor_insert(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DB *db;
	DBC *dbc;
	DBT *key, *value;
	WT_SESSION *session;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;
	dbc = cursor->dbc;
	db = cursor->db;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = copyin_key(wt_cursor)) != 0)
		return (ret);
	copyin_value(wt_cursor);

	if (cursor->config_append) {
		/*
		 * Berkeley DB cursors have no operation to append/create a
		 * new record and set the cursor; use the DB handle instead
		 * then set the cursor explicitly.
		 *
		 * When appending, we're allocating and returning a new record
		 * number.
		 */
		if ((ret = db->put(db, NULL, key, value, DB_APPEND)) != 0)
			ERET(session, WT_ERROR, "Db.put: %s", db_strerror(ret));
		wt_cursor->recno = *(db_recno_t *)key->data;

		if ((ret = dbc->get(dbc, key, value, DB_SET)) != 0)
			ERET(session, WT_ERROR,
			    "DbCursor.get: %s", db_strerror(ret));
	} else if (cursor->config_overwrite) {
		if ((ret = dbc->put(dbc, key, value, DB_KEYFIRST)) != 0)
			ERET(session, WT_ERROR,
			    "DbCursor.put: %s", db_strerror(ret));
	} else {
		/*
		 * Berkeley DB cursors don't have a no-overwrite flag; use
		 * the DB handle instead then set the cursor explicitly.
		 */
		if ((ret =
		    db->put(db, NULL, key, value, DB_NOOVERWRITE)) != 0) {
			if (ret == DB_KEYEXIST)
				return (WT_DUPLICATE_KEY);
			ERET(session, WT_ERROR, "Db.put: %s", db_strerror(ret));
		}
		if ((ret = dbc->get(dbc, key, value, DB_SET)) != 0)
			ERET(session, WT_ERROR,
			    "DbCursor.get: %s", db_strerror(ret));
	}

	return (0);
}

static int
kvs_cursor_update(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_SESSION *session;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = copyin_key(wt_cursor)) != 0)
		return (ret);
	copyin_value(wt_cursor);

	if ((ret = dbc->put(dbc, key, value, DB_KEYFIRST)) != 0)
		ERET(session, WT_ERROR, "DbCursor.put: %s", db_strerror(ret));

	return (0);
}

static int
kvs_cursor_remove(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_SESSION *session;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	/*
	 * WiredTiger's "remove" of a bitfield is really an update with a value
	 * of a single byte of zero.
	 */
	if (cursor->config_bitfield) {
		wt_cursor->value.size = 1;
		wt_cursor->value.data = "\0";
		return (kvs_cursor_update(wt_cursor));
	}

	if ((ret = copyin_key(wt_cursor)) != 0)
		return (ret);

	if ((ret = dbc->get(dbc, key, value, DB_SET)) != 0) {
		if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
			return (WT_NOTFOUND);
		ERET(session, WT_ERROR, "DbCursor.get: %s", db_strerror(ret));
	}
	if ((ret = dbc->del(dbc, 0)) != 0)
		ERET(session, WT_ERROR, "DbCursor.del: %s", db_strerror(ret));

	return (0);
}

static int
kvs_cursor_close(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DATA_SOURCE *data;
	DB *db;
	DBC *dbc;
	WT_SESSION *session;
	int ret, tret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;
	data = (DATA_SOURCE *)cursor->dsrc;
	ret = 0;

	dbc = cursor->dbc;
	cursor->dbc = NULL;
	if (dbc != NULL && (tret = dbc->close(dbc)) != 0)
		ERET(session, WT_ERROR,
		    "DbCursor.close: %s", db_strerror(tret));

	db = cursor->db;
	cursor->db = NULL;
	if (db != NULL && (tret = db->close(db, 0)) != 0)
		ERET(session, WT_ERROR, "Db.close: %s", db_strerror(tret));
	free(wt_cursor);

	lock();
	--data->open_cursors;
	unlock();

	return (ret);
}

static int
kvs_create(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config)
{
	DB *db;
	DBTYPE type;
	WT_CONFIG_ITEM v;
	int ret, tret;
	const char *name;

	(void)dsrc;				/* Unused parameters */

						/* Get the object name */
	if ((ret = uri2name(session, uri, &name)) != 0)
		return (ret);

						/* Check key/value formats */
	if ((ret = wt_ext->config_get(
	    wt_ext, session, config, "key_format", &v)) != 0)
		ERET(session, ret,
		    "key_format configuration: %s", wiredtiger_strerror(ret));
	type = v.len == 1 && v.str[0] == 'r' ? DB_RECNO : DB_BTREE;

	ret = 0;			/* Create the Berkeley DB table */
	if ((tret = db_create(&db, dbenv, 0)) != 0)
		ERET(session, WT_ERROR, "db_create: %s", db_strerror(tret));
	if ((tret = db->open(db, NULL, name, NULL, type, DB_CREATE, 0)) != 0)
		ESET(session, WT_ERROR, "Db.open: %s", uri, db_strerror(tret));
	if ((tret = db->close(db, 0)) != 0)
		ESET(session, WT_ERROR, "Db.close", db_strerror(tret));
	return (ret);
}

static int
kvs_drop(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config)
{
	DB *db;
	int ret, tret;
	const char *name;

	(void)config;				/* Unused parameters */

						/* Get the object name */
	if ((ret = uri2name(session, uri, &name)) != 0)
		return (ret);

	if ((ret = single_thread(dsrc)) != 0)
		return (ret);

	ret = 0;
	if ((tret = db_create(&db, dbenv, 0)) != 0)
		ESET(session, WT_ERROR, "db_create: %s", db_strerror(tret));
	else if ((tret = db->remove(db, name, NULL, 0)) != 0)
		ESET(session, WT_ERROR, "Db.remove: %s", db_strerror(tret));
	/* db handle is dead */

	unlock();
	return (ret);
}

static int
kvs_open_cursor(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config, WT_CURSOR **new_cursor)
{
	CURSOR_SOURCE *cursor;
	DATA_SOURCE *data;
	DB *db;
	WT_CONFIG_ITEM v;
	int locked, ret, tret;
	const char *name;

	data = (DATA_SOURCE *)dsrc;
	locked = 0;
						/* Get the object name */
	if ((ret = uri2name(session, uri, &name)) != 0)
		return (ret);
						/* Allocate the cursor */
	cursor = calloc(1, sizeof(CURSOR_SOURCE));
	cursor->session = session;
	cursor->dsrc = dsrc;
						/* Parse configuration */
	if ((ret = wt_ext->config_get(
	    wt_ext, session, config, "append", &v)) != 0) {
		ESET(session, ret,
		    "append configuration: %s", wiredtiger_strerror(ret));
		goto err;
	}
	cursor->config_append = v.val != 0;
	if ((ret = wt_ext->config_get(
	    wt_ext, session, config, "overwrite", &v)) != 0) {
		ESET(session, ret,
		    "overwrite configuration: %s", wiredtiger_strerror(ret));
		goto err;
	}
	cursor->config_overwrite = v.val != 0;
	if ((ret = wt_ext->config_get(
	    wt_ext, session, config, "key_format", &v)) != 0) {
		ESET(session, ret,
		    "key_format configuration: %s", wiredtiger_strerror(ret));
		goto err;
	}
	cursor->config_recno = v.len == 1 && v.str[0] == 'r';

	if ((ret = wt_ext->config_get(
	    wt_ext, session, config, "value_format", &v)) != 0) {
		ESET(session, ret,
		    "value_format configuration: %s", wiredtiger_strerror(ret));
		goto err;
	}
	cursor->config_bitfield =
	    v.len == 2 && isdigit(v.str[0]) && v.str[1] == 't';

	lock();
	locked = 1;
				/* Open the Berkeley DB cursor */
	if ((tret = db_create(&cursor->db, dbenv, 0)) != 0) {
		ESET(session, WT_ERROR, "db_create: %s", db_strerror(tret));
		goto err;
	}
	db = cursor->db;
	if ((tret = db->open(db, NULL, name, NULL,
	    cursor->config_recno ? DB_RECNO : DB_BTREE, DB_CREATE, 0)) != 0) {
		ESET(session, WT_ERROR, "Db.open: %s", db_strerror(tret));
		goto err;
	}
	if ((tret = db->cursor(db, NULL, &cursor->dbc, 0)) != 0) {
		ESET(session, WT_ERROR, "Db.cursor: %s", db_strerror(tret));
		goto err;
	}

				/* Initialize the methods */
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

	++data->open_cursors;

	if (0) {
err:		free(cursor);
	}

	if (locked)
		unlock();
	return (ret);
}

static int
kvs_rename(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *newname, WT_CONFIG_ARG *config)
{
	DB *db;
	int ret, tret;
	const char *name;

	(void)config;				/* Unused parameters */

						/* Get the object name */
	if ((ret = uri2name(session, uri, &name)) != 0)
		return (ret);

	if ((ret = single_thread(dsrc)) != 0)
		return (ret);

	ret = 0;
	if ((tret = db_create(&db, dbenv, 0)) != 0)
		ESET(session, WT_ERROR, "db_create: %s", db_strerror(tret));
	else if ((tret = db->rename(db, name, NULL, newname, 0)) != 0)
		ESET(session, WT_ERROR, "Db.rename: %s", db_strerror(tret));
	/* db handle is dead */

	unlock();
	return (ret);
}

static int
kvs_truncate(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config)
{
	DB *db;
	int ret, tret;
	const char *name;

	(void)config;				/* Unused parameters */

						/* Get the object name */
	if ((ret = uri2name(session, uri, &name)) != 0)
		return (ret);

	if ((ret = single_thread(dsrc)) != 0)
		return (ret);

	ret = 0;
	if ((tret = db_create(&db, dbenv, 0)) == 0) {
		if ((tret = db->open(db,
		    NULL, name, NULL, DB_UNKNOWN, DB_TRUNCATE, 0)) != 0)
			ESET(session, WT_ERROR,
			    "Db.open: %s", db_strerror(tret));
		if ((tret = db->close(db, 0)) != 0)
			ESET(session, WT_ERROR,
			    "Db.close: %s", db_strerror(tret));
	} else
		ESET(session, WT_ERROR, "db_create: %s", db_strerror(tret));

	unlock();
	return (ret);
}

static int
kvs_verify(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config)
{
	DB *db;
	int ret, tret;
	const char *name;

	(void)config;				/* Unused parameters */

						/* Get the object name */
	if ((ret = uri2name(session, uri, &name)) != 0)
		return (ret);

	if ((ret = single_thread(dsrc)) != 0)
		return (ret);

	ret = 0;
	if ((tret = db_create(&db, dbenv, 0)) != 0)
		ESET(session, WT_ERROR, "db_create: %s", db_strerror(tret));
	else if ((tret = db->verify(db, name, NULL, NULL, 0)) != 0)
		ESET(session, WT_ERROR,
		    "Db.verify: %s: %s", uri, db_strerror(tret));
	/* db handle is dead */

	unlock();
	return (ret);
}

int
wiredtiger_extension_init(WT_CONNECTION *conn, WT_CONFIG_ARG *config)
{
	int ret;

	(void)config;				/* Unused parameters */

						/* Acquire the extension API. */
	wt_ext = conn->get_extension_api(conn);

	memset(&ds, 0, sizeof(ds));

	if ((ret = pthread_rwlock_init(&ds.rwlock, NULL)) != 0)
		ERET(NULL, WT_PANIC, "lock init: %s", strerror(ret));

	if ((ret = db_env_create(&dbenv, 0)) != 0)
		ERET(NULL, WT_ERROR, "db_env_create: %s", db_strerror(ret));
	dbenv->set_errpfx(dbenv, "bdb");
	dbenv->set_errfile(dbenv, stderr);
	if ((ret = dbenv->open(dbenv, "RUNDIR/KVS",
	    DB_CREATE | DB_INIT_LOCK | DB_INIT_MPOOL | DB_PRIVATE, 0)) != 0)
		ERET(NULL, WT_ERROR, "DbEnv.open: %s", db_strerror(ret));

	ds.dsrc.create = kvs_create;
	ds.dsrc.compact = NULL;			/* No compaction */
	ds.dsrc.drop = kvs_drop;
	ds.dsrc.open_cursor = kvs_open_cursor;
	ds.dsrc.rename = kvs_rename;
	ds.dsrc.salvage = NULL;			/* No salvage */
	ds.dsrc.truncate = kvs_truncate;
	ds.dsrc.verify = kvs_verify;
	if ((ret =
	    conn->add_data_source(conn, "kvsbdb:", &ds.dsrc, NULL)) != 0)
		ERET(NULL, ret, "WT_CONNECTION.add_data_source");

	return (0);
}

int
wiredtiger_extension_terminate(WT_CONNECTION *conn)
{
	int ret;

	(void)conn;				/* Unused parameters */

	 if ((ret = pthread_rwlock_destroy(&ds.rwlock)) != 0)
		ERET(NULL, WT_PANIC, "lock destroy: %s", strerror(ret));

	if (dbenv != NULL && (ret = dbenv->close(dbenv, 0)) != 0)
		ERET(NULL, WT_ERROR, "DbEnv.close: %s", db_strerror(ret));

	return (0);
}
