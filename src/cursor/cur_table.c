/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __curtable_open_indices(WT_CURSOR_TABLE *ctable);

#define	APPLY_CG(ctable, f) do {					\
	WT_CURSOR **__cp;						\
	int __i;							\
	for (__i = 0, __cp = ctable->cg_cursors;			\
	     __i < ctable->table->ncolgroups;				\
	     __i++, __cp++)						\
		WT_RET((*__cp)->f(*__cp));				\
} while (0)

#define	APPLY_IDX(ctable, f) do {					\
	WT_CURSOR **__cp;						\
	int __i;							\
	if (ctable->table->nindices == 0)				\
		break;							\
	if ((__cp = (ctable)->idx_cursors) == NULL) {			\
		WT_RET(__curtable_open_indices(ctable));		\
		__cp = (ctable)->idx_cursors;				\
	}								\
	for (__i = 0; __i < ctable->table->nindices; __i++, __cp++)	\
		WT_RET((*__cp)->f(*__cp));				\
} while (0)

/*
 * __curtable_get_key --
 *	WT_CURSOR->get_key implementation for tables.
 */
static int
__curtable_get_key(WT_CURSOR *cursor, ...)
{
	WT_CURSOR *primary;
	WT_CURSOR_TABLE *ctable;
	WT_SESSION_IMPL *session;
	const char *fmt;
	va_list ap;
	int ret;

	CURSOR_API_CALL(cursor, session, get_key, NULL);
	ctable = (WT_CURSOR_TABLE *)cursor;
	primary = *ctable->cg_cursors;

	va_start(ap, cursor);
	fmt = F_ISSET(cursor, WT_CURSTD_RAW) ? "u" : cursor->key_format;
	ret = __wt_struct_unpackv(session,
	    primary->key.data, primary->key.size, fmt, ap);
	va_end(ap);

	API_END(session);
	return (ret);
}

/*
 * __curtable_get_value --
 *	WT_CURSOR->get_value implementation for tables.
 */
static int
__curtable_get_value(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_TABLE *ctable;
	WT_ITEM *item;
	WT_SESSION_IMPL *session;
	va_list ap;
	int ret;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, set_value, NULL);

	if (!F_SET(cursor, WT_CURSTD_VALUE_SET)) {
		__wt_errx(session, "Value not set");
		return (EINVAL);
	}

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		ret = __wt_schema_project_merge(session,
		    ctable->cg_cursors, ctable->plan,
		    cursor->value_format, &cursor->value);
		if (ret == 0) {
			item = va_arg(ap, WT_ITEM *);
			item->data = cursor->value.data;
			item->size = cursor->value.size;
		}
	} else
		ret = __wt_schema_project_out(session,
		    ctable->cg_cursors, ctable->plan, ap);
	va_end(ap);

	API_END(session);

	return (ret);
}

/*
 * __curtable_set_key --
 *	WT_CURSOR->set_key implementation for tables.
 */
static void
__curtable_set_key(WT_CURSOR *cursor, ...)
{
	WT_BUF *buf;
	WT_CURSOR **cp;
	WT_CURSOR_TABLE *ctable;
	WT_ITEM *item;
	WT_SESSION_IMPL *session;
	const char *fmt, *str;
	size_t sz;
	va_list ap;
	int i, ret;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, set_key, NULL);

	va_start(ap, cursor);
	fmt = F_ISSET(cursor, WT_CURSTD_RAW) ? "u" : cursor->key_format;
	/* Fast path some common cases: single strings or byte arrays. */
	if (fmt[0] == 'r' && fmt[1] == '\0') {
		cursor->recno = va_arg(ap, uint64_t);
		cursor->key.data = &cursor->recno;
		sz = sizeof(cursor->recno);
	} else if (fmt[0] == 'S' && fmt[1] == '\0') {
		str = va_arg(ap, const char *);
		sz = strlen(str) + 1;
		cursor->key.data = (void *)str;
	} else if (fmt[0] == 'u' && fmt[1] == '\0') {
		item = va_arg(ap, WT_ITEM *);
		sz = item->size;
		cursor->key.data = (void *)item->data;
	} else {
		buf = &cursor->key;
		sz = __wt_struct_sizev(session, cursor->key_format, ap);
		va_end(ap);
		va_start(ap, cursor);
		if ((ret = __wt_buf_initsize(session, buf, sz)) == 0 &&
		    (ret = __wt_struct_packv(session, buf->mem, sz,
		    cursor->key_format, ap)) == 0)
			F_SET(cursor, WT_CURSTD_KEY_SET);
		else {
			cursor->saved_err = ret;
			F_CLR(cursor, WT_CURSTD_KEY_SET);
			return;
		}
	}
	WT_ASSERT(session, sz <= UINT32_MAX);
	cursor->key.size = (uint32_t)sz;
	va_end(ap);

	for (i = 0, cp = ctable->cg_cursors;
	     i < ctable->table->ncolgroups;
	     i++, cp++) {
		(*cp)->recno = cursor->recno;
		(*cp)->key.data = cursor->key.data;
		(*cp)->key.size = cursor->key.size;
	}

	API_END(session);
}

