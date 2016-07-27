/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
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

#undef	INLINE
#define	INLINE	inline				/* Turn off inline */

#ifndef	UINT32_MAX                      	/* Maximum 32-bit unsigned */
#define	UINT32_MAX	4294967295U
#endif

/*
 * Macros to output an error message and set or return an error.
 * Requires local variables:
 *	int ret;
 */
#undef	ERET
#define	ERET(wtext, session, v, ...) do {				\
	(void)wtext->err_printf(wtext, session, __VA_ARGS__);		\
	return (v);							\
} while (0)
#undef	ESET
#define	ESET(wtext, session, v, ...) do {				\
	(void)wtext->err_printf(wtext, session, __VA_ARGS__);		\
	ret = v;							\
} while (0)
#undef	ETRET
#define	ETRET(a) do {							\
	int __ret;							\
	if ((__ret = (a)) != 0 &&					\
	    (__ret == WT_PANIC ||					\
	    ret == 0 || ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND))	\
		ret = __ret;						\
} while (0)

typedef struct __data_source DATA_SOURCE;

typedef struct __cursor_source {
	WT_CURSOR wtcursor;			/* Must come first */

	WT_EXTENSION_API *wtext;		/* Extension functions */

	DATA_SOURCE *ds;			/* Underlying Berkeley DB */

	DB	*db;				/* Berkeley DB handles */
	DBC	*dbc;
	DBT	 key, value;
	db_recno_t recno;

	int	 config_append;			/* config "append" */
	int	 config_bitfield;		/* config "value_format=#t" */
	int	 config_overwrite;		/* config "overwrite" */
	int	 config_recno;			/* config "key_format=r" */
} CURSOR_SOURCE;

struct __data_source {
	WT_DATA_SOURCE wtds;			/* Must come first */

	WT_EXTENSION_API *wtext;		/* Extension functions */

	/*
	 * We single thread all WT_SESSION methods and return EBUSY if a
	 * WT_SESSION method is called and there's an open cursor.
	 *
	 * XXX
	 * This only works for a single object: if there were more than one
	 * object in test/format, cursor open would use the passed-in uri to
	 * find a { lock, cursor-count } pair to reference from each cursor
	 * object, and each session.XXX method call would have to use the
	 * appropriate { lock, cursor-count } pair based on their passed-in
	 * uri.
	 */
	pthread_rwlock_t rwlock;		/* Global lock */

	DB_ENV *dbenv;				/* Berkeley DB environment */
	int open_cursors;			/* Open cursor count */
};

/*
 * os_errno --
 *	Limit our use of errno so it's easy to remove.
 */
static int
os_errno(void)
{
	return (errno);
}

/*
 * lock_init --
 *	Initialize an object's lock.
 */
static int
lock_init(
    WT_EXTENSION_API *wtext, WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret = 0;

	if ((ret = pthread_rwlock_init(lockp, NULL)) != 0)
		ERET(wtext, session, WT_PANIC, "lock init: %s", strerror(ret));
	return (0);
}

/*
 * lock_destroy --
 *	Destroy an object's lock.
 */
static int
lock_destroy(
    WT_EXTENSION_API *wtext, WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret = 0;

	if ((ret = pthread_rwlock_destroy(lockp)) != 0)
		ERET(wtext,
		    session, WT_PANIC, "lock destroy: %s", strerror(ret));
	return (0);
}

/*
 * writelock --
 *	Acquire a write lock.
 */
static INLINE int
writelock(
    WT_EXTENSION_API *wtext, WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret = 0;

	if ((ret = pthread_rwlock_wrlock(lockp)) != 0)
		ERET(wtext,
		    session, WT_PANIC, "write-lock: %s", strerror(ret));
	return (0);
}

/*
 * unlock --
 *	Release an object's lock.
 */
static INLINE int
unlock(WT_EXTENSION_API *wtext, WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret = 0;

	if ((ret = pthread_rwlock_unlock(lockp)) != 0)
		ERET(wtext, session, WT_PANIC, "unlock: %s", strerror(ret));
	return (0);
}

