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

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <build_unix/db.h>
#include <wiredtiger.h>

#ifndef	UINT32_MAX                      	/* Maximum 32-bit unsigned */
#define	UINT32_MAX	4294967295U
#endif

static DB_ENV *dbenv;				/* Enclosing environment */

typedef struct __data_source DATA_SOURCE;	/* XXX: May not need? */
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

	int	 append;			/* config "append" */
	int	 overwrite;			/* config "overwrite" */
	int	 recno;				/* config "key_format=r" */
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
recno_convert(WT_CURSOR *wt_cursor, uint32_t *recnop)
{
	CURSOR_SOURCE *cursor;
	WT_SESSION *session;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;

	if (wt_cursor->recno > UINT32_MAX) {
		(void)session->msg_printf(session,
		    "record number %" PRIuMAX ": %s",
		    (uintmax_t)wt_cursor->recno, strerror(ERANGE));
		return (ERANGE);
	}

	*recnop = (uint32_t)wt_cursor->recno;
	return (0);
}

static int
uri2name(WT_SESSION *session, const char *uri, const char **namep)
{
	const char *name;

	if ((name = strchr(uri, ':')) == NULL || *++name == '\0') {
		(void)session->msg_printf(
		    session, "unsupported object: %s", uri);
		return (EINVAL);
	}
	*namep = name;
	return (0);
}

/*! [Data-source: boolean configuration] */
static void
cfg_parse_bool(const char *cfg[], const char *match, int *valuep)
{
	const char *p;

	*valuep = 0;
	for (; (p = *cfg) != NULL; ++cfg)
		if ((p = strstr(p, match)) != NULL && p[strlen(match)] == '=') {
			*valuep = p[strlen(match) + 1] == '1' ? 1 : 0;
			return;
		}
}
/*! [Data-source: boolean configuration] */

/*! [Data-source: string configuration] */
static int
cfg_parse_str(
    WT_SESSION *session, const char *cfg[], const char *match, char **valuep)
{
	char *t;
	const char *p;

	*valuep = NULL;
	for (; (p = *cfg) != NULL; ++cfg)
		if ((p = strstr(p, match)) != NULL && p[strlen(match)] == '=') {
			/*
			 * Copy the configuration string's value and return it
			 * in allocated memory: our caller must free it.
			 */
			p += strlen(match) + 1;
			if ((*valuep = strdup(p)) == NULL) {
				(void)session->msg_printf(
				    session, "%s", strerror(errno));
				return (errno);
			}

			/* nul-terminate the string's value. */
			for (t = *valuep; *t != '\0' && *t != ','; ++t)
				;
			*t = '\0';
			return (0);
		}
	return (0);
}
/*! [Data-source: string configuration] */

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

/*! [Data-source: error message] */
	/*
	 * If the Berkeley DB function fails, log the error and then return an
	 * error in WiredTiger's name space.
	 */
	if ((ret = db->cursor(db, NULL, &dbc, 0)) != 0) {
		(void)session->msg_printf(
		    session, "Db.cursor: %s", db_strerror(ret));
		return (WT_ERROR);
	}
