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
	ISESSION *isession;

	ctable = (ICURSOR_TABLE *)cursor;
	btree = ctable->db->btree;
	isession = (ISESSION *)cursor->session;

	WT_RET(__wt_walk_begin(isession->toc, &btree->root_page, &ctable->walk));
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
	DBT *key;
	ICURSOR_TABLE *ctable;
	ISESSION *isession;
	WT_CURSOR_STD *cstd;
	WT_ITEM *item;
	WT_UPDATE *upd;
	void *huffman;
	int ret;

	ctable = (ICURSOR_TABLE *)cursor;
	cstd = &ctable->cstd;
	btree = ctable->db->btree;
	huffman = btree->huffman_data;
	isession = (ISESSION *)cursor->session;
	ret = 0;

	/*
	 * TODO: fix this, currently done by the internal API layer, lower
	 * levels should be passed the handles they need, rather than putting
	 * everything in WT_TOC / ISESSION.
	 */
	isession->toc->db = ctable->db;

	if (ctable->walk.tree == NULL)
		return (__curtable_first(cursor));

	do {
		while (ctable->nitems == 0) {
			WT_RET(__wt_walk_next(isession->toc, &ctable->walk,
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
				WT_RET(__wt_item_process(isession->toc,
				    ctable->rip->key, ctable->key_tmp));
				key = ctable->key_tmp;
			} else
				key = (DBT *)ctable->rip;

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
			item = ctable->rip->data;
			if (WT_ITEM_TYPE(item) == WT_ITEM_DATA) {
				if (huffman == NULL) {
					cstd->value.item.data =
					    WT_ITEM_BYTE(item);
					cstd->value.item.size =
					    WT_ITEM_LEN(item);
					break;
				}
			} else if (WT_ITEM_TYPE(item) == WT_ITEM_DATA_OVFL) {
				WT_RET(__wt_item_process(isession->toc,
				    item, ctable->value_tmp));
				cstd->value.item.data = ctable->value_tmp->data;
				cstd->value.item.size = ctable->value_tmp->size;
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
	ENV *env;
	ICURSOR_TABLE *ctable;
	int ret;

	ctable = (ICURSOR_TABLE *)cursor;
	env = ((ICONNECTION *)cursor->session->connection)->env;
	ret = 0;

	__wt_walk_end(env, &ctable->walk);
	WT_TRET(__wt_curstd_close(cursor, config));

	return (ret);
}

static int
__get_btree(ISESSION *isession, const char *name, size_t namelen, DB **dbp)
{
	DB *db;

	TAILQ_FOREACH(db, &isession->btrees, q) {
		if (strncmp(name, db->btree->name, namelen) == 0 &&
		    db->btree->name[namelen] == '\0') {
			*dbp = db;
			return (0);
		}
	}

	return (WT_NOTFOUND);
}

int
__wt_curtable_open(WT_SESSION *session,
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
	DB *db;
	ENV *env;
	ISESSION *isession;
	ICURSOR_TABLE *ctable;
	WT_CONFIG_ITEM cvalue;
	WT_CURSOR *cursor;
	int bulk, dump, ret;
	size_t csize;

	isession = (ISESSION *)session;
	env = ((ICONNECTION *)session->connection)->env;

	/* TODO: handle projections. */
	tablename = uri + 6;

	ret = __get_btree(isession, tablename, strlen(tablename), &db);
	if (ret == WT_NOTFOUND) {
		ret = 0;
		WT_RET(env->db(env, 0, &db));
		WT_RET(db->open(db, tablename, 0, 0));

		TAILQ_INSERT_HEAD(&isession->btrees, db, q);
	} else
		WT_RET(ret);

	bulk = dump = 0;
	CONFIG_LOOP(isession, config, cvalue)
		CONFIG_ITEM("dump")
			dump = (cvalue.val != 0);
		CONFIG_ITEM("bulk")
			bulk = (cvalue.val != 0);
	CONFIG_END(isession)

	csize = bulk ? sizeof(ICURSOR_BULK) : sizeof(ICURSOR_TABLE);
	WT_RET(__wt_calloc(env, 1, csize, &ctable));

	cursor = &ctable->cstd.iface;
	*cursor = iface;
	cursor->session = session;
	cursor->key_format = cursor->value_format = "u";
	ctable->db = db;
	__wt_curstd_init(&ctable->cstd);
	if (bulk)
		WT_ERR(__wt_curbulk_init((ICURSOR_BULK *)ctable));
	if (dump)
		__wt_curdump_init(&ctable->cstd);

	WT_ERR(__wt_scr_alloc(isession->toc, 0, &ctable->key_tmp));
	WT_ERR(__wt_scr_alloc(isession->toc, 0, &ctable->value_tmp));

	STATIC_ASSERT(offsetof(ICURSOR_TABLE, cstd) == 0);
	TAILQ_INSERT_HEAD(&isession->cursors, &ctable->cstd, q);
	*cursorp = cursor;

	if (0) {
err:		__wt_free(env, ctable, csize);
	}

	return (ret);
}