static int
single_thread(
    WT_DATA_SOURCE *wtds, WT_SESSION *session, pthread_rwlock_t *lockp)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	int ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	if ((ret = writelock(wtext, session, lockp)) != 0)
		return (ret);
	if (ds->open_cursors != 0) {
		if ((ret = unlock(wtext, session, lockp)) != 0)
			return (ret);
		return (EBUSY);
	}
	return (0);
}

static int
uri2name(WT_EXTENSION_API *wtext,
    WT_SESSION *session, const char *uri, const char **namep)
{
	const char *name;

	if ((name = strchr(uri, ':')) == NULL || *++name == '\0')
		ERET(wtext, session, EINVAL, "unsupported object: %s", uri);
	*namep = name;
	return (0);
}

static INLINE int
recno_convert(WT_CURSOR *wtcursor, db_recno_t *recnop)
{
	CURSOR_SOURCE *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;

	session = wtcursor->session;
	cursor = (CURSOR_SOURCE *)wtcursor;
	wtext = cursor->wtext;

	if (wtcursor->recno > UINT32_MAX)
		ERET(wtext,
		    session, ERANGE, "record number %" PRIuMAX ": %s",
		    (uintmax_t)wtcursor->recno, strerror(ERANGE));

	*recnop = (uint32_t)wtcursor->recno;
	return (0);
}

static INLINE int
copyin_key(WT_CURSOR *wtcursor)
{
	CURSOR_SOURCE *cursor;
	DBT *key;
	int ret = 0;

	cursor = (CURSOR_SOURCE *)wtcursor;
	key = &cursor->key;

	if (cursor->config_recno) {
		if ((ret = recno_convert(wtcursor, &cursor->recno)) != 0)
			return (ret);
		key->data = &cursor->recno;
		key->size = sizeof(db_recno_t);
	} else {
		key->data = (char *)wtcursor->key.data;
		key->size = (uint32_t)wtcursor->key.size;
	}
	return (0);
}

static INLINE void
copyout_key(WT_CURSOR *wtcursor)
{
	CURSOR_SOURCE *cursor;
	DBT *key;

	cursor = (CURSOR_SOURCE *)wtcursor;
	key = &cursor->key;

	if (cursor->config_recno)
		wtcursor->recno = *(db_recno_t *)key->data;
	else {
		wtcursor->key.data = key->data;
		wtcursor->key.size = key->size;
	}
}

static INLINE void
copyin_value(WT_CURSOR *wtcursor)
{
	CURSOR_SOURCE *cursor;
	DBT *value;

	cursor = (CURSOR_SOURCE *)wtcursor;
	value = &cursor->value;

	value->data = (char *)wtcursor->value.data;
	value->size = (uint32_t)wtcursor->value.size;
}

static INLINE void
copyout_value(WT_CURSOR *wtcursor)
{
	CURSOR_SOURCE *cursor;
	DBT *value;

	cursor = (CURSOR_SOURCE *)wtcursor;
	value = &cursor->value;

	wtcursor->value.data = value->data;
	wtcursor->value.size = value->size;
}

#if 0
static int
bdb_dump(WT_CURSOR *wtcursor, WT_SESSION *session, const char *tag)
{
	CURSOR_SOURCE *cursor;
	DB *db;
	DBC *dbc;
	DBT *key, *value;
	WT_EXTENSION_API *wtext;
	int ret = 0;

	cursor = (CURSOR_SOURCE *)wtcursor;
	wtext = cursor->wtext;

	db = cursor->db;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = db->cursor(db, NULL, &dbc, 0)) != 0)
		ERET(wtext,
		    session, WT_ERROR, "Db.cursor: %s", db_strerror(ret));
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
		ERET(wtext,
		    session, WT_ERROR, "DbCursor.get: %s", db_strerror(ret));

	return (0);
}
#endif

