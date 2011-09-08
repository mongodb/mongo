/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __curindex_get_value --
 *	WT_CURSOR->get_value implementation for index cursors.
 */
static int
__curindex_get_value(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_INDEX *cindex;
	WT_ITEM *item;
	WT_SESSION_IMPL *session;
	va_list ap;
	int ret;

	cindex = (WT_CURSOR_INDEX *)cursor;
	CURSOR_API_CALL(cursor, session, get_value, NULL);
	WT_CURSOR_NEEDVALUE(cursor);

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		ret = __wt_schema_project_merge(session,
		    cindex->cg_cursors, cindex->value_plan,
		    cursor->value_format, &cursor->value);
		if (ret == 0) {
			item = va_arg(ap, WT_ITEM *);
			item->data = cursor->value.data;
			item->size = cursor->value.size;
		}
	} else
		ret = __wt_schema_project_out(session,
		    cindex->cg_cursors, cindex->value_plan, ap);
	va_end(ap);
err:	API_END(session);

	return (ret);
}

/*
 * __curindex_set_value --
 *	WT_CURSOR->set_value implementation for index cursors.
 */
static void
__curindex_set_value(WT_CURSOR *cursor, ...)
{
	WT_SESSION_IMPL *session;
	int ret;

	CURSOR_API_CALL(cursor, session, set_value, NULL);
	cursor->saved_err = ENOTSUP;
	F_CLR(cursor, WT_CURSTD_VALUE_SET);
	API_END(session);
}

/*
 * __curindex_move --
 *	When an index cursor changes position, set the primary key in the
 *	associated column groups and update their positions to match.
 */
static int
__curindex_move(WT_CURSOR_INDEX *cindex)
{
	WT_BUF *firstkey;
	WT_CURSOR **cp;
	WT_SESSION_IMPL *session;
	uint64_t recno;
	int i;

	session = (WT_SESSION_IMPL *)cindex->cbt.iface.session;
	firstkey = NULL;

	for (i = 0, cp = cindex->cg_cursors;
	    i < WT_COLGROUPS(cindex->table);
	    i++, cp++) {
		if (*cp == NULL)
			continue;
		/* Set the primary key. */
		if (firstkey == NULL) {
			WT_RET(__wt_schema_project_slice(session,
			    cp, cindex->key_plan, 1,
			    cindex->cbt.iface.key_format,
			    (WT_ITEM *)&cindex->cbt.iface.key));
			firstkey = &(*cp)->key;
			recno = (*cp)->recno;
		} else {
			(*cp)->key.data = firstkey->data;
			(*cp)->key.size = firstkey->size;
			(*cp)->recno = recno;
		}
		F_SET(*cp, WT_CURSTD_KEY_SET);
		WT_RET((*cp)->search(*cp));
	}

	return (0);
}

/*
 * __curindex_first --
 *	WT_CURSOR->first method for index cursors.
 */
static int
__curindex_first(WT_CURSOR *cursor)
{
	WT_CURSOR_INDEX *cindex;
	WT_SESSION_IMPL *session;
	int ret;

	cindex = (WT_CURSOR_INDEX *)cursor;
	CURSOR_API_CALL(cursor, session, first, cindex->cbt.btree);
	if ((ret = __wt_btcur_first(&cindex->cbt)) == 0)
		ret = __curindex_move(cindex);
	API_END(session);

	return (ret);
}

/*
 * __curindex_last --
 *	WT_CURSOR->last method for index cursors.
 */
static int
__curindex_last(WT_CURSOR *cursor)
{
	WT_CURSOR_INDEX *cindex;
	WT_SESSION_IMPL *session;
	int ret;

	cindex = (WT_CURSOR_INDEX *)cursor;
	CURSOR_API_CALL(cursor, session, last, cindex->cbt.btree);
	if ((ret = __wt_btcur_last(&cindex->cbt)) == 0)
		ret = __curindex_move(cindex);
	API_END(session);

	return (ret);
}

/*
 * __curindex_next --
 *	WT_CURSOR->next method for index cursors.
 */
