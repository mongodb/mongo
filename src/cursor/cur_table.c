/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __curtable_open_indices(WT_CURSOR_TABLE *ctable);
static int __curtable_update(WT_CURSOR *cursor);

#define	APPLY_CG(ctable, f) do {					\
	WT_CURSOR **__cp;						\
	u_int __i;							\
	for (__i = 0, __cp = ctable->cg_cursors;			\
	    __i < WT_COLGROUPS(ctable->table);				\
	    __i++, __cp++)						\
		WT_TRET((*__cp)->f(*__cp));				\
} while (0)

#define	APPLY_IDX(ctable, f) do {					\
	WT_INDEX *idx;							\
	WT_CURSOR **__cp;						\
	u_int __i;							\
	WT_ERR(__curtable_open_indices(ctable));			\
	__cp = (ctable)->idx_cursors;					\
	for (__i = 0; __i < ctable->table->nindices; __i++, __cp++) {	\
		idx = ctable->table->indices[__i];			\
		WT_ERR(__wt_schema_project_merge(session,		\
		    ctable->cg_cursors,					\
		    idx->key_plan, idx->key_format, &(*__cp)->key));	\
		if (idx->need_value) {					\
			(*__cp)->value.data = "";			\
			(*__cp)->value.size = 1;			\
		}							\
		F_SET(*__cp, WT_CURSTD_KEY_APP |			\
		    WT_CURSTD_VALUE_APP);				\
		WT_ERR((*__cp)->f(*__cp));				\
	}								\
} while (0)

/*
 * __wt_curtable_get_key --
 *	WT_CURSOR->get_key implementation for tables.
 */
int
__wt_curtable_get_key(WT_CURSOR *cursor, ...)
{
	WT_CURSOR *primary;
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	va_list ap;

	ctable = (WT_CURSOR_TABLE *)cursor;
	primary = *ctable->cg_cursors;

	va_start(ap, cursor);
	ret = __wt_cursor_get_keyv(primary, cursor->flags, ap);
	va_end(ap);

	return (ret);
}

/*
 * __wt_curtable_get_value --
 *	WT_CURSOR->get_value implementation for tables.
 */
int
__wt_curtable_get_value(WT_CURSOR *cursor, ...)
{
	WT_CURSOR *primary;
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	WT_ITEM *item;
	WT_SESSION_IMPL *session;
	va_list ap;

	ctable = (WT_CURSOR_TABLE *)cursor;
	primary = *ctable->cg_cursors;
	CURSOR_API_CALL(cursor, session, get_value, NULL);
	WT_CURSOR_NEEDVALUE(primary);

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSOR_RAW_OK)) {
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

err:	API_END(session);
	return (ret);
}

/*
 * __wt_curtable_set_key --
 *	WT_CURSOR->set_key implementation for tables.
 */
void
__wt_curtable_set_key(WT_CURSOR *cursor, ...)
{
	WT_CURSOR **cp, *primary;
	WT_CURSOR_TABLE *ctable;
	va_list ap;
	u_int i;

	ctable = (WT_CURSOR_TABLE *)cursor;
	cp = ctable->cg_cursors;
	primary = *cp++;

	va_start(ap, cursor);
	__wt_cursor_set_keyv(primary, cursor->flags, ap);
	va_end(ap);

	if (!F_ISSET(primary, WT_CURSTD_KEY_SET))
		return;

	/* Copy the primary key to the other cursors. */
	for (i = 1; i < WT_COLGROUPS(ctable->table); i++, cp++) {
		(*cp)->recno = primary->recno;
		(*cp)->key.data = primary->key.data;
		(*cp)->key.size = primary->key.size;
		F_SET(*cp, WT_CURSTD_KEY_APP);
	}
}

/*
 * __wt_curtable_set_value --
 *	WT_CURSOR->set_value implementation for tables.
 */
void
__wt_curtable_set_value(WT_CURSOR *cursor, ...)
{
	WT_CURSOR **cp;
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	WT_ITEM *item;
	WT_SESSION_IMPL *session;
	va_list ap;
	u_int i;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, set_value, NULL);

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSOR_RAW_OK)) {
		item = va_arg(ap, WT_ITEM *);
		cursor->value.data = item->data;
		cursor->value.size = item->size;
		ret = __wt_schema_project_slice(session,
		    ctable->cg_cursors, ctable->plan, 0,
		    cursor->value_format, &cursor->value);
	} else
		ret = __wt_schema_project_in(session,
		    ctable->cg_cursors, ctable->plan, ap);
	va_end(ap);

	for (i = 0, cp = ctable->cg_cursors;
	    i < WT_COLGROUPS(ctable->table); i++, cp++)
		if (ret == 0)
			F_SET(*cp, WT_CURSTD_VALUE_APP);
		else {
			(*cp)->saved_err = ret;
			F_CLR(*cp, WT_CURSTD_VALUE_SET);
		}