static int
kvs_cursor_next(WT_CURSOR *wtcursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR_SOURCE *)wtcursor;
	wtext = cursor->wtext;

	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = dbc->get(dbc, key, value, DB_NEXT)) == 0)  {
		copyout_key(wtcursor);
		copyout_value(wtcursor);
		return (0);
	}

	if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
		return (WT_NOTFOUND);
	ERET(wtext, session, WT_ERROR, "DbCursor.get: %s", db_strerror(ret));
}

static int
kvs_cursor_prev(WT_CURSOR *wtcursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR_SOURCE *)wtcursor;
	wtext = cursor->wtext;

	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = dbc->get(dbc, key, value, DB_PREV)) == 0)  {
		copyout_key(wtcursor);
		copyout_value(wtcursor);
		return (0);
	}

	if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
		return (WT_NOTFOUND);
	ERET(wtext, session, WT_ERROR, "DbCursor.get: %s", db_strerror(ret));
}

static int
kvs_cursor_reset(WT_CURSOR *wtcursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR_SOURCE *)wtcursor;
	wtext = cursor->wtext;

	/* Close and re-open the Berkeley DB cursor */
	if ((dbc = cursor->dbc) != NULL) {
		cursor->dbc = NULL;
		if ((ret = dbc->close(dbc)) != 0)
			ERET(wtext, session, WT_ERROR,
			    "DbCursor.close: %s", db_strerror(ret));

		if ((ret = cursor->db->cursor(cursor->db, NULL, &dbc, 0)) != 0)
			ERET(wtext, session, WT_ERROR,
			    "Db.cursor: %s", db_strerror(ret));
		cursor->dbc = dbc;
	}
	return (0);
}

static int
kvs_cursor_search(WT_CURSOR *wtcursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR_SOURCE *)wtcursor;
	wtext = cursor->wtext;

	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = copyin_key(wtcursor)) != 0)
		return (ret);

	if ((ret = dbc->get(dbc, key, value, DB_SET)) == 0) {
		copyout_key(wtcursor);
		copyout_value(wtcursor);
		return (0);
	}

	if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
		return (WT_NOTFOUND);
	ERET(wtext, session, WT_ERROR, "DbCursor.get: %s", db_strerror(ret));
}

static int
kvs_cursor_search_near(WT_CURSOR *wtcursor, int *exact)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	size_t len;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR_SOURCE *)wtcursor;
	wtext = cursor->wtext;

	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = copyin_key(wtcursor)) != 0)
		return (ret);

retry:	if ((ret = dbc->get(dbc, key, value, DB_SET_RANGE)) == 0) {
		/*
		 * WiredTiger returns the logically adjacent key (which might
		 * be less than, equal to, or greater than the specified key),
		 * Berkeley DB returns a key equal to or greater than the
		 * specified key.  Check for an exact match, otherwise Berkeley
		 * DB must have returned a larger key than the one specified.
		 */
		if (key->size == wtcursor->key.size &&
		    memcmp(key->data, wtcursor->key.data, key->size) == 0)
			*exact = 0;
		else
			*exact = 1;
		copyout_key(wtcursor);
		copyout_value(wtcursor);
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
		len = key->size < wtcursor->key.size ?
		    key->size : wtcursor->key.size;
		if (memcmp(key->data, wtcursor->key.data, len) < 0) {
			*exact = -1;
			copyout_key(wtcursor);
			copyout_value(wtcursor);
			return (0);
		}
		goto retry;
	}

	if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
		return (WT_NOTFOUND);
	ERET(wtext, session, WT_ERROR, "DbCursor.get: %s", db_strerror(ret));
}

