/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "../btree/bt_inline.c"

static int __curtable_next(WT_CURSOR *cursor);

static int
__curtable_first(WT_CURSOR *cursor)
{
	BTREE *btree;
	ICURSOR_TABLE *ctable;
	SESSION *session;

	ctable = (ICURSOR_TABLE *)cursor;
	btree = ctable->btree;
	session = (SESSION *)cursor->session;

	WT_RET(__wt_walk_begin(session, &btree->root_page, &ctable->walk));
	F_SET((WT_CURSOR_STD *)cursor, WT_CURSTD_POSITIONED);

	return (__curtable_next(cursor));
}

static int
__curtable_last(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

static int
__curtable_next(WT_CURSOR *cursor)
{
	BTREE *btree;
	WT_DATAITEM *key;
	ICURSOR_TABLE *ctable;
	SESSION *session;
	WT_CURSOR_STD *cstd;
	WT_ITEM *item;
	WT_UPDATE *upd;
	void *huffman;
	int ret;

	ctable = (ICURSOR_TABLE *)cursor;
	cstd = &ctable->cstd;
	btree = ctable->btree;
	huffman = btree->huffman_data;
	session = (SESSION *)cursor->session;
	ret = 0;

	/*
	 * TODO: fix this, currently done by the internal API layer, lower
	 * levels should be passed the handles they need, rather than putting
	 * everything in SESSION / SESSION.
	 */
	session->btree = ctable->btree;

	if (ctable->walk.tree == NULL)
		return (__curtable_first(cursor));

	do {
		while (ctable->nitems == 0) {
			WT_RET(__wt_walk_next(session, &ctable->walk,
			    0, &ctable->ref));
			if (ctable->ref == NULL) {
				F_CLR(cstd, WT_CURSTD_POSITIONED);
				return (WT_NOTFOUND);
			}
			if (ctable->ref->page->dsk->type != WT_PAGE_ROW_LEAF)
				continue;

			ctable->nitems = ctable->ref->page->indx_count;
			ctable->rip = ctable->ref->page->u.row_leaf.d;
		}

		for (; ctable->nitems > 0; ++ctable->rip, ctable->nitems--) {
			/* Check for deletion. */
			upd = WT_ROW_UPDATE(ctable->ref->page, ctable->rip);
			if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
				continue;

			/*
			 * The key and value variables reference the DBTs we'll
			 * print.  Set the key.
			 */
			if (__wt_key_process(ctable->rip)) {
				WT_RET(__wt_item_process(session,
				    ctable->rip->key, ctable->key_tmp));
				key = &ctable->key_tmp->item;
			} else
				key = (WT_DATAITEM *)ctable->rip;

			cstd->key.item.data = key->data;
			cstd->key.item.size = key->size;

			/*
			 * If the item was ever upd, dump the data from the
			 * upd entry.
			 */
			if (upd != NULL) {
				cstd->value.item.data = WT_UPDATE_DATA(upd);
				cstd->value.item.size = upd->size;
				break;
			}

			/* Set data to reference the data we'll dump. */
			item = ctable->rip->value;
			if (WT_ITEM_TYPE(item) == WT_ITEM_DATA) {
				if (huffman == NULL) {
					cstd->value.item.data =
					    WT_ITEM_BYTE(item);
					cstd->value.item.size =
					    WT_ITEM_LEN(item);
					break;
				}
			} else if (WT_ITEM_TYPE(item) == WT_ITEM_DATA_OVFL) {
				WT_RET(__wt_item_process(session,
				    item, ctable->value_tmp));
				cstd->value.item = ctable->value_tmp->item;
				break;
			} else
				continue;
		}

		if (ctable->nitems == 0)
			continue;

		/* We have a key/value pair, return it and move on. */
		++ctable->rip;
		ctable->nitems--;
		return (0);
	} while (0);

	return (ret);
}

static int
__curtable_prev(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

static int
__curtable_search(WT_CURSOR *cursor)
{
	int exact;
	return (cursor->search_near(cursor, &exact) ||
	    (exact != 0 ? WT_NOTFOUND : 0));
}

static int
__curtable_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_UNUSED(cursor);
	WT_UNUSED(exact);

	return (ENOTSUP);
}

static int
__curtable_insert(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

static int
__curtable_update(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

static int
__curtable_del(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

static int
__curtable_close(WT_CURSOR *cursor, const char *config)
{
	ICURSOR_TABLE *ctable;
	SESSION *session;
	int ret;

	ctable = (ICURSOR_TABLE *)cursor;
	session = (SESSION *)cursor->session;
	ret = 0;

	__wt_walk_end(session, &ctable->walk);
	WT_TRET(__wt_curstd_close(cursor, config));

	return (ret);
}

static int
__get_btree(SESSION *session, const char *name, size_t namelen, BTREE **btreep)
{
	BTREE *btree;

	TAILQ_FOREACH(btree, &session->btrees, q) {
		if (strncmp(name, btree->name, namelen) == 0 &&
		    btree->name[namelen] == '\0') {
			*btreep = btree;
			return (0);
		}
	}

	return (WT_NOTFOUND);
}

int
__wt_curtable_open(SESSION *session,
    const char *uri, const char *config, WT_CURSOR **cursorp)
{
	static WT_CURSOR iface = {
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		__curtable_first,
		__curtable_last,
		__curtable_next,
		__curtable_prev,
		__curtable_search,
		__curtable_search_near,
		__curtable_insert,
		__curtable_update,
		__curtable_del,
		__curtable_close,
	};
	const char *tablename;
	BTREE *btree;
	CONNECTION *conn;
	ICURSOR_TABLE *ctable;
	WT_CONFIG_ITEM cvalue;
	WT_CURSOR *cursor;
	int bulk, dump, ret;
	size_t csize;

	conn = S2C(session);

	/* TODO: handle projections. */
	tablename = uri + 6;

	ret = __get_btree(session, tablename, strlen(tablename), &btree);
	if (ret == WT_NOTFOUND) {
		ret = 0;
		WT_RET(conn->btree(conn, 0, &btree));
		WT_RET(btree->open(btree, tablename, 0, 0));

		TAILQ_INSERT_HEAD(&session->btrees, btree, q);
	} else
		WT_RET(ret);

	bulk = dump = 0;
	CONFIG_LOOP(session, config, cvalue)
		CONFIG_ITEM("dump")
			dump = (cvalue.val != 0);
		CONFIG_ITEM("bulk")
			bulk = (cvalue.val != 0);
	CONFIG_END(session)

	csize = bulk ? sizeof(ICURSOR_BULK) : sizeof(ICURSOR_TABLE);
	WT_RET(__wt_calloc(session, 1, csize, &ctable));

	cursor = &ctable->cstd.iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->key_format = cursor->value_format = "u";
	ctable->btree = btree;
	__wt_curstd_init(&ctable->cstd);
	if (bulk)
		WT_ERR(__wt_curbulk_init((ICURSOR_BULK *)ctable));
	if (dump)
		__wt_curdump_init(&ctable->cstd);

	WT_ERR(__wt_scr_alloc(session, 0, &ctable->key_tmp));
	WT_ERR(__wt_scr_alloc(session, 0, &ctable->value_tmp));

	STATIC_ASSERT(offsetof(ICURSOR_TABLE, cstd) == 0);
	TAILQ_INSERT_HEAD(&session->cursors, &ctable->cstd, q);
	*cursorp = cursor;

	if (0) {
err:		__wt_free(session, ctable, csize);
	}

	return (ret);
}