/*! [Data-source: error message] */

	printf("==> %s\n", tag);
	while ((ret = dbc->get(dbc, key, value, DB_NEXT)) == 0)
		if (cursor->recno)
			printf("\t%llu/%.*s\n",
			    (unsigned long long)*(db_recno_t *)key->data,
			    (int)value->size, (char *)value->data);
		else
			printf("\t%.*s/%.*s\n",
			    (int)key->size, (char *)key->data,
			    (int)value->size, (char *)value->data);

	if (ret != DB_NOTFOUND) {
		(void)session->msg_printf(
		    session, "DbCursor.get: %s", db_strerror(ret));
		return (WT_ERROR);
	}

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

	if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
		return (WT_NOTFOUND);
	(void)session->msg_printf(
	    session, "DbCursor.get: %s", db_strerror(ret));
	return (WT_ERROR);
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

	if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
		return (WT_NOTFOUND);
	(void)session->msg_printf(
	    session, "DbCursor.get: %s", db_strerror(ret));
	return (WT_ERROR);
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
		if ((ret = dbc->close(dbc)) != 0) {
			(void)session->msg_printf(
			    session, "DbCursor.close: %s", db_strerror(ret));
			return (WT_ERROR);
		}

		if ((ret =
		    cursor->db->cursor(cursor->db, NULL, &dbc, 0)) != 0) {
			(void)session->msg_printf(
			    session, "Db.cursor: %s", db_strerror(ret));
			return (WT_ERROR);
		}
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
	db_recno_t recno;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if (cursor->recno) {
		if ((ret = recno_convert(wt_cursor, &recno)) != 0)
			return (ret);
		key->data = &recno;
		key->size = sizeof(db_recno_t);
	} else {
		key->data = (char *)wt_cursor->key.data;
		key->size = wt_cursor->key.size;
	}

	if ((ret = dbc->get(dbc, key, value, DB_SET)) == 0) {
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

	if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
		return (WT_NOTFOUND);
	(void)session->msg_printf(
	    session, "DbCursor.get: %s", db_strerror(ret));
	return (WT_ERROR);
}

static int
kvs_cursor_search_near(WT_CURSOR *wt_cursor, int *exact)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_SESSION *session;
	db_recno_t recno;
	int ret;

	(void)exact;				/* Unused parameters */

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if (cursor->recno) {
		if ((ret = recno_convert(wt_cursor, &recno)) != 0)
			return (ret);
		key->data = &recno;
		key->size = sizeof(db_recno_t);
	} else {
		key->data = (char *)wt_cursor->key.data;
		key->size = wt_cursor->key.size;
	}

	if ((ret = dbc->get(dbc, key, value, DB_SET_RANGE)) == 0) {
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

	if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
		return (WT_NOTFOUND);
	(void)session->msg_printf(
	    session, "DbCursor.get: %s", db_strerror(ret));
	return (WT_ERROR);
}

