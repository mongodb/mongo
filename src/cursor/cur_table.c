/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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

/* Cursor type for custom extractor callback. */
typedef struct {
	WT_CURSOR iface;
	WT_CURSOR_TABLE *ctable;
	WT_CURSOR *idxc;
	int (*f)(WT_CURSOR *);
} WT_CURSOR_EXTRACTOR;

/*
 * __curextract_insert --
 *	Handle a key produced by a custom extractor.
 */
static int
__curextract_insert(WT_CURSOR *cursor) {
	WT_CURSOR_EXTRACTOR *cextract;
	WT_ITEM *key, ikey, pkey;
	WT_SESSION_IMPL *session;

	cextract = (WT_CURSOR_EXTRACTOR *)cursor;
	session = (WT_SESSION_IMPL *)cursor->session;

	WT_ITEM_SET(ikey, cursor->key);
	/*
	 * We appended a padding byte to the key to avoid rewriting the last
	 * column.  Strip that away here.
	 */
	WT_ASSERT(session, ikey.size > 0);
	--ikey.size;
	WT_RET(__wt_cursor_get_raw_key(cextract->ctable->cg_cursors[0], &pkey));

	/*
	 * We have the index key in the format we need, and all of the primary
	 * key columns are required: just append them.
	 */
	key = &cextract->idxc->key;
	WT_RET(__wt_buf_grow(session, key, ikey.size + pkey.size));
	memcpy((uint8_t *)key->mem, ikey.data, ikey.size);
	memcpy((uint8_t *)key->mem + ikey.size, pkey.data, pkey.size);
	key->size = ikey.size + pkey.size;

	/*
	 * The index key is now set and the value is empty (it starts clear and
	 * is never set).
	 */
	F_SET(cextract->idxc, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);

	/* Call the underlying cursor function to update the index. */
	return (cextract->f(cextract->idxc));
}

/*
 * __apply_idx --
 *	Apply an operation to all indices of a table.
 */
static int
__apply_idx(WT_CURSOR_TABLE *ctable, size_t func_off) {
	WT_CURSOR_STATIC_INIT(iface,
	    __wt_cursor_get_key,		/* get-key */
	    __wt_cursor_get_value,		/* get-value */
	    __wt_cursor_set_key,		/* set-key */
	    __wt_cursor_set_value,		/* set-value */
	    __wt_cursor_notsup,			/* compare */
	    __wt_cursor_notsup,			/* next */
	    __wt_cursor_notsup,			/* prev */
	    __wt_cursor_notsup,			/* reset */
	    __wt_cursor_notsup,			/* search */
	    __wt_cursor_notsup,			/* search-near */
	    __curextract_insert,		/* insert */
	    __wt_cursor_notsup,			/* update */
	    __wt_cursor_notsup,			/* remove */
	    __wt_cursor_notsup);		/* close */
	WT_CURSOR **cp;
	WT_CURSOR_EXTRACTOR extract_cursor;
	WT_DECL_RET;
	WT_INDEX *idx;
	WT_ITEM key, value;
	WT_SESSION_IMPL *session;
	int (*f)(WT_CURSOR *);
	u_int i;

	cp = ctable->idx_cursors;
	session = (WT_SESSION_IMPL *)ctable->iface.session;

	for (i = 0; i < ctable->table->nindices; i++, cp++) {
		f = *(int (**)(WT_CURSOR *))((uint8_t *)*cp + func_off);
		idx = ctable->table->indices[i];
		if (idx->extractor) {
			extract_cursor.iface = iface;
			extract_cursor.iface.session = &session->iface;
			extract_cursor.iface.key_format = idx->exkey_format;
			extract_cursor.ctable = ctable;
			extract_cursor.idxc = *cp;
			extract_cursor.f = f;

			WT_RET(__wt_cursor_get_raw_key(&ctable->iface, &key));
			WT_RET(
			    __wt_cursor_get_raw_value(&ctable->iface, &value));
			ret = idx->extractor->extract(idx->extractor,
			    &session->iface, &key, &value,
			    &extract_cursor.iface);

			__wt_buf_free(session, &extract_cursor.iface.key);
			WT_RET(ret);
		} else {
			WT_RET(__wt_schema_project_merge(session,
			    ctable->cg_cursors,
			    idx->key_plan, idx->key_format, &(*cp)->key));
			/*
			 * The index key is now set and the value is empty
			 * (it starts clear and is never set).
			 */
			F_SET(*cp, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);
			WT_RET(f(*cp));
		}
		WT_RET((*cp)->reset(*cp));
	}

	return (0);
}

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