/*
 * __curtable_set_value --
 *	WT_CURSOR->set_value implementation for tables.
 */
static void
__curtable_set_value(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_TABLE *ctable;
	WT_ITEM *item;
	WT_SESSION_IMPL *session;
	va_list ap;
	int ret;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, set_value, NULL);

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		item = va_arg(ap, WT_ITEM *);
		cursor->value.data = item->data;
		cursor->value.size = item->size;
		ret = __wt_schema_project_slice(session,
		    ctable->cg_cursors, ctable->plan,
		    cursor->value_format, (WT_ITEM *)&cursor->value);
	} else
		ret = __wt_schema_project_in(session,
		    ctable->cg_cursors, ctable->plan, ap);
	va_end(ap);

	if (ret == 0)
		F_SET(cursor, WT_CURSTD_VALUE_SET);
	else {
		cursor->saved_err = ret;
		F_CLR(cursor, WT_CURSTD_VALUE_SET);
		return;
	}

	API_END(session);
}

/*
 * __curtable_first --
 *	WT_CURSOR->first method for the table cursor type.
 */
static int
__curtable_first(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_SESSION_IMPL *session;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, next, NULL);
	APPLY_CG(ctable, first);
	API_END(session);

	return (0);
}

/*
 * __curtable_last --
 *	WT_CURSOR->last method for the table cursor type.
 */
static int
__curtable_last(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_SESSION_IMPL *session;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, last, NULL);
	APPLY_CG(ctable, last);
	API_END(session);

	return (0);
}

/*
 * __curtable_next --
 *	WT_CURSOR->next method for the table cursor type.
 */
static int
__curtable_next(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_SESSION_IMPL *session;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, next, NULL);
	APPLY_CG(ctable, next);
	API_END(session);

	return (0);
}

/*
 * __curtable_prev --
 *	WT_CURSOR->prev method for the table cursor type.
 */
static int
__curtable_prev(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_SESSION_IMPL *session;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, prev, NULL);
	APPLY_CG(ctable, prev);
	API_END(session);

	return (0);
}

/*
 * __curtable_search_near --
 *	WT_CURSOR->search_near method for the table cursor type.
 */
static int
__curtable_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_CURSOR_TABLE *ctable;
	WT_CURSOR *primary, **cp;
	WT_SESSION_IMPL *session;
	int i;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, search_near, NULL);
	cp = ctable->cg_cursors;
	primary = *cp;
	WT_RET(primary->search_near(primary, exact));

	for (i = 1, ++cp; i < ctable->table->ncolgroups; i++) {
		(*cp)->key.data = primary->key.data;
		(*cp)->key.size = primary->key.size;
		(*cp)->recno = primary->recno;
		WT_RET((*cp)->search(*cp));
	}
	API_END(session);

	return (0);
}

/*
 * __curtable_insert --
 *	WT_CURSOR->insert method for the table cursor type.
 */
static int
__curtable_insert(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_CURSOR *primary, **cp;
	WT_SESSION_IMPL *session;
	int i;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, insert, NULL);
	cp = ctable->cg_cursors;
	primary = *cp++;
	WT_RET(primary->insert(primary));

	for (i = 1; i < ctable->table->ncolgroups; i++, cp++) {
		(*cp)->recno = primary->recno;
		WT_RET((*cp)->insert(*cp));
	}
	API_END(session);

	return (0);
}

/*
 * __curtable_update --
 *	WT_CURSOR->update method for the table cursor type.
 */
static int
__curtable_update(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_SESSION_IMPL *session;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, update, NULL);
	APPLY_CG(ctable, update);
	APPLY_IDX(ctable, update);
	API_END(session);

	return (0);
}

/*
 * __curtable_remove --
 *	WT_CURSOR->remove method for the table cursor type.
 */
static int
__curtable_remove(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_SESSION_IMPL *session;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, remove, NULL);
	APPLY_CG(ctable, remove);
	APPLY_IDX(ctable, remove);
	API_END(session);

	return (0);
}

/*
 * __curtable_close --
 *	WT_CURSOR->close method for the table cursor type.
 */
static int
__curtable_close(WT_CURSOR *cursor, const char *config)
{
	WT_CURSOR_TABLE *ctable;
	WT_CURSOR **cp;
	WT_SESSION_IMPL *session;
	int i, ret;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL_CONF(cursor, session, close, NULL, config, cfg);
	WT_UNUSED(cfg);

	ret = 0;
	for (i = 0, cp = (ctable)->cg_cursors;
	    i < ctable->table->ncolgroups; i++, cp++)
		if (*cp != NULL) {
			WT_TRET((*cp)->close(*cp, config));
			*cp = NULL;
		}

	for (i = 0, cp = (ctable)->idx_cursors;
	    i < ctable->table->nindices; i++, cp++)
		if (*cp != NULL) {
			WT_TRET((*cp)->close(*cp, config));
			*cp = NULL;
		}

	if (ctable->plan != ctable->table->plan)
		__wt_free(session, ctable->plan);
	WT_TRET(__wt_cursor_close(cursor, config));
	API_END(session);

	return (ret);
}