err:	API_END(session);
}

/*
 * __curtable_compare --
 *	WT_CURSOR->compare implementation for tables.
 */
static int
__curtable_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	CURSOR_API_CALL(a, session, compare, NULL);

	/*
	 * Confirm both cursors refer to the same source and have keys, then
	 * call the underlying object's comparison routine.
	 */
	if (strcmp(a->uri, b->uri) != 0)
		WT_ERR_MSG(session, EINVAL,
		    "comparison method cursors must reference the same object");
	WT_CURSOR_NEEDKEY(WT_CURSOR_PRIMARY(a));
	WT_CURSOR_NEEDKEY(WT_CURSOR_PRIMARY(b));

	ret = WT_CURSOR_PRIMARY(a)->compare(
	    WT_CURSOR_PRIMARY(a), WT_CURSOR_PRIMARY(b), cmpp);

err:	API_END(session);
	return (ret);
}

/*
 * __curtable_next --
 *	WT_CURSOR->next method for the table cursor type.
 */
static int
__curtable_next(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, next, NULL);
	APPLY_CG(ctable, next);

err:	API_END(session);
	return (ret);
}

/*
 * __curtable_next_random --
 *	WT_CURSOR->insert method for the table cursor type when configured with
 * next_random.
 */
static int
__curtable_next_random(WT_CURSOR *cursor)
{
	WT_CURSOR *primary, **cp;
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	u_int i;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, next, NULL);
	cp = ctable->cg_cursors;

	/* Split out the first next, it retrieves the random record. */
	primary = *cp++;
	WT_ERR(primary->next(primary));

	/* Fill in the rest of the columns. */
	for (i = 1; i < WT_COLGROUPS(ctable->table); i++, cp++) {
		(*cp)->key.data = primary->key.data;
		(*cp)->key.size = primary->key.size;
		(*cp)->recno = primary->recno;
		F_SET(*cp, WT_CURSTD_KEY_APP);
		WT_ERR((*cp)->search(*cp));
	}

err:	API_END(session);
	return (ret);
}

/*
 * __curtable_prev --
 *	WT_CURSOR->prev method for the table cursor type.
 */
static int
__curtable_prev(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, prev, NULL);
	APPLY_CG(ctable, prev);

err:	API_END(session);
	return (ret);
}

/*
 * __curtable_reset --
 *	WT_CURSOR->reset method for the table cursor type.
 */
static int
__curtable_reset(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, reset, NULL);
	APPLY_CG(ctable, reset);

err:	API_END(session);
	return (ret);
}

/*
 * __curtable_search --
 *	WT_CURSOR->search method for the table cursor type.
 */
static int
__curtable_search(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, search, NULL);
	APPLY_CG(ctable, search);

err:	API_END(session);
	return (ret);
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
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	u_int i;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, search_near, NULL);
	cp = ctable->cg_cursors;
	primary = *cp;
	WT_ERR(primary->search_near(primary, exact));

	for (i = 1, ++cp; i < WT_COLGROUPS(ctable->table); i++) {
		(*cp)->key.data = primary->key.data;
		(*cp)->key.size = primary->key.size;
		(*cp)->recno = primary->recno;
		WT_ERR((*cp)->search(*cp));
	}

err:	API_END(session);
	return (ret);
}

/*
 * __curtable_insert --
 *	WT_CURSOR->insert method for the table cursor type.
 */