static int
kvs_cursor_insert(WT_CURSOR *wtcursor)
{
	CURSOR_SOURCE *cursor;
	DB *db;
	DBC *dbc;
	DBT *key, *value;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR_SOURCE *)wtcursor;
	wtext = cursor->wtext;

	dbc = cursor->dbc;
	db = cursor->db;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = copyin_key(wtcursor)) != 0)
		return (ret);
	copyin_value(wtcursor);

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
			ERET(wtext,
			    session, WT_ERROR, "Db.put: %s", db_strerror(ret));
		wtcursor->recno = *(db_recno_t *)key->data;

		if ((ret = dbc->get(dbc, key, value, DB_SET)) != 0)
			ERET(wtext, session, WT_ERROR,
			    "DbCursor.get: %s", db_strerror(ret));
	} else if (cursor->config_overwrite) {
		if ((ret = dbc->put(dbc, key, value, DB_KEYFIRST)) != 0)
			ERET(wtext, session, WT_ERROR,
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
			ERET(wtext,
			    session, WT_ERROR, "Db.put: %s", db_strerror(ret));
		}
		if ((ret = dbc->get(dbc, key, value, DB_SET)) != 0)
			ERET(wtext, session, WT_ERROR,
			    "DbCursor.get: %s", db_strerror(ret));
	}

	return (0);
}

static int
kvs_cursor_update(WT_CURSOR *wtcursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR_SOURCE *)wtcursor;
	wtext = cursor->wtext;

	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if ((ret = copyin_key(wtcursor)) != 0)
		return (ret);
	copyin_value(wtcursor);

	if ((ret = dbc->put(dbc, key, value, DB_KEYFIRST)) != 0)
		ERET(wtext,
		    session, WT_ERROR, "DbCursor.put: %s", db_strerror(ret));

	return (0);
}

static int
kvs_cursor_remove(WT_CURSOR *wtcursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR_SOURCE *)wtcursor;
	wtext = cursor->wtext;

	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	/*
	 * WiredTiger's "remove" of a bitfield is really an update with a value
	 * of a single byte of zero.
	 */
	if (cursor->config_bitfield) {
		wtcursor->value.size = 1;
		wtcursor->value.data = "\0";
		return (kvs_cursor_update(wtcursor));
	}

	if ((ret = copyin_key(wtcursor)) != 0)
		return (ret);

	if ((ret = dbc->get(dbc, key, value, DB_SET)) != 0) {
		if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
			return (WT_NOTFOUND);
		ERET(wtext,
		    session, WT_ERROR, "DbCursor.get: %s", db_strerror(ret));
	}
	if ((ret = dbc->del(dbc, 0)) != 0)
		ERET(wtext,
		    session, WT_ERROR, "DbCursor.del: %s", db_strerror(ret));

	return (0);
}

static int
kvs_cursor_close(WT_CURSOR *wtcursor)
{
	CURSOR_SOURCE *cursor;
	DATA_SOURCE *ds;
	DB *db;
	DBC *dbc;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR_SOURCE *)wtcursor;
	ds = cursor->ds;
	wtext = cursor->wtext;

	dbc = cursor->dbc;
	cursor->dbc = NULL;
	if (dbc != NULL && (ret = dbc->close(dbc)) != 0)
		ERET(wtext, session, WT_ERROR,
		    "DbCursor.close: %s", db_strerror(ret));

	db = cursor->db;
	cursor->db = NULL;
	if (db != NULL && (ret = db->close(db, 0)) != 0)
		ERET(wtext,
		    session, WT_ERROR, "Db.close: %s", db_strerror(ret));
	free(wtcursor);

	if ((ret = writelock(wtext, session, &ds->rwlock)) != 0)
		return (ret);
	--ds->open_cursors;
	if ((ret = unlock(wtext, session, &ds->rwlock)) != 0)
		return (ret);

	return (0);
}