err:	API_END_RET(session, ret);
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
		F_SET(*cp, WT_CURSTD_KEY_EXT);
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
	if (F_ISSET(cursor, WT_CURSOR_RAW_OK | WT_CURSTD_DUMP_JSON)) {
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
			F_SET(*cp, WT_CURSTD_VALUE_EXT);
		else {
			(*cp)->saved_err = ret;
			F_CLR(*cp, WT_CURSTD_VALUE_SET);
		}

err:	API_END(session, ret);
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
	if (strcmp(a->internal_uri, b->internal_uri) != 0)
		WT_ERR_MSG(session, EINVAL,
		    "comparison method cursors must reference the same object");
	WT_CURSOR_CHECKKEY(WT_CURSOR_PRIMARY(a));
	WT_CURSOR_CHECKKEY(WT_CURSOR_PRIMARY(b));

	ret = WT_CURSOR_PRIMARY(a)->compare(
	    WT_CURSOR_PRIMARY(a), WT_CURSOR_PRIMARY(b), cmpp);

err:	API_END_RET(session, ret);
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

err:	API_END_RET(session, ret);
}

/*
 * __curtable_next_random --
 *	WT_CURSOR->next method for the table cursor type when configured with
 *	next_random.
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
		F_SET(*cp, WT_CURSTD_KEY_EXT);
		WT_ERR((*cp)->search(*cp));
	}

err:	API_END_RET(session, ret);
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

err:	API_END_RET(session, ret);
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

err:	API_END_RET(session, ret);
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

err:	API_END_RET(session, ret);
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
		F_SET(*cp, WT_CURSTD_KEY_EXT);
		WT_ERR((*cp)->search(*cp));
	}

err:	API_END_RET(session, ret);
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
	uint32_t flag_orig;
	u_int i;

	ctable = (WT_CURSOR_TABLE *)cursor;
	CURSOR_UPDATE_API_CALL(cursor, session, insert, NULL);
	WT_ERR(__curtable_open_indices(ctable));

	/*
	 * Split out the first insert, it may be allocating a recno.
	 *
	 * If the table has indices, we also need to know whether this record
	 * is replacing an existing record so that the existing index entries
	 * can be removed.  We discover if this is an overwrite by configuring
	 * the primary cursor for no-overwrite, and checking if the insert
	 * detects a duplicate key.
	 */
	cp = ctable->cg_cursors;
	primary = *cp++;

	flag_orig = F_ISSET(primary, WT_CURSTD_OVERWRITE);
	if (ctable->table->nindices > 0)
		F_CLR(primary, WT_CURSTD_OVERWRITE);
	ret = primary->insert(primary);
	F_SET(primary, flag_orig);

	if (ret == WT_DUPLICATE_KEY && F_ISSET(cursor, WT_CURSTD_OVERWRITE)) {
		/*
		 * !!!
		 * The insert failure clears these flags, but does not touch the
		 * items.  We could make a copy each time for overwrite cursors,
		 * but for now we just reset the flags.
		 */
		F_SET(primary, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);
		ret = __curtable_update(cursor);
		goto err;
	}
	WT_ERR(ret);

	for (i = 1; i < WT_COLGROUPS(ctable->table); i++, cp++) {
		(*cp)->recno = primary->recno;
		WT_ERR((*cp)->insert(*cp));
	}

	WT_ERR(__apply_idx(ctable, offsetof(WT_CURSOR, insert)));

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
	WT_DECL_ITEM(value_copy);
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
	if (ctable->table->nindices > 0) {
		WT_ERR(__wt_scr_alloc(
		    session, ctable->cg_cursors[0]->value.size, &value_copy));
		WT_ERR(__wt_schema_project_merge(session,
		    ctable->cg_cursors, ctable->plan,
		    cursor->value_format, value_copy));
		APPLY_CG(ctable, search);

		/* Remove only if the key exists. */
		if (ret == 0) {
			WT_ERR(
			    __apply_idx(ctable, offsetof(WT_CURSOR, remove)));
			WT_ERR(__wt_schema_project_slice(session,
			    ctable->cg_cursors, ctable->plan, 0,
			    cursor->value_format, value_copy));
		} else
			WT_ERR_NOTFOUND_OK(ret);
	}

	APPLY_CG(ctable, update);
	WT_ERR(ret);

	if (ctable->table->nindices > 0)
		WT_ERR(__apply_idx(ctable, offsetof(WT_CURSOR, insert)));

err:	CURSOR_UPDATE_API_END(session, ret);
	__wt_scr_free(&value_copy);
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
	WT_ERR(__curtable_open_indices(ctable));

	/* Find the old record so it can be removed from indices */
	if (ctable->table->nindices > 0) {
		APPLY_CG(ctable, search);
		WT_ERR(ret);
		WT_ERR(__apply_idx(ctable, offsetof(WT_CURSOR, remove)));
	}

	APPLY_CG(ctable, remove);