static int
__curtable_insert(WT_CURSOR *cursor)
{
	WT_CURSOR *primary, **cp;
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	u_int i;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_UPDATE_API_CALL(cursor, session, insert, NULL);
	cp = ctable->cg_cursors;

	/*
	 * Split out the first insert, it may be allocating a recno, and this
	 * is also the point at which we discover whether this is an overwrite.
	 */
	primary = *cp++;
	if ((ret = primary->insert(primary)) != 0) {
		if (ret == WT_DUPLICATE_KEY &&
		    F_ISSET(cursor, WT_CURSTD_OVERWRITE)) {
			/*
			 * !!! The insert failure clears these flags, but does
			 * not touch the items.  We could make a copy every time
			 * for overwrite cursors, but for now we just reset the
			 * flags.
			 */
			F_SET(primary, WT_CURSTD_KEY_APP | WT_CURSTD_VALUE_APP);
			ret = __curtable_update(cursor);
		}
		goto err;
	}

	for (i = 1; i < WT_COLGROUPS(ctable->table); i++, cp++) {
		(*cp)->recno = primary->recno;
		WT_ERR((*cp)->insert(*cp));
	}

	APPLY_IDX(ctable, insert);

err:	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __curtable_update --
 *	WT_CURSOR->update method for the table cursor type.
 */
static int
__curtable_update(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_UPDATE_API_CALL(cursor, session, update, NULL);
	WT_ERR(__curtable_open_indices(ctable));
	/*
	 * If the table has indices, first delete any old index keys, then
	 * update the primary, then insert the new index keys.  This is
	 * complicated by the fact that we need the old value to generate the
	 * old index keys, so we make a temporary copy of the new value.
	 */
	if (ctable->idx_cursors != NULL) {
		WT_ERR(__wt_schema_project_merge(session,
		    ctable->cg_cursors, ctable->plan,
		    cursor->value_format, &cursor->value));
		APPLY_CG(ctable, search);
		WT_ERR(ret);
		APPLY_IDX(ctable, remove);
		WT_ERR(__wt_schema_project_slice(session,
		    ctable->cg_cursors, ctable->plan, 0,
		    cursor->value_format, &cursor->value));
	}
	APPLY_CG(ctable, update);
	WT_ERR(ret);
	if (ctable->idx_cursors != NULL)
		APPLY_IDX(ctable, insert);

err:	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __curtable_remove --
 *	WT_CURSOR->remove method for the table cursor type.
 */
static int
__curtable_remove(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_UPDATE_API_CALL(cursor, session, remove, NULL);

	/* Find the old record so it can be removed from indices */
	WT_ERR(__curtable_open_indices(ctable));
	if (ctable->table->nindices > 0) {
		APPLY_CG(ctable, search);
		WT_ERR(ret);
		APPLY_IDX(ctable, remove);
	}

	APPLY_CG(ctable, remove);

err:	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __wt_curtable_truncate --
 *	WT_SESSION.truncate support when table cursors are specified.
 */
int
__wt_curtable_truncate(
    WT_SESSION_IMPL *session, WT_CURSOR *start, WT_CURSOR *stop)
{
	WT_CURSOR **list_start, **list_stop;
	WT_CURSOR_TABLE *ctable, *ctable_start, *ctable_stop;
	WT_DECL_ITEM(key);
	WT_DECL_RET;
	WT_ITEM raw;
	u_int i;
	int cmp;

	WT_RET(__wt_scr_alloc(session, 128, &key));

	/*
	 * Step through the cursor range, removing any indices.
	 *
	 * If there are indices, copy the key we're using to step through the
	 * cursor range (so we can reset the cursor to its original position),
	 * then remove all of the index records in the truncated range.  Get a
	 * raw copy of the key because it's simplest to do, but copy the key:
	 * all that happens underneath is the data and size fields are reset to
	 * reference the cursor's key, and in the case of record numbers, it's
	 * the cursor's recno buffer, which will be updated as we cursor through
	 * the object.
	 */
	if (start == NULL) {
		ctable = (WT_CURSOR_TABLE *)stop;
		WT_ERR(__curtable_open_indices(ctable));
		if (ctable->table->nindices > 0) {
			WT_ERR(__wt_cursor_get_raw_key(stop, &raw));
			WT_ERR(__wt_buf_set(session, key, raw.data, raw.size));

			do {
				APPLY_CG(ctable, search);
				WT_ERR(ret);
				APPLY_IDX(ctable, remove);
			} while ((ret = stop->prev(stop)) == 0);
			WT_ERR_NOTFOUND_OK(ret);
			ret = 0;

			__wt_cursor_set_raw_key(stop, key);
			APPLY_CG(ctable, search);
		}
	} else if (stop == NULL) {
		ctable = (WT_CURSOR_TABLE *)start;
		WT_ERR(__curtable_open_indices(ctable));
		if (ctable->table->nindices > 0) {
			WT_ERR(__wt_cursor_get_raw_key(start, &raw));
			WT_ERR(__wt_buf_set(session, key, raw.data, raw.size));

			do {
				APPLY_CG(ctable, search);
				WT_ERR(ret);
				APPLY_IDX(ctable, remove);
			} while ((ret = start->next(start)) == 0);
			WT_ERR_NOTFOUND_OK(ret);
			ret = 0;

			__wt_cursor_set_raw_key(start, key);
			APPLY_CG(ctable, search);
		}
	} else {
		ctable = (WT_CURSOR_TABLE *)start;
		WT_ERR(__curtable_open_indices(ctable));
		if (ctable->table->nindices > 0) {
			WT_ERR(__wt_cursor_get_raw_key(start, &raw));
			WT_ERR(__wt_buf_set(session, key, raw.data, raw.size));

			do {
				APPLY_CG(ctable, search);
				WT_ERR(ret);
				APPLY_IDX(ctable, remove);
				WT_ERR(start->compare(start, stop, &cmp));
			} while (cmp < 0 && (ret = start->next(start)) == 0);
			WT_ERR_NOTFOUND_OK(ret);
			ret = 0;

			__wt_cursor_set_raw_key(start, key);
			APPLY_CG(ctable, search);
		}
	}

	/*
	 * Truncate the column groups.
	 *
	 * Assumes the table's cursors have the same set of underlying objects,
	 * in the same order.
	 */
	if (start == NULL)
		for (i = 0,
		    ctable_stop = (WT_CURSOR_TABLE *)stop,
		    list_stop = ctable_stop->cg_cursors;
		    i < WT_COLGROUPS(ctable_stop->table);
		    i++, ++list_stop)
			WT_ERR(
			    __wt_curfile_truncate(session, NULL, *list_stop));
	else if (stop == NULL)
		for (i = 0,
		    ctable_start = (WT_CURSOR_TABLE *)start,
		    list_start = ctable_start->cg_cursors;
		    i < WT_COLGROUPS(ctable_start->table);
		    i++, ++list_start)
			WT_ERR(
			    __wt_curfile_truncate(session, *list_start, NULL));
	else {
		for (i = 0,
		    ctable_start = (WT_CURSOR_TABLE *)start,
		    list_start = ctable_start->cg_cursors,
		    ctable_stop = (WT_CURSOR_TABLE *)stop,
		    list_stop = ctable_stop->cg_cursors;
		    i < WT_COLGROUPS(ctable_start->table);
		    i++, ++list_start, ++list_stop)
			WT_ERR(__wt_curfile_truncate(
			    session, *list_start, *list_stop));
	}

err:	__wt_scr_free(&key);
	return (ret);
}

/*
 * __curtable_close --
 *	WT_CURSOR->close method for the table cursor type.
 */
static int
__curtable_close(WT_CURSOR *cursor)
{
	WT_CURSOR_TABLE *ctable;
	WT_CURSOR **cp;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	u_int i;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL(cursor, session, close, NULL);

	for (i = 0, cp = (ctable)->cg_cursors;
	    i < WT_COLGROUPS(ctable->table); i++, cp++)
		if (*cp != NULL) {
			WT_TRET((*cp)->close(*cp));
			*cp = NULL;
		}

	if (ctable->idx_cursors != NULL)
		for (i = 0, cp = (ctable)->idx_cursors;
		    i < ctable->table->nindices; i++, cp++)
			if (*cp != NULL) {
				WT_TRET((*cp)->close(*cp));
				*cp = NULL;
			}

	if (ctable->plan != ctable->table->plan)
		__wt_free(session, ctable->plan);
	if (cursor->value_format != ctable->table->value_format)
		__wt_free(session, cursor->value_format);
	__wt_free(session, ctable->cg_cursors);
	__wt_free(session, ctable->idx_cursors);
	/* The URI is owned by the table. */
	cursor->uri = NULL;
	WT_TRET(__wt_cursor_close(cursor));

err:	API_END(session);
	return (ret);
}

static int
__curtable_open_colgroups(WT_CURSOR_TABLE *ctable, const char *cfg_arg[])
{
	WT_SESSION_IMPL *session;
	WT_TABLE *table;
	WT_CURSOR **cp;
	/*
	 * Underlying column groups are always opened without dump or
	 * overwrite, and only the primary is opened with next_random.
	 */
	const char *cfg[] = {
		cfg_arg[0], cfg_arg[1], "dump=\"\",overwrite=false", NULL, NULL
	};
	u_int i;

	session = (WT_SESSION_IMPL *)ctable->iface.session;
	table = ctable->table;

	if (!table->cg_complete)
		WT_RET_MSG(session, EINVAL,
		    "Can't use '%s' until all column groups are created",
		    table->name);

	WT_RET(__wt_calloc_def(session,
	    WT_COLGROUPS(table), &ctable->cg_cursors));

	for (i = 0, cp = ctable->cg_cursors;
	    i < WT_COLGROUPS(table);
	    i++, cp++) {
		WT_RET(__wt_open_cursor(session, table->cgroups[i]->source,
		    &ctable->iface, cfg, cp));
		cfg[3] = "next_random=false";
	}
	return (0);
}

static int
__curtable_open_indices(WT_CURSOR_TABLE *ctable)
{
	WT_CURSOR **cp, *primary;
	WT_SESSION_IMPL *session;
	WT_TABLE *table;
	const char *cfg[] = API_CONF_DEFAULTS(session, open_cursor, NULL);
	u_int i;

	session = (WT_SESSION_IMPL *)ctable->iface.session;
	table = ctable->table;

	WT_RET(__wt_schema_open_indices(session, table));
	if (table->nindices == 0 || ctable->idx_cursors != NULL)
		return (0);

	/* Check for bulk cursors. */
	primary = *ctable->cg_cursors;
	if (F_ISSET(((WT_CURSOR_BTREE *)primary)->btree, WT_BTREE_BULK))
		WT_RET_MSG(session, ENOTSUP,
		    "Bulk load is not supported for tables with indices");
	WT_RET(__wt_calloc_def(session, table->nindices, &ctable->idx_cursors));

	for (i = 0, cp = ctable->idx_cursors; i < table->nindices; i++, cp++)
		WT_RET(__wt_open_cursor(session, table->indices[i]->source,
		    &ctable->iface, cfg, cp));
	return (0);
}

/*
 * __wt_curtable_open --
 *	WT_SESSION->open_cursor method for table cursors.
 */
int
__wt_curtable_open(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR **cursorp)
{
	WT_CURSOR_STATIC_INIT(iface,
	    __wt_curtable_get_key,		/* get-key */
	    __wt_curtable_get_value,		/* get-value */
	    __wt_curtable_set_key,		/* set-key */
	    __wt_curtable_set_value,		/* set-value */
	    __curtable_compare,			/* compare */
	    __curtable_next,			/* next */
	    __curtable_prev,			/* prev */
	    __curtable_reset,			/* reset */
	    __curtable_search,			/* search */
	    __curtable_search_near,		/* search-near */
	    __curtable_insert,			/* insert */
	    __curtable_update,			/* update */
	    __curtable_remove,			/* remove */
	    __curtable_close);			/* close */
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	WT_CURSOR_TABLE *ctable;
	WT_DECL_RET;
	WT_ITEM fmt, plan;
	WT_TABLE *table;
	size_t size;
	const char *tablename, *columns;

	WT_CLEAR(fmt);
	WT_CLEAR(plan);
	ctable = NULL;

	tablename = uri;
	if (!WT_PREFIX_SKIP(tablename, "table:"))
		return (EINVAL);
	columns = strchr(tablename, '(');
	if (columns == NULL)
		size = strlen(tablename);
	else
		size = WT_PTRDIFF(columns, tablename);
	WT_RET(__wt_schema_get_table(session, tablename, size, 0, &table));

	if (table->is_simple)
		/* Just return a cursor on the underlying data source. */
		return (__wt_open_cursor(session,
		    table->cgroups[0]->source, NULL, cfg, cursorp));

	WT_RET(__wt_calloc_def(session, 1, &ctable));

	cursor = &ctable->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->uri = table->name;
	cursor->key_format = table->key_format;
	cursor->value_format = table->value_format;

	ctable->table = table;
	ctable->plan = table->plan;

	/* Handle projections. */
	if (columns != NULL) {
		WT_ERR(__wt_struct_reformat(session, table,
		    columns, strlen(columns), NULL, 1, &fmt));
		cursor->value_format = __wt_buf_steal(session, &fmt, NULL);

		WT_ERR(__wt_struct_plan(session, table,
		    columns, strlen(columns), 0, &plan));
		ctable->plan = __wt_buf_steal(session, &plan, NULL);
	}

	/*
	 * random_retrieval
	 * Random retrieval cursors only support next, reset and close.
	 */
	WT_ERR(__wt_config_gets_defno(session, cfg, "next_random", &cval));
	if (cval.val != 0) {
		__wt_cursor_set_notsup(cursor);
		cursor->next = __curtable_next_random;
		cursor->reset = __curtable_reset;
	}

	STATIC_ASSERT(offsetof(WT_CURSOR_TABLE, iface) == 0);
	WT_ERR(__wt_cursor_init(cursor, cursor->uri, NULL, cfg, cursorp));

	/*
	 * Open the colgroup cursors immediately: we're going to need them for
	 * any operation.  We defer opening index cursors until we need them
	 * for an update.  Note that this must come after the call to
	 * __wt_cursor_init: the table cursor must already be on the list of
	 * session cursors or we can't work out where to put the colgroup
	 * cursor(s).
	 */
	WT_ERR(__curtable_open_colgroups(ctable, cfg));

	if (0) {
err:		WT_TRET(__curtable_close(cursor));
		*cursorp = NULL;
	}

	return (ret);
}