static int
kvs_session_create(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	DB *db;
	DBTYPE type;
	WT_CONFIG_ITEM v;
	WT_EXTENSION_API *wtext;
	int ret = 0;
	const char *name;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
						/* Get the object name */
	if ((ret = uri2name(wtext, session, uri, &name)) != 0)
		return (ret);
						/* Check key/value formats */
	if ((ret =
	    wtext->config_get(wtext, session, config, "key_format", &v)) != 0)
		ERET(wtext, session, ret,
		    "key_format configuration: %s",
		    wtext->strerror(wtext, session, ret));
	type = v.len == 1 && v.str[0] == 'r' ? DB_RECNO : DB_BTREE;

	/* Create the Berkeley DB table */
	if ((ret = db_create(&db, ds->dbenv, 0)) != 0)
		ERET(wtext,
		    session, WT_ERROR, "db_create: %s", db_strerror(ret));
	if ((ret = db->open(db, NULL, name, NULL, type, DB_CREATE, 0)) != 0)
		ERET(wtext,
		    session, WT_ERROR, "Db.open: %s", uri, db_strerror(ret));
	if ((ret = db->close(db, 0)) != 0)
		ERET(wtext, session, WT_ERROR, "Db.close", db_strerror(ret));

	return (0);
}

static int
kvs_session_drop(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	DB *db;
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	int ret = 0;
	const char *name;

	(void)config;				/* Unused parameters */

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
						/* Get the object name */
	if ((ret = uri2name(wtext, session, uri, &name)) != 0)
		return (ret);

	if ((ret = single_thread(wtds, session, &ds->rwlock)) != 0)
		return (ret);

	if ((ret = db_create(&db, ds->dbenv, 0)) != 0)
		ESET(wtext,
		    session, WT_ERROR, "db_create: %s", db_strerror(ret));
	else if ((ret = db->remove(db, name, NULL, 0)) != 0)
		ESET(wtext,
		    session, WT_ERROR, "Db.remove: %s", db_strerror(ret));
	/* db handle is dead */

	ETRET(unlock(wtext, session, &ds->rwlock));
	return (ret);
}

static int
kvs_session_open_cursor(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config, WT_CURSOR **new_cursor)
{
	CURSOR_SOURCE *cursor;
	DATA_SOURCE *ds;
	DB *db;
	WT_CONFIG_ITEM v;
	WT_EXTENSION_API *wtext;
	int locked, ret;
	const char *name;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	locked = 0;
						/* Get the object name */
	if ((ret = uri2name(wtext, session, uri, &name)) != 0)
		return (ret);
						/* Allocate the cursor */
	if ((cursor = calloc(1, sizeof(CURSOR_SOURCE))) == NULL)
		return (os_errno());
	cursor->ds = (DATA_SOURCE *)wtds;
	cursor->wtext = wtext;
						/* Parse configuration */
	if ((ret = wtext->config_get(
	    wtext, session, config, "append", &v)) != 0) {
		ESET(wtext, session, ret,
		    "append configuration: %s",
		    wtext->strerror(wtext, session, ret));
		goto err;
	}
	cursor->config_append = v.val != 0;

	if ((ret = wtext->config_get(
	    wtext, session, config, "overwrite", &v)) != 0) {
		ESET(wtext, session, ret,
		    "overwrite configuration: %s",
		    wtext->strerror(wtext, session, ret));
		goto err;
	}
	cursor->config_overwrite = v.val != 0;

	if ((ret = wtext->config_get(
	    wtext, session, config, "key_format", &v)) != 0) {
		ESET(wtext, session, ret,
		    "key_format configuration: %s",
		    wtext->strerror(wtext, session, ret));
		goto err;
	}
	cursor->config_recno = v.len == 1 && v.str[0] == 'r';

	if ((ret = wtext->config_get(
	    wtext, session, config, "value_format", &v)) != 0) {
		ESET(wtext, session, ret,
		    "value_format configuration: %s",
		    wtext->strerror(wtext, session, ret));
		goto err;
	}
	cursor->config_bitfield =
	    v.len == 2 && isdigit((u_char)v.str[0]) && v.str[1] == 't';

	if ((ret = writelock(wtext, session, &ds->rwlock)) != 0)
		goto err;
	locked = 1;
				/* Open the Berkeley DB cursor */
	if ((ret = db_create(&cursor->db, ds->dbenv, 0)) != 0) {
		ESET(wtext,
		    session, WT_ERROR, "db_create: %s", db_strerror(ret));
		goto err;
	}
	db = cursor->db;
	if ((ret = db->open(db, NULL, name, NULL,
	    cursor->config_recno ? DB_RECNO : DB_BTREE, DB_CREATE, 0)) != 0) {
		ESET(wtext,
		    session, WT_ERROR, "Db.open: %s", db_strerror(ret));
		goto err;
	}
	if ((ret = db->cursor(db, NULL, &cursor->dbc, 0)) != 0) {
		ESET(wtext,
		    session, WT_ERROR, "Db.cursor: %s", db_strerror(ret));
		goto err;
	}

				/* Initialize the methods */
	cursor->wtcursor.next = kvs_cursor_next;
	cursor->wtcursor.prev = kvs_cursor_prev;
	cursor->wtcursor.reset = kvs_cursor_reset;
	cursor->wtcursor.search = kvs_cursor_search;
	cursor->wtcursor.search_near = kvs_cursor_search_near;
	cursor->wtcursor.insert = kvs_cursor_insert;
	cursor->wtcursor.update = kvs_cursor_update;
	cursor->wtcursor.remove = kvs_cursor_remove;
	cursor->wtcursor.close = kvs_cursor_close;

	*new_cursor = (WT_CURSOR *)cursor;

	++ds->open_cursors;

	if (0) {
err:		free(cursor);
	}

	if (locked)
		ETRET(unlock(wtext, session, &ds->rwlock));
	return (ret);
}