static int
kvs_cursor_insert(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DB *db;
	DBC *dbc;
	DBT *key, *value;
	WT_SESSION *session;
	db_recno_t recno;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;
	dbc = cursor->dbc;
	db = cursor->db;
	key = &cursor->key;
	value = &cursor->value;

	if (cursor->recno) {
		if ((ret = recno_convert(wt_cursor, &recno)) != 0)
			return (ret);
		key->data = &recno;
		key->size = sizeof(db_recno_t);
	} else {
		key->data = (char *)wt_cursor->key.data;
		key->size = wt_cursor->key.size;
	}
	value->data = (char *)wt_cursor->value.data;
	value->size = wt_cursor->value.size;

	if (cursor->append) {
		/*
		 * Berkeley DB cursors have no operation to append/create a
		 * new record and set the cursor; use the DB handle instead
		 * then set the cursor explicitly.
		 *
		 * When appending, we're allocating and returning a new record
		 * number.
		 */
		if ((ret = db->put(db, NULL, key, value, DB_APPEND)) != 0) {
			(void)session->msg_printf(
			    session, "Db.put: %s", db_strerror(ret));
			return (WT_ERROR);
		}
		wt_cursor->recno = *(db_recno_t *)key->data;

		if ((ret = dbc->get(dbc, key, value, DB_SET)) != 0) {
			(void)session->msg_printf(
			    session, "DbCursor.get: %s", db_strerror(ret));
			return (WT_ERROR);
		}
	} else if (cursor->overwrite) {
		if ((ret = dbc->put(dbc, key, value, DB_KEYFIRST)) != 0) {
			(void)session->msg_printf(
			    session, "DbCursor.put: %s", db_strerror(ret));
			return (WT_ERROR);
		}
	} else {
		/*
		 * Berkeley DB cursors don't have a no-overwrite flag; use
		 * the DB handle instead then set the cursor explicitly.
		 */
		if ((ret =
		    db->put(db, NULL, key, value, DB_NOOVERWRITE)) != 0) {
			if (ret == DB_KEYEXIST)
				return (WT_DUPLICATE_KEY);

			(void)session->msg_printf(
			    session, "Db.put: %s", db_strerror(ret));
			return (WT_ERROR);
		}

		if ((ret = dbc->get(dbc, key, value, DB_SET)) != 0) {
			(void)session->msg_printf(
			    session, "DbCursor.get: %s", db_strerror(ret));
			return (WT_ERROR);
		}
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
	db_recno_t recno;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if (cursor->recno) {
		if ((ret = recno_convert(wt_cursor, &recno)) != 0)
			return (ret);
		key->data = &recno;
		key->size = sizeof(db_recno_t);
	} else {
		key->data = (char *)wt_cursor->key.data;
		key->size = wt_cursor->key.size;
	}
	value->data = (char *)wt_cursor->value.data;
	value->size = wt_cursor->value.size;

	if ((ret = dbc->put(dbc, key, value, DB_KEYFIRST)) != 0) {
		(void)session->msg_printf(
		    session, "DbCursor.put: %s", db_strerror(ret));
		return (WT_ERROR);
	}

	return (0);
}

static int
kvs_cursor_remove(WT_CURSOR *wt_cursor)
{
	CURSOR_SOURCE *cursor;
	DBC *dbc;
	DBT *key, *value;
	WT_SESSION *session;
	db_recno_t recno;
	int ret;

	cursor = (CURSOR_SOURCE *)wt_cursor;
	session = cursor->session;
	dbc = cursor->dbc;
	key = &cursor->key;
	value = &cursor->value;

	if (cursor->recno) {
		if ((ret = recno_convert(wt_cursor, &recno)) != 0)
			return (ret);
		key->data = &recno;
		key->size = sizeof(db_recno_t);
	} else {
		key->data = (char *)wt_cursor->key.data;
		key->size = wt_cursor->key.size;
	}

	if ((ret = dbc->get(dbc, key, value, DB_SET)) != 0) {
		if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY)
			return (WT_NOTFOUND);
		(void)session->msg_printf(
		    session, "DbCursor.get: %s", db_strerror(ret));
		return (WT_ERROR);
	}
	if ((ret = dbc->del(dbc, 0)) != 0) {
		(void)session->msg_printf(
		    session, "DbCursor.del: %s", db_strerror(ret));
		return (WT_ERROR);
	}

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
	if (dbc != NULL && (tret = dbc->close(dbc)) != 0) {
		(void)session->msg_printf(
		    session, "DbCursor.close: %s", db_strerror(tret));
		ret = WT_ERROR;
	}

	db = cursor->db;
	cursor->db = NULL;
	if (db != NULL && (tret = db->close(db, 0)) != 0) {
		(void)session->msg_printf(
		    session, "Db.close: %s", db_strerror(tret));
		ret = WT_ERROR;
	}
	free(wt_cursor);

	lock();
	--data->open_cursors;
	unlock();

	return (ret);
}