err:	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __wt_table_range_truncate --
 *	Truncate of a cursor range, table implementation.
 */
int
__wt_table_range_truncate(WT_CURSOR_TABLE *start, WT_CURSOR_TABLE *stop)
{
	WT_CURSOR *wt_start, *wt_stop;
	WT_CURSOR_TABLE *ctable;
	WT_DECL_ITEM(key);
	WT_DECL_RET;
	WT_ITEM raw;
	WT_SESSION_IMPL *session;
	u_int i;
	int cmp;

	ctable = (start != NULL) ? start : stop;
	session = (WT_SESSION_IMPL *)ctable->iface.session;
	wt_start = &start->iface;
	wt_stop = &stop->iface;

	/* Open any indices. */
	WT_RET(__curtable_open_indices(ctable));
	WT_RET(__wt_scr_alloc(session, 128, &key));

	/*
	 * Step through the cursor range, removing the index entries.
	 *
	 * If there are indices, copy the key we're using to step through the
	 * cursor range (so we can reset the cursor to its original position),
	 * then remove all of the index records in the truncated range.  Copy
	 * the raw key because the memory is only valid until the cursor moves.
	 */
	if (ctable->table->nindices > 0) {
		if (start == NULL) {
			WT_ERR(__wt_cursor_get_raw_key(wt_stop, &raw));
			WT_ERR(__wt_buf_set(session, key, raw.data, raw.size));

			do {
				APPLY_CG(stop, search);
				WT_ERR(ret);
				WT_ERR(__apply_idx(
				    stop, offsetof(WT_CURSOR, remove)));
			} while ((ret = wt_stop->prev(wt_stop)) == 0);
			WT_ERR_NOTFOUND_OK(ret);

			__wt_cursor_set_raw_key(wt_stop, key);
			APPLY_CG(stop, search);
		} else {
			WT_ERR(__wt_cursor_get_raw_key(wt_start, &raw));
			WT_ERR(__wt_buf_set(session, key, raw.data, raw.size));

			cmp = -1;
			do {
				APPLY_CG(start, search);
				WT_ERR(ret);
				WT_ERR(__apply_idx(
				    start, offsetof(WT_CURSOR, remove)));
				if (stop != NULL)
					WT_ERR(wt_start->compare(
					    wt_start, wt_stop,
					    &cmp));
			} while (cmp < 0 &&
			    (ret = wt_start->next(wt_start)) == 0);
			WT_ERR_NOTFOUND_OK(ret);

			__wt_cursor_set_raw_key(wt_start, key);
			APPLY_CG(start, search);
		}
	}

	/* Truncate the column groups. */
	for (i = 0; i < WT_COLGROUPS(ctable->table); i++)
		WT_ERR(__wt_range_truncate(
		    (start == NULL) ? NULL : start->cg_cursors[i],
		    (stop == NULL) ? NULL : stop->cg_cursors[i]));

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

	for (i = 0, cp = ctable->cg_cursors;
	    i < WT_COLGROUPS(ctable->table); i++, cp++)
		if (*cp != NULL) {
			WT_TRET((*cp)->close(*cp));
			*cp = NULL;
		}

	if (ctable->idx_cursors != NULL)
		for (i = 0, cp = ctable->idx_cursors;
		    i < ctable->table->nindices; i++, cp++)
			if (*cp != NULL) {
				WT_TRET((*cp)->close(*cp));
				*cp = NULL;
			}

	if (ctable->plan != ctable->table->plan)
		__wt_free(session, ctable->plan);
	for (i = 0; ctable->cfg[i] != NULL; ++i)
		__wt_free(session, ctable->cfg[i]);
	__wt_free(session, ctable->cfg);
	if (cursor->value_format != ctable->table->value_format)
		__wt_free(session, cursor->value_format);
	__wt_free(session, ctable->cg_cursors);
	__wt_free(session, ctable->idx_cursors);
	__wt_schema_release_table(session, ctable->table);
	/* The URI is owned by the table. */
	cursor->internal_uri = NULL;
	WT_TRET(__wt_cursor_close(cursor));

err:	API_END_RET(session, ret);
}

/*
 * __curtable_open_colgroups --
 *	Open cursors on column groups for a table cursor.
 */