static int
kvs_session_rename(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    const char *uri, const char *newname, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	DB *db;
	WT_EXTENSION_API *wtext;
	int ret = 0;
	const char *name;

	(void)config;				/* Unused parameters */

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
						/* Get the object name */
	if ((ret = uri2name(wtext, session, uri, &name)) != 0)
		return (ret);

	if ((ret = single_thread(wtds, session, &ds->rwlock)) != 0)
		return (ret);

	if ((ret = db_create(&db, ds->dbenv, 0)) != 0)
		ESET(wtext,
		    session, WT_ERROR, "db_create: %s", db_strerror(ret));
	else if ((ret = db->rename(db, name, NULL, newname, 0)) != 0)
		ESET(wtext,
		    session, WT_ERROR, "Db.rename: %s", db_strerror(ret));
	/* db handle is dead */

	ETRET(unlock(wtext, session, &ds->rwlock));
	return (ret);
}

static int
kvs_session_truncate(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	DB *db;
	WT_EXTENSION_API *wtext;
	int tret, ret = 0;
	const char *name;

	(void)config;				/* Unused parameters */

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
						/* Get the object name */
	if ((ret = uri2name(wtext, session, uri, &name)) != 0)
		return (ret);

	if ((ret = single_thread(wtds, session, &ds->rwlock)) != 0)
		return (ret);

	if ((ret = db_create(&db, ds->dbenv, 0)) != 0)
		ESET(wtext,
		    session, WT_ERROR, "db_create: %s", db_strerror(ret));
	else {
		if ((ret = db->open(db,
		    NULL, name, NULL, DB_UNKNOWN, DB_TRUNCATE, 0)) != 0)
			ESET(wtext, session, WT_ERROR,
			    "Db.open: %s", db_strerror(ret));
		if ((tret = db->close(db, 0)) != 0)
			ESET(wtext, session, WT_ERROR,
			    "Db.close: %s", db_strerror(tret));
	}

	ETRET(unlock(wtext, session, &ds->rwlock));
	return (ret);
}