static int
__curindex_next(WT_CURSOR *cursor)
{
	WT_CURSOR_INDEX *cindex;
	WT_SESSION_IMPL *session;
	int ret;

	cindex = (WT_CURSOR_INDEX *)cursor;
	CURSOR_API_CALL(cursor, session, next, cindex->cbt.btree);
	if ((ret = __wt_btcur_next(&cindex->cbt)) == 0)
		ret = __curindex_move(cindex);
	API_END(session);

	return (ret);
}

/*
 * __curindex_prev --
 *	WT_CURSOR->prev method for index cursors.
 */
static int
__curindex_prev(WT_CURSOR *cursor)
{
	WT_CURSOR_INDEX *cindex;
	WT_SESSION_IMPL *session;
	int ret;

	cindex = (WT_CURSOR_INDEX *)cursor;
	CURSOR_API_CALL(cursor, session, prev, cindex->cbt.btree);
	if ((ret = __wt_btcur_prev(&cindex->cbt)) == 0)
		ret = __curindex_move(cindex);
	API_END(session);

	return (ret);
}

/*
 * __curindex_search --
 *	WT_CURSOR->search method for index cursors.
 */
static int
__curindex_search(WT_CURSOR *cursor)
{
	WT_CURSOR_INDEX *cindex;
	WT_BUF *oldkeyp;
	WT_SESSION_IMPL *session;
	int exact, ret;

	cindex = (WT_CURSOR_INDEX *)cursor;
	CURSOR_API_CALL(cursor, session, search, cindex->cbt.btree);

	/*
	 * XXX a very odd corner case is an index with a recno key.
	 * The only way to get here is by creating an index on a column store
	 * using only the primary's recno as the index key.  Disallow that for
	 * now.
	 */
	WT_ASSERT(session, strcmp(cursor->key_format, "r") != 0);

	/*
	 * We expect partial matches, but we want the smallest item that
	 * matches the prefix.  Fail if there is no matching item.
	 */

	/*
	 * Take a copy of the search key.
	 * XXX we can avoid this with a cursor flag indicating when the
	 * application owns the data.
	 */
	WT_ERR(__wt_scr_alloc(session, cursor->key.size, &oldkeyp));
	memcpy(oldkeyp->mem, cursor->key.data, cursor->key.size);
	oldkeyp->size = cursor->key.size;

	WT_ERR(cursor->search_near(cursor, &exact));

	/* We expect partial matches. */
	if (exact < 1)
		WT_ERR(cursor->next(cursor));

	if (cursor->key.size < oldkeyp->size ||
	    memcmp(oldkeyp->data, cursor->key.data, oldkeyp->size) != 0) {
		ret = WT_NOTFOUND;
		goto err;
	}

	WT_ERR(__curindex_move(cindex));

err:	__wt_scr_free(&oldkeyp);
	API_END(session);

	return (ret);
}

/*
 * __curindex_search_near --
 *	WT_CURSOR->search_near method for index cursors.
 */
static int
__curindex_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_CURSOR_INDEX *cindex;
	WT_SESSION_IMPL *session;
	int ret;

	cindex = (WT_CURSOR_INDEX *)cursor;
	CURSOR_API_CALL(cursor, session, search_near, cindex->cbt.btree);
	if ((ret = __wt_btcur_search_near(&cindex->cbt, exact)) == 0)
		ret = __curindex_move(cindex);
	API_END(session);

	return (ret);
}

/*
 * __curindex_insert --
 *	WT_CURSOR->insert method for index cursors.
 */
