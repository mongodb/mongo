/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __curtable_open_indices(WT_CURSOR_TABLE *ctable);
static int __curtable_update(WT_CURSOR *cursor);

#define	APPLY_CG(ctable, f) do {					\
	WT_CURSOR **__cp;						\
	int __i;							\
	for (__i = 0, __cp = ctable->cg_cursors;			\
	     __i < WT_COLGROUPS(ctable->table);				\
	     __i++, __cp++)						\
		WT_TRET((*__cp)->f(*__cp));				\
} while (0)

#define	APPLY_IDX(ctable, f) do {					\
	WT_BTREE *btree;						\
	WT_CURSOR **__cp;						\
	int __i;							\
	WT_ERR(__curtable_open_indices(ctable));			\
	__cp = (ctable)->idx_cursors;					\
	for (__i = 0; __i < ctable->table->nindices; __i++, __cp++) {	\
		btree = ((WT_CURSOR_BTREE *)*__cp)->btree;		\
		WT_ERR(__wt_schema_project_merge(session,		\
		    ctable->cg_cursors,					\
		    btree->key_plan, btree->key_format, &(*__cp)->key));\
		F_SET(*__cp, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);	\
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
	CURSOR_API_CALL_NOCONF(cursor, session, get_value, NULL);
	WT_CURSOR_NEEDVALUE(primary);

	va_start(ap, cursor);
	if (F_ISSET(cursor,
	    WT_CURSTD_DUMP_HEX | WT_CURSTD_DUMP_PRINT | WT_CURSTD_RAW)) {
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
	int i;

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
		F_SET(*cp, WT_CURSTD_KEY_SET);
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
	int i;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, set_value, NULL);

	va_start(ap, cursor);
	if (F_ISSET(cursor,
	    WT_CURSTD_DUMP_HEX | WT_CURSTD_DUMP_PRINT | WT_CURSTD_RAW)) {
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
	     i < WT_COLGROUPS(ctable->table);
	     i++, cp++)
		if (ret == 0)
			F_SET(*cp, WT_CURSTD_VALUE_SET);
		else {
			(*cp)->saved_err = ret;
			F_CLR(*cp, WT_CURSTD_VALUE_SET);
		}

	API_END(session);
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
	CURSOR_API_CALL_NOCONF(cursor, session, next, NULL);
	APPLY_CG(ctable, next);
	API_END(session);

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
	CURSOR_API_CALL_NOCONF(cursor, session, prev, NULL);
	APPLY_CG(ctable, prev);
	API_END(session);

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
	CURSOR_API_CALL_NOCONF(cursor, session, reset, NULL);
	APPLY_CG(ctable, reset);
	API_END(session);

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
	CURSOR_API_CALL_NOCONF(cursor, session, search, NULL);
	APPLY_CG(ctable, search);
	API_END(session);

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
	int i;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, search_near, NULL);
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
	int i;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, insert, NULL);
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
			F_SET(primary, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
			ret = __curtable_update(cursor);
		}
		goto err;
	}

	for (i = 1; i < WT_COLGROUPS(ctable->table); i++, cp++) {
		(*cp)->recno = primary->recno;
		WT_ERR((*cp)->insert(*cp));
	}

	APPLY_IDX(ctable, insert);
err:	API_END(session);

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
	CURSOR_API_CALL_NOCONF(cursor, session, update, NULL);
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
err:	API_END(session);

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
	CURSOR_API_CALL_NOCONF(cursor, session, remove, NULL);

	/* Find the old record so it can be removed from indices */
	WT_ERR(__curtable_open_indices(ctable));
	if (ctable->table->nindices > 0) {
		APPLY_CG(ctable, search);
		WT_ERR(ret);
		APPLY_IDX(ctable, remove);
	}

	APPLY_CG(ctable, remove);
err:	API_END(session);

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
	int i;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, close, NULL);

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
	__wt_free(session, ctable->cg_cursors);
	__wt_free(session, ctable->idx_cursors);
	/* The URI is owned by the table. */
	cursor->uri = NULL;
	WT_TRET(__wt_cursor_close(cursor));
	API_END(session);

	return (ret);
}