static int
kvs_session_verify(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	DB *db;
	WT_EXTENSION_API *wtext;
	int ret = 0;
	const char *name;

	(void)config;				/* Unused parameters */

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
						/* Get the object name */
	if ((ret = uri2name(wtext, session, uri, &name)) != 0)
		return (ret);

	if ((ret = single_thread(wtds, session, &ds->rwlock)) != 0)
		return (ret);

	if ((ret = db_create(&db, ds->dbenv, 0)) != 0)
		ESET(wtext,
		    session, WT_ERROR, "db_create: %s", db_strerror(ret));
	else if ((ret = db->verify(db, name, NULL, NULL, 0)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "Db.verify: %s: %s", uri, db_strerror(ret));
	/* db handle is dead */

	ETRET(unlock(wtext, session, &ds->rwlock));
	return (ret);
}

static int
kvs_terminate(WT_DATA_SOURCE *wtds, WT_SESSION *session)
{
	DB_ENV *dbenv;
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	int ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	dbenv = ds->dbenv;

	if (dbenv != NULL && (ret = dbenv->close(dbenv, 0)) != 0)
		ESET(wtext,
		    session, WT_ERROR, "DbEnv.close: %s", db_strerror(ret));

	ETRET(lock_destroy(wtext, session, &ds->rwlock));

	return (ret);
}

int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	/*
	 * List of the WT_DATA_SOURCE methods -- it's static so it breaks at
	 * compile-time should the structure changes underneath us.
	 */
	static WT_DATA_SOURCE wtds = {
		kvs_session_create,		/* session.create */
		NULL,				/* No session.compaction */
		kvs_session_drop,		/* session.drop */
		kvs_session_open_cursor,	/* session.open_cursor */
		kvs_session_rename,		/* session.rename */
		NULL,				/* No session.salvage */
		kvs_session_truncate,		/* session.truncate */
		NULL,				/* No range_truncate */
		kvs_session_verify,		/* session.verify */
		NULL,				/* session.checkpoint */
		kvs_terminate			/* termination */
	};
	DATA_SOURCE *ds;
	DB_ENV *dbenv;
	WT_EXTENSION_API *wtext;
	size_t len;
	int ret = 0;
	const char *home;
	char *path;

	(void)config;				/* Unused parameters */

	ds = NULL;
	dbenv = NULL;
	path = NULL;
						/* Acquire the extension API */
	wtext = connection->get_extension_api(connection);

	/* Allocate the local data-source structure. */
	if ((ds = calloc(1, sizeof(DATA_SOURCE))) == NULL)
		return (os_errno());
	ds->wtext = wtext;
						/* Configure the global lock */
	if ((ret = lock_init(wtext, NULL, &ds->rwlock)) != 0)
		goto err;

	ds->wtds = wtds;			/* Configure the methods */

						/* Berkeley DB environment */
	if ((ret = db_env_create(&dbenv, 0)) != 0) {
		ESET(wtext,
		    NULL, WT_ERROR, "db_env_create: %s", db_strerror(ret));
		goto err;
	}
	dbenv->set_errpfx(dbenv, "bdb");
	dbenv->set_errfile(dbenv, stderr);

	home = connection->get_home(connection);
	len = strlen(home) + 10;
	if ((path = malloc(len)) == NULL)
		goto err;
	(void)snprintf(path, len, "%s/KVS", home);
	if ((ret = dbenv->open(dbenv, path,
	    DB_CREATE | DB_INIT_LOCK | DB_INIT_MPOOL | DB_PRIVATE, 0)) != 0) {
		ESET(wtext, NULL, WT_ERROR, "DbEnv.open: %s", db_strerror(ret));
		goto err;
	}
	ds->dbenv = dbenv;

	if ((ret =				/* Add the data source */
	    connection->add_data_source(
	    connection, "kvsbdb:", (WT_DATA_SOURCE *)ds, NULL)) != 0) {
		ESET(wtext, NULL, ret, "WT_CONNECTION.add_data_source");
		goto err;
	}

	if (0) {
err:		if (dbenv != NULL)
			(void)dbenv->close(dbenv, 0);
		free(ds);
	}
	free(path);
	return (ret);
}

int
wiredtiger_extension_terminate(WT_CONNECTION *connection)
{
	(void)connection;			/* Unused parameters */

	return (0);
}