static int
__curindex_insert(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curindex_update --
 *	WT_CURSOR->update method for index cursors.
 */
static int
__curindex_update(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curindex_remove --
 *	WT_CURSOR->remove method for index cursors.
 */
static int
__curindex_remove(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __curindex_close --
 *	WT_CURSOR->close method for index cursors.
 */
static int
__curindex_close(WT_CURSOR *cursor, const char *config)
{
	WT_BTREE *btree;
	WT_CURSOR_INDEX *cindex;
	WT_CURSOR **cp;
	WT_SESSION_IMPL *session;
	int i, ret;

	cindex = (WT_CURSOR_INDEX *)cursor;
	btree = cindex->cbt.btree;

	CURSOR_API_CALL_CONF(cursor, session, close, NULL, config, cfg);
	WT_UNUSED(cfg);

	for (i = 0, cp = (cindex)->cg_cursors;
	    i < WT_COLGROUPS(cindex->table); i++, cp++)
		if (*cp != NULL) {
			WT_TRET((*cp)->close(*cp, config));
			*cp = NULL;
		}

	__wt_free(session, cindex->cg_cursors);
	if (cindex->key_plan != btree->key_plan)
		__wt_free(session, cindex->key_plan);
	if (cindex->value_plan != btree->value_plan)
		__wt_free(session, cindex->value_plan);

	WT_TRET(__wt_btcur_close(&cindex->cbt, config));
	WT_TRET(__wt_cursor_close(cursor, config));
err:	API_END(session);

	return (ret);
}

static int
__curindex_open_colgroups(
    WT_SESSION_IMPL *session, WT_CURSOR_INDEX *cindex, const char *config)
{
	WT_TABLE *table;
	WT_CURSOR **cp;
	char *proj;
	uint32_t arg;

	table = cindex->table;
	WT_RET(__wt_calloc_def(session, WT_COLGROUPS(table), &cp));
	cindex->cg_cursors = cp;

	/* Work out which column groups we need. */
	for (proj = (char *)cindex->value_plan; *proj != '\0'; proj++) {
		arg = (uint32_t)strtoul(proj, &proj, 10);
		if ((*proj != WT_PROJ_KEY && *proj != WT_PROJ_VALUE) ||
		    cp[arg] != NULL)
			continue;
		session->btree = table->colgroup[arg];
		WT_RET(__wt_curfile_create(session, 0, config, &cp[arg]));
	}

	return (0);
}

/*
 * __wt_curindex_open --
 *	WT_SESSION->open_cursor method for index cursors.
 */
int
__wt_curindex_open(WT_SESSION_IMPL *session,
    const char *uri, const char *config, WT_CURSOR **cursorp)
{
	static WT_CURSOR iface = {
		NULL,
		NULL,
		NULL,
		NULL,
		__curindex_get_value,
		NULL,
		__curindex_set_value,
		__curindex_first,
		__curindex_last,
		__curindex_next,
		__curindex_prev,
		__curindex_search,
		__curindex_search_near,
		__curindex_insert,
		__curindex_update,
		__curindex_remove,
		__curindex_close,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF key */
		{ NULL, 0, 0, NULL, 0 },/* WT_BUF value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	WT_CURSOR_INDEX *cindex;
	WT_CURSOR_BTREE *cbt;
	WT_CURSOR *cursor;
	WT_TABLE *table;
	const char *columns, *idxname, *tablename;
	size_t namesize;
	int ret;

	ret = 0;

	tablename = uri;
	if (!WT_PREFIX_SKIP(tablename, "index:") ||
	    (idxname = strchr(tablename, ':')) == NULL)
		return (EINVAL);
	namesize = (size_t)(idxname - tablename);
	++idxname;

	if ((ret = __wt_schema_get_table(session,
	    tablename, namesize, &table)) != 0) {
		if (ret == WT_NOTFOUND) {
			__wt_errx(session,
			    "Cannot open cursor '%s' on unknown table", uri);
			ret = EINVAL;
		}
		return (ret);
	}

	columns = strchr(idxname, '(');
	if (columns == NULL)
		namesize = strlen(idxname);
	else
		namesize = (size_t)(columns - idxname);

	WT_RET(__wt_schema_open_index(session, table, idxname, namesize));
	WT_RET(__wt_calloc_def(session, 1, &cindex));

	cindex->table = table;
	cbt = &cindex->cbt;
	cbt->btree = session->btree;
	cindex->key_plan = session->btree->key_plan;
	cindex->value_plan = session->btree->value_plan;

	/* Open the column groups needed for this index cursor. */
	WT_RET(__curindex_open_colgroups(session, cindex, config));

	cursor = &cbt->iface;
	*cursor = iface;
	cursor->session = &session->iface;

	/*
	 * XXX: not exactly, the cursor may not expose all of the key columns,
	 * and should use the table's value format by default.
	 */
	cursor->key_format = cbt->btree->key_format;
	cursor->value_format = table->value_format;

	__wt_cursor_init(cursor, 1, config);
	*cursorp = cursor;

	return (0);
}