static int
__curtable_open_colgroups(WT_CURSOR_TABLE *ctable, const char *cfg_arg[])
{
	WT_SESSION_IMPL *session;
	WT_TABLE *table;
	WT_CURSOR **cp;
	/*
	 * Underlying column groups are always opened without dump, and only
	 * the primary is opened with next_random.
	 */
	const char *cfg[] = {
		cfg_arg[0], cfg_arg[1], "dump=\"\"", NULL, NULL
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

/*
 * __curtable_open_indices --
 *	Open cursors on indices for a table cursor.
 */
static int
__curtable_open_indices(WT_CURSOR_TABLE *ctable)
{
	WT_CURSOR **cp, *primary;
	WT_SESSION_IMPL *session;
	WT_TABLE *table;
	u_int i;

	session = (WT_SESSION_IMPL *)ctable->iface.session;
	table = ctable->table;

	WT_RET(__wt_schema_open_indices(session, table));
	if (table->nindices == 0 || ctable->idx_cursors != NULL)
		return (0);

	/* Check for bulk cursors. */
	primary = *ctable->cg_cursors;
	if (F_ISSET(primary, WT_CURSTD_BULK))
		WT_RET_MSG(session, ENOTSUP,
		    "Bulk load is not supported for tables with indices");

	WT_RET(__wt_calloc_def(session, table->nindices, &ctable->idx_cursors));
	for (i = 0, cp = ctable->idx_cursors; i < table->nindices; i++, cp++)
		WT_RET(__wt_open_cursor(session, table->indices[i]->source,
		    &ctable->iface, ctable->cfg, cp));
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
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_TABLE *table;
	size_t size;
	int cfg_cnt;
	const char *tablename, *columns;

	WT_STATIC_ASSERT(offsetof(WT_CURSOR_TABLE, iface) == 0);

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

	if (table->is_simple) {
		/* Just return a cursor on the underlying data source. */
		ret = __wt_open_cursor(session,
		    table->cgroups[0]->source, NULL, cfg, cursorp);

		__wt_schema_release_table(session, table);
		return (ret);
	}

	WT_RET(__wt_calloc_def(session, 1, &ctable));

	cursor = &ctable->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->internal_uri = table->name;
	cursor->key_format = table->key_format;
	cursor->value_format = table->value_format;

	ctable->table = table;
	ctable->plan = table->plan;

	/* Handle projections. */
	if (columns != NULL) {
		WT_ERR(__wt_scr_alloc(session, 0, &tmp));
		WT_ERR(__wt_struct_reformat(session, table,
		    columns, strlen(columns), NULL, 1, tmp));
		WT_ERR(__wt_strndup(
		    session, tmp->data, tmp->size, &cursor->value_format));

		WT_ERR(__wt_buf_init(session, tmp, 0));
		WT_ERR(__wt_struct_plan(session, table,
		    columns, strlen(columns), 0, tmp));
		WT_ERR(__wt_strndup(
		    session, tmp->data, tmp->size, &ctable->plan));
	}

	/*
	 * random_retrieval
	 * Random retrieval cursors only support next, reset and close.
	 */
	WT_ERR(__wt_config_gets_def(session, cfg, "next_random", 0, &cval));
	if (cval.val != 0) {
		__wt_cursor_set_notsup(cursor);
		cursor->next = __curtable_next_random;
		cursor->reset = __curtable_reset;
	}

	WT_ERR(__wt_cursor_init(
	    cursor, cursor->internal_uri, NULL, cfg, cursorp));

	if (F_ISSET(cursor, WT_CURSTD_DUMP_JSON))
		WT_ERR(__wt_json_column_init(cursor, table->key_format,
		    NULL, &table->colconf));

	/*
	 * Open the colgroup cursors immediately: we're going to need them for
	 * any operation.  We defer opening index cursors until we need them
	 * for an update.  Note that this must come after the call to
	 * __wt_cursor_init: the table cursor must already be on the list of
	 * session cursors or we can't work out where to put the colgroup
	 * cursor(s).
	 */
	WT_ERR(__curtable_open_colgroups(ctable, cfg));

	/*
	 * We'll need to squirrel away a copy of the cursor configuration
	 * for if/when we open indices.
	 *
	 * cfg[0] is the baseline configuration for the cursor open and we can
	 * acquire another copy from the configuration structures, so it would
	 * be reasonable not to copy it here: but I'd rather be safe than sorry.
	 *
	 * Underlying indices are always opened without dump.
	 */
	for (cfg_cnt = 0; cfg[cfg_cnt] != NULL; ++cfg_cnt)
		;
	WT_ERR(__wt_calloc_def(session, cfg_cnt + 2, &ctable->cfg));
	for (cfg_cnt = 0; cfg[cfg_cnt] != NULL; ++cfg_cnt)
		WT_ERR(
		    __wt_strdup(session, cfg[cfg_cnt], &ctable->cfg[cfg_cnt]));
	WT_ERR(__wt_strdup(session, "dump=\"\"", &ctable->cfg[cfg_cnt]));

	if (0) {
err:		WT_TRET(__curtable_close(cursor));
		*cursorp = NULL;
	}

	__wt_scr_free(&tmp);
	return (ret);
}