static int
kvs_create(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, int exclusive, const char *cfg[])
{
	DB *db;
	DBTYPE type;
	uint32_t flags;
	int ret, tret;
	char *p;
	const char *name;

	(void)dsrc;				/* Unused parameters */

						/* Get the object name */
	if ((ret = uri2name(session, uri, &name)) != 0)
		return (ret);

	if ((ret = cfg_parse_str(session, cfg, "key_format", &p)) != 0)
		return (ret);
	type = strcmp(p, "r") == 0 ? DB_RECNO : DB_BTREE;
	free(p);

	if ((ret = cfg_parse_str(session, cfg, "value_format", &p)) != 0)
		return (ret);
	if (isdigit(p[0]) && p[1] == 't' && p[2] == '\0') {
		(void)session->msg_printf(session,
		    "kvs_create: unsupported value format %s", p);
		ret = EINVAL;
	}
	free(p);
	if (ret != 0)
		return (ret);

	flags = DB_CREATE | (exclusive ? DB_EXCL : 0);

	ret = 0;			/* Create the Berkeley DB table */
	if ((tret = db_create(&db, dbenv, 0)) != 0) {
		(void)session->msg_printf(
		    session, "db_create: %s", db_strerror(tret));
		return (WT_ERROR);
	}
	if ((tret = db->open(db, NULL, name, NULL, type, flags, 0)) != 0) {
		(void)session->msg_printf(
		    session, "Db.open: %s", uri, db_strerror(tret));
		ret = WT_ERROR;
	}
	if ((tret = db->close(db, 0)) != 0) {
		(void)session->msg_printf
		    (session, "Db.close", db_strerror(tret));
		ret = WT_ERROR;
	}
	return (ret);
}

static int
kvs_drop(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *cfg[])
{
	DB *db;
	int ret, tret;
	const char *name;

	(void)cfg;				/* Unused parameters */

						/* Get the object name */
	if ((ret = uri2name(session, uri, &name)) != 0)
		return (ret);

	if ((ret = single_thread(dsrc)) != 0)
		return (ret);

	ret = 0;
	if ((tret = db_create(&db, dbenv, 0)) != 0) {
		(void)session->msg_printf(
		    session, "db_create: %s", db_strerror(tret));
		ret = WT_ERROR;
	} else if ((tret = db->remove(db, name, NULL, 0)) != 0) {
		(void)session->msg_printf(
		    session, "Db.remove: %s", db_strerror(tret));
		ret = WT_ERROR;
	}
	/* db handle is dead */

	unlock();
	return (ret);
}