static int
__curtable_open_colgroups(WT_CURSOR_TABLE *ctable, const char *cfg[])
{
	WT_SESSION_IMPL *session;
	WT_TABLE *table;
	WT_CURSOR **cp;
	const char *cfg_no_overwrite[4];
	int i;

	session = (WT_SESSION_IMPL *)ctable->iface.session;
	table = ctable->table;

	/* Underlying column groups are always opened without overwrite */
	cfg_no_overwrite[0] = cfg[0];
	cfg_no_overwrite[1] = cfg[1];
	cfg_no_overwrite[2] = "overwrite=false";
	cfg_no_overwrite[3] = NULL;

	if (!table->cg_complete)
		WT_RET_MSG(session, EINVAL,
		    "Can't use '%s' until all column groups are created",
		    table->name);

	WT_RET(__wt_calloc_def(session,
	    WT_COLGROUPS(table), &ctable->cg_cursors));

	for (i = 0, cp = ctable->cg_cursors;
	    i < WT_COLGROUPS(table);
	    i++, cp++)
		WT_RET(__wt_curfile_open(session, table->cg_name[i],
		    &ctable->iface, cfg_no_overwrite, cp));
	return (0);
}

static int
__curtable_open_indices(WT_CURSOR_TABLE *ctable)
{
	WT_CURSOR **cp, *primary;
	WT_SESSION_IMPL *session;
	WT_TABLE *table;
	const char *cfg[] = API_CONF_DEFAULTS(session, open_cursor, NULL);
	int i;

	session = (WT_SESSION_IMPL *)ctable->iface.session;
	table = ctable->table;

	if (!ctable->table->idx_complete)
		WT_RET(__wt_schema_open_index(session, table, NULL, 0));
	if (table->nindices == 0 || ctable->idx_cursors != NULL)
		return (0);
	/* Check for bulk cursors. */
	primary = *ctable->cg_cursors;
	if (F_ISSET(((WT_CURSOR_BTREE *)primary)->btree, WT_BTREE_BULK))
		WT_RET_MSG(session, ENOTSUP,
		    "Bulk load is not supported for tables with indices");
	WT_RET(__wt_calloc_def(session, table->nindices, &ctable->idx_cursors));

	for (i = 0, cp = ctable->idx_cursors; i < table->nindices; i++, cp++)
		WT_RET(__wt_curfile_open(session, table->idx_name[i],
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
	static WT_CURSOR iface = {
		NULL,
		NULL,
		NULL,
		NULL,
		__wt_curtable_get_key,
		__wt_curtable_get_value,
		__wt_curtable_set_key,
		__wt_curtable_set_value,
		NULL,
		__curtable_next,
		__curtable_prev,
		__curtable_reset,
		__curtable_search,
		__curtable_search_near,
		__curtable_insert,
		__curtable_update,
		__curtable_remove,
		__curtable_close,
		NULL,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ 0 },                  /* raw recno buffer */
		{ NULL, 0, 0, NULL, 0 },/* WT_ITEM key */
		{ NULL, 0, 0, NULL, 0 },/* WT_ITEM value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
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
	if ((ret =
	    __wt_schema_get_table(session, tablename, size, &table)) != 0) {
		if (ret == WT_NOTFOUND)
			WT_RET_MSG(session, EINVAL,
			    "Cannot open cursor '%s' on unknown table", uri);
		return (ret);
	}

	if (!table->cg_complete)
		WT_RET_MSG(session, EINVAL,
		    "Cannot open cursor '%s' on incomplete table", uri);
	if (table->is_simple)
		/*
		 * The returned cursor should be public: it is not part of a
		 * table cursor.
		 */
		return (__wt_curfile_open(
		    session, table->cg_name[0], NULL, cfg, cursorp));

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
err:		(void)__curtable_close(cursor);
		*cursorp = NULL;
	}

	return (ret);
}