static int
__curtable_open_colgroups(WT_CURSOR_TABLE *ctable, const char *config)
{
	WT_SESSION_IMPL *session;
	WT_TABLE *table;
	WT_CURSOR **cp;
	int i;

	session = (WT_SESSION_IMPL *)ctable->iface.session;
	table = ctable->table;

	if (!table->is_complete) {
		__wt_errx(session,
		    "Can't use table '%s' until all column groups are created",
		    table->name);
		return (EINVAL);
	}

	WT_ASSERT(session, table->ncolgroups > 0);
	WT_RET(__wt_calloc_def(session,
	    table->ncolgroups, &ctable->cg_cursors));

	for (i = 0, cp = ctable->cg_cursors; i < table->ncolgroups; i++, cp++) {
		session->btree = table->colgroup[i];
		WT_RET(__wt_curfile_create(session, 0, config, cp));
	}
	return (0);
}

static int
__curtable_open_indices(WT_CURSOR_TABLE *ctable)
{
	WT_SESSION_IMPL *session;
	WT_TABLE *table;

	session = (WT_SESSION_IMPL *)ctable->iface.session;
	table = ctable->table;

	if (table->nindices == 0)
		return (0);
	WT_RET(__wt_calloc_def(session, table->nindices, &ctable->idx_cursors));
	return (0);
}

/*
 * __wt_curtable_open --
 *	WT_SESSION->open_cursor method for table cursors.
 */
int
__wt_curtable_open(WT_SESSION_IMPL *session,
    const char *uri, const char *config, WT_CURSOR **cursorp)
{
	static WT_CURSOR iface = {
		NULL,
		NULL,
		NULL,
		__curtable_get_key,
		__curtable_get_value,
		__curtable_set_key,
		__curtable_set_value,
		__curtable_first,
		__curtable_last,
		__curtable_next,
		__curtable_prev,
		NULL,
		__curtable_search_near,
		__curtable_insert,
		__curtable_update,
		__curtable_remove,
		__curtable_close,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	const char *tablename, *columns;
	WT_BUF fmt, plan;
	WT_CURSOR_TABLE *ctable;
	WT_CURSOR *cursor;
	WT_TABLE *table;
	size_t size;
	uint32_t bufsz;
	int ret;

	WT_CLEAR(fmt);
	WT_CLEAR(plan);
	tablename = uri;
	if (!WT_PREFIX_SKIP(tablename, "table:"))
		return (EINVAL);
	columns = strchr(tablename, '(');
	if (columns == NULL)
		size = strlen(tablename);
	else
		size = columns - tablename;
	if ((ret = __wt_schema_get_table(session,
	    tablename, size, &table)) != 0) {
		if (ret == WT_NOTFOUND) {
			__wt_errx(session,
			    "Cannot open cursor '%s' on unknown table", uri);
			ret = EINVAL;
		}
		return (ret);
	}

	if (!table->is_complete) {
		__wt_errx(session,
		    "Cannot open cursor '%s' on incomplete table", uri);
		return (EINVAL);
	} else if (table->is_simple) {
		session->btree = table->colgroup[0];
		return (__wt_curfile_create(session, 0, config, cursorp));
	}

	WT_RET(__wt_calloc_def(session, 1, &ctable));

	cursor = &ctable->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->key_format = table->key_format;
	cursor->value_format = table->value_format;
	ctable->table = table;
	ctable->plan = table->plan;

	/* Handle projections. */
	if (columns != NULL) {
		WT_ERR(__wt_struct_reformat(session, table,
		    columns, strlen(columns), 1, &fmt));
		__wt_buf_steal(session, &fmt, &cursor->value_format, &bufsz);

		WT_ERR(__wt_struct_plan(session, table,
		    columns, strlen(columns), &plan));
		__wt_buf_steal(session, &plan, &ctable->plan, &bufsz);
	}

	/*
	 * Open the colgroup cursors immediately: we're going to need them for
	 * any operation.  We defer opening index cursors until we need them
	 * for an update.
	 */
	WT_ERR(__curtable_open_colgroups(ctable, config));

	STATIC_ASSERT(offsetof(WT_CURSOR_TABLE, iface) == 0);
	__wt_cursor_init(cursor, 1, config);
	*cursorp = cursor;

	if (0) {
err:		__wt_free(session, ctable);
		__wt_buf_free(session, &fmt);
		__wt_buf_free(session, &plan);
	}

	return (ret);
}