static int
kvs_open_cursor(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *cfg[], WT_CURSOR **new_cursor)
{
	CURSOR_SOURCE *cursor;
	DATA_SOURCE *data;
	DB *db;
	int ret, tret;
	char *key_format;
	const char *name;

	(void)cfg;				/* Unused parameters */

	data = (DATA_SOURCE *)dsrc;
						/* Get the object name */
	if ((ret = uri2name(session, uri, &name)) != 0)
		return (ret);
						/* Allocate the cursor */
	cursor = calloc(1, sizeof(CURSOR_SOURCE));
	cursor->session = session;
	cursor->dsrc = dsrc;
						/* Parse configuration */
/*! [Data-source: open_cursor configuration] */
	cfg_parse_bool(cfg, "append", &cursor->append);
	cfg_parse_bool(cfg, "overwrite", &cursor->overwrite);
	if ((ret = cfg_parse_str(session, cfg, "key_format", &key_format)) != 0)
		goto err;
	cursor->recno = strcmp(key_format, "r") == 0;
	free(key_format);
/*! [Data-source: open_cursor configuration] */

	lock();
				/* Open the Berkeley DB cursor */
	if ((tret = db_create(&cursor->db, dbenv, 0)) != 0) {
		(void)session->msg_printf(
		    session, "db_create: %s", db_strerror(tret));
		ret = WT_ERROR;
		goto err;
	}
	db = cursor->db;
	if ((tret = db->open(db, NULL, name,
	    NULL, cursor->recno ? DB_RECNO : DB_BTREE, DB_CREATE, 0)) != 0) {
		(void)session->msg_printf(
		    session, "Db.open: %s", db_strerror(tret));
		ret = WT_ERROR;
		goto err;
	}
	if ((tret = db->cursor(db, NULL, &cursor->dbc, 0)) != 0) {
		(void)session->msg_printf(
		    session, "Db.cursor: %s", db_strerror(tret));
		ret = WT_ERROR;
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

	unlock();
	return (ret);
}

static int
kvs_rename(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *newname, const char *cfg[])
{
	DB *db;
	int ret, tret;
	const char *name;

	(void)cfg;				/* Unused parameters */

						/* Get the object name */
	if ((ret = uri2name(session, uri, &name)) != 0)
		return (ret);

	if ((ret = single_thread(dsrc)) != 0)
		return (ret);

	ret = 0;
	if ((tret = db_create(&db, dbenv, 0)) != 0) {
		(void)session->msg_printf(
		    session, "db_create: %s", db_strerror(tret));
		ret = WT_ERROR;
	} else if ((tret = db->rename(db, name, NULL, newname, 0)) != 0) {
		(void)session->msg_printf(
		    session, "Db.rename: %s", db_strerror(tret));
		ret = WT_ERROR;
	}
	/* db handle is dead */

	unlock();
	return (ret);
}

static int
kvs_truncate(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *cfg[])
{
	DB *db;
	int ret, tret;
	const char *name;

	(void)cfg;				/* Unused parameters */

						/* Get the object name */
	if ((ret = uri2name(session, uri, &name)) != 0)
		return (ret);

	if ((ret = single_thread(dsrc)) != 0)
		return (ret);

	ret = 0;
	if ((tret = db_create(&db, dbenv, 0)) != 0) {
		(void)session->msg_printf(
		    session, "db_create: %s", db_strerror(tret));
		ret = WT_ERROR;
		goto err;
	}
	if ((tret = db->open(db,
	    NULL, name, NULL, DB_UNKNOWN, DB_TRUNCATE, 0)) != 0) {
		(void)session->msg_printf(
		    session, "Db.open: %s", db_strerror(tret));
		ret = WT_ERROR;
	}
	if ((tret = db->close(db, 0)) != 0) {
		(void)session->msg_printf(
		    session, "Db.close: %s", db_strerror(tret));
		ret = WT_ERROR;
	}

err:	unlock();
	return (ret);
}

static int
kvs_verify(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *cfg[])
{
	DB *db;
	int ret, tret;
	const char *name;

	(void)cfg;				/* Unused parameters */

						/* Get the object name */
	if ((ret = uri2name(session, uri, &name)) != 0)
		return (ret);

	if ((ret = single_thread(dsrc)) != 0)
		return (ret);

	ret = 0;
	if ((tret = db_create(&db, dbenv, 0)) != 0) {
		(void)session->msg_printf(
		    session, "db_create: %s", db_strerror(tret));
		ret = WT_ERROR;
	} else if ((tret = db->verify(db, name, NULL, NULL, 0)) != 0) {
		(void)db->close(db, 0);

		(void)session->msg_printf(session,
		    "Db.verify: %s: %s", uri, db_strerror(tret));
		ret = WT_ERROR;
	}
	/* db handle is dead */

	unlock();
	return (ret);
}

void die(int, const char *, ...);

void
kvsbdb_init(WT_CONNECTION *conn, const char *dir)
{
	int ret;

	memset(&ds, 0, sizeof(ds));

	if (pthread_rwlock_init(&ds.rwlock, NULL) != 0)
		die(errno, "pthread_rwlock_init");

	if ((ret = db_env_create(&dbenv, 0)) != 0)
		die(0, "db_env_create: %s", db_strerror(ret));
	dbenv->set_errpfx(dbenv, "bdb");
	dbenv->set_errfile(dbenv, stderr);
	if ((ret = dbenv->open(dbenv, dir,
	    DB_CREATE | DB_INIT_LOCK | DB_INIT_MPOOL | DB_PRIVATE, 0)) != 0)
		die(0, "DbEnv.open: %s", db_strerror(ret));

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
		die(ret, "WT_CONNECTION.add_data_source");
}

void
kvsbdb_close(WT_CONNECTION *conn)
{
	int ret;

	(void)conn;				/* Unused parameters */

	 (void)pthread_rwlock_destroy(&ds.rwlock);

	if (dbenv != NULL && (ret = dbenv->close(dbenv, 0)) != 0)
		die(0, "DB_ENV.close: %s", db_strerror(ret));
}
