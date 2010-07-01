/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_bt_page_discard_repl(ENV *, WT_SDBT *);
static int  __wt_bt_rec_col_fix(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int  __wt_bt_rec_col_int(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int  __wt_bt_rec_col_var(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int  __wt_bt_rec_row(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int  __wt_bt_rec_row_int(WT_TOC *, WT_PAGE *, WT_PAGE *);

/*
 * __wt_bt_rec_page --
 *	Move a page from its in-memory state out to disk.
 */
int
__wt_bt_rec_page(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	ENV *env;
	WT_PAGE *new;
	WT_PAGE_HDR *hdr;
	u_int32_t max;
	int ret;

	db = toc->db;
	env = toc->env;
	hdr = page->hdr;

	/* If the page isn't dirty, we're done. */
	WT_ASSERT(toc->env, WT_PAGE_MODIFY_ISSET(page));

	/*
	 * Multiple pages are marked for "draining" by the cache drain server,
	 * which means nobody can read them -- but, this thread of control has
	 * to update higher pages in the tree when it writes this page, which
	 * requires reading other pages, which might themselves be marked for
	 * draining.   Set a flag to allow this thread of control to see pages
	 * marked for draining -- we know it's safe, because only this thread
	 * is writing pages.
	 *
	 * Reconciliation is probably running because the cache is full, which
	 * means reads are locked out -- reconciliation can read, regardless.
	 */
	F_SET(toc, WT_READ_DRAIN | WT_READ_PRIORITY);

	/*
	 * Pages allocated by bulk load, but never subsequently modified don't
	 * need to be reconciled, they can simply be written to their allocated
	 * file blocks.   Those pages are "modified", but don't have in-memory
	 * versions.
	 */
	if (page->indx_count == 0) {
		ret = __wt_page_write(db, page);
		goto done;
	}

	switch (hdr->type) {
	case WT_PAGE_DESCRIPT:
	case WT_PAGE_OVFL:
		ret = __wt_page_write(db, page);
		goto done;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		/* We'll potentially need a new leaf page. */
		max = db->leafmax;
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		/* We'll potentially need a new internal page. */
		max = db->intlmax;
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

	/* Make sure the TOC's scratch buffer is big enough. */
	if (toc->tmp1.mem_size < sizeof(WT_PAGE) + max)
		WT_ERR(__wt_realloc(env, &toc->tmp1.mem_size,
		    sizeof(WT_PAGE) + max, &toc->tmp1.data));
	memset(toc->tmp1.data, 0, sizeof(WT_PAGE) + max);

	/* Initialize the reconciliation buffer as a replacement page. */
	new = toc->tmp1.data;
	new->hdr =
	    (WT_PAGE_HDR *)((u_int8_t *)toc->tmp1.data + sizeof(WT_PAGE));
	new->size = max;
	new->addr = page->addr;
	__wt_bt_set_ff_and_sa_from_offset(new, WT_PAGE_BYTE(new));
	new->hdr->type = page->hdr->type;
	new->hdr->level = page->hdr->level;

	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
		WT_ERR(__wt_bt_rec_col_fix(toc, page, new));
		break;
	case WT_PAGE_COL_VAR:
		WT_ERR(__wt_bt_rec_col_var(toc, page, new));
		break;
	case WT_PAGE_COL_INT:
		WT_ERR(__wt_bt_rec_col_int(toc, page, new));
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_ERR(__wt_bt_rec_row_int(toc, page, new));
		break;
	case WT_PAGE_ROW_LEAF:
	case WT_PAGE_DUP_LEAF:
		WT_ERR(__wt_bt_rec_row(toc, page, new));
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

err:
done:	/*
	 * Clear the modification flag.  This doesn't sound safe, because the
	 * cache thread might decide to discard this page as "clean".  That's
	 * not correct, because we're either the Db.sync method and holding a
	 * hazard reference and we finish our write before releasing our hazard
	 * reference, or we're the cache drain server and no other thread can
	 * discard the page.
	 */
	WT_PAGE_MODIFY_CLR_AND_FLUSH(page);

	F_CLR(toc, WT_READ_DRAIN | WT_READ_PRIORITY);
	return (ret);
}

/*
 * __wt_bt_rec_col_int --
 *	Reconcile a column store internal page.
 */
static int
__wt_bt_rec_col_int(WT_TOC *toc, WT_PAGE *page, WT_PAGE *rp)
{
	rp = NULL;
	return (__wt_page_write(toc->db, page));
}

/*
 * __wt_bt_rec_row_int --
 *	Reconcile a row store internal page.
 */
static int
__wt_bt_rec_row_int(WT_TOC *toc, WT_PAGE *page, WT_PAGE *rp)
{
	rp = NULL;
	return (__wt_page_write(toc->db, page));
}

/*
 * __wt_bt_rec_col_fix --
 *	Reconcile a fixed-width column-store leaf page.
 */
static int
__wt_bt_rec_col_fix(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_COL_INDX *cip;
	WT_PAGE_HDR *hdr;
	WT_SDBT *sdbt;
	u_int32_t i, len;
	u_int16_t repeat_count;
	u_int8_t *data, *last_data;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	hdr = new->hdr;
	last_data = NULL;

	/*
	 * We need a "deleted" data item to store on the page.  Make sure the
	 * WT_TOC's scratch buffer is big enough (our caller is using tmp1 so
	 * we use tmp2).   Clear the buffer's contents, and set the delete flag.
	 */
	len = db->fixed_len + sizeof(u_int16_t);
	if (toc->tmp2.mem_size < len)
		WT_RET(__wt_realloc(
		    env, &toc->tmp2.mem_size, len, &toc->tmp2.data));
	memset(toc->tmp2.data, 0, len);
	if (F_ISSET(idb, WT_REPEAT_COMP)) {
		WT_FIX_REPEAT_COUNT(toc->tmp2.data) = 1;
		WT_FIX_DELETE_SET(WT_FIX_REPEAT_DATA(toc->tmp2.data));
	} else {
		len = db->fixed_len;		/* No compression, fix len */
		WT_FIX_DELETE_SET(toc->tmp2.data);
	}

	WT_INDX_FOREACH(page, cip, i) {
		/*
		 * Get a reference to the data, on- or off- page, and see if
		 * it's been deleted.
		 */
		repeat_count = 1;
		WT_REPL_CURRENT_SET(cip, sdbt);
		if (sdbt != NULL) {
			if (WT_SDBT_DELETED_ISSET(sdbt->data))
				data = toc->tmp2.data;
			else
				data = sdbt->data;
		} else if (cip->data == NULL) {
			data = toc->tmp2.data;
		} else {
			data = cip->data;
			repeat_count = WT_FIX_REPEAT_COUNT(cip->data);
		}
		new->records += repeat_count;

		/*
		 * If the database supports repeat compression, check to see
		 * if we can fold this item into the previous one.
		 */
		if (last_data != NULL &&
		    memcmp(WT_FIX_REPEAT_DATA(last_data),
		    WT_FIX_REPEAT_DATA(data), db->fixed_len) == 0 &&
		    WT_FIX_REPEAT_COUNT(last_data) < UINT16_MAX) {
			WT_FIX_REPEAT_COUNT(last_data) += repeat_count;
			continue;
		}

		if (len > new->space_avail) {
			fprintf(stderr, "PAGE GREW, SPLIT\n");
			__wt_abort(toc->env);
		}

		if (F_ISSET(idb, WT_REPEAT_COMP))
			last_data = new->first_free;
		memcpy(new->first_free, data, len);
		new->first_free += len;
		new->space_avail -= len;

		++hdr->u.entries;
	}

	WT_ASSERT(toc->env, __wt_bt_verify_page(toc, new, NULL) == 0);

	return (__wt_page_write(db, new));
}

/*
 * __wt_bt_rec_col_var --
 *	Reconcile a variable-width column-store leaf page.
 */
static int
__wt_bt_rec_col_var(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	enum { DATA_ON_PAGE, DATA_OFF_PAGE } data_loc;
	DB *db;
	DBT *data, data_dbt;
	WT_COL_INDX *cip;
	WT_ITEM data_item;
	WT_OVFL data_ovfl;
	WT_PAGE_HDR *hdr;
	WT_SDBT *sdbt;
	u_int32_t i, len;

	db = toc->db;
	hdr = new->hdr;

	WT_CLEAR(data_dbt);
	WT_CLEAR(data_item);

	data = &data_dbt;

	WT_INDX_FOREACH(page, cip, i) {
		/*
		 * Get a reference to the data, on- or off- page, and see if
		 * it's been deleted.
		 */
		WT_REPL_CURRENT_SET(cip, sdbt);
		if (sdbt != NULL) {
			if (WT_SDBT_DELETED_ISSET(sdbt->data))
				goto deleted;

			/*
			 * Build the data's WT_ITEM chunk from the most recent
			 * replacement value.
			 */
			data->data = sdbt->data;
			data->size = sdbt->size;
			WT_RET(__wt_bt_build_data_item(
			    toc, data, &data_item, &data_ovfl));
			data_loc = DATA_OFF_PAGE;
		} else if (cip->data == NULL) {
deleted:		data->data = NULL;
			data->size = 0;
			WT_RET(__wt_bt_build_data_item(
			    toc, data, &data_item, &data_ovfl));
			WT_ITEM_TYPE_SET(&data_item, WT_ITEM_DEL);
			data_loc = DATA_OFF_PAGE;
		} else {
			data->data = cip->data;
			data->size = WT_ITEM_SPACE_REQ(WT_ITEM_LEN(cip->data));
			data_loc = DATA_ON_PAGE;
		}

		switch (data_loc) {
		case DATA_OFF_PAGE:
			len = WT_ITEM_SPACE_REQ(data->size);
			break;
		case DATA_ON_PAGE:
			len = data->size;
			break;
		}
		if (len > new->space_avail) {
			fprintf(stderr, "PAGE GREW, SPLIT\n");
			__wt_abort(toc->env);
		}
		switch (data_loc) {
		case DATA_ON_PAGE:
			memcpy(new->first_free, data->data, data->size);
			new->first_free += data->size;
			new->space_avail -= data->size;
			break;
		case DATA_OFF_PAGE:
			memcpy(new->first_free, &data_item, sizeof(data_item));
			memcpy(new->first_free +
			    sizeof(data_item), data->data, data->size);
			new->first_free += len;
			new->space_avail -= len;
		}
		++new->records;
		++hdr->u.entries;
	}

	WT_ASSERT(toc->env, __wt_bt_verify_page(toc, new, NULL) == 0);

	return (__wt_page_write(db, new));
}

/*
 * __wt_bt_rec_row --
 *	Reconcile a row-store leaf page.
 */
static int
__wt_bt_rec_row(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	enum { DATA_ON_PAGE, DATA_OFF_PAGE } data_loc;
	enum { KEY_ON_PAGE, KEY_OFF_PAGE, KEY_NONE } key_loc;
	DB *db;
	DBT *key, key_dbt, *data, data_dbt;
	WT_ITEM key_item, data_item;
	WT_OVFL key_ovfl, data_ovfl;
	WT_PAGE_HDR *hdr;
	WT_ROW_INDX *rip;
	WT_SDBT *sdbt;
	u_int32_t i, len, type;

	db = toc->db;
	hdr = new->hdr;

	WT_CLEAR(data_dbt);
	WT_CLEAR(key_dbt);
	WT_CLEAR(data_item);
	WT_CLEAR(key_item);

	key = &key_dbt;
	data = &data_dbt;

	/*
	 * Walk the page, accumulating key/data groups (groups, because a key
	 * can reference a duplicate data set).
	 */
	WT_INDX_FOREACH(page, rip, i) {
		/*
		 * Get a reference to the data.  We get the data first because
		 * it may have been deleted, in which case we ignore the pair.
		 */
		WT_REPL_CURRENT_SET(rip, sdbt);
		if (sdbt != NULL) {
			if (WT_SDBT_DELETED_ISSET(sdbt->data))
				continue;

			/*
			 * Build the data's WT_ITEM chunk from the most recent
			 * replacement value.
			 */
			data->data = sdbt->data;
			data->size = sdbt->size;
			WT_RET(__wt_bt_build_data_item(
			    toc, data, &data_item, &data_ovfl));
			data_loc = DATA_OFF_PAGE;
		} else {
			/* Copy the item off the page. */
			data->data = rip->data;
			data->size = WT_ITEM_SPACE_REQ(WT_ITEM_LEN(rip->data));
			data_loc = DATA_ON_PAGE;
		}

		/*
		 * Check if the key is a duplicate (the key preceding it on the
		 * page references the same information).  We don't store the
		 * key for the second and subsequent data items in duplicated
		 * groups.
		 */
		if (i > 0 && rip->key == (rip - 1)->key) {
			type = data_loc == DATA_ON_PAGE ?
			    WT_ITEM_TYPE(rip->data) : WT_ITEM_TYPE(&data_item);
			switch (type) {
				case WT_ITEM_DATA:
				case WT_ITEM_DUP:
					type = WT_ITEM_DUP;
					break;
				case WT_ITEM_DATA_OVFL:
				case WT_ITEM_DUP_OVFL:
					type = WT_ITEM_DUP_OVFL;
					break;
				WT_ILLEGAL_FORMAT(db);
				}
			if (data_loc == DATA_ON_PAGE)
				WT_ITEM_TYPE_SET(rip->data, type);
			else
				WT_ITEM_TYPE_SET(&data_item, type);
			key_loc = KEY_NONE;
		} else if (WT_KEY_PROCESS(rip)) {
			key->data = rip->key;
			key->size = WT_ITEM_SPACE_REQ(WT_ITEM_LEN(rip->key));
			key_loc = KEY_ON_PAGE;
		} else {
			key->data = rip->key;
			key->size = rip->size;
			WT_RET(__wt_bt_build_key_item(
			    toc, key, &key_item, &key_ovfl));
			key_loc = KEY_OFF_PAGE;
		}

		len = 0;
		switch (key_loc) {
		case KEY_OFF_PAGE:
			len = WT_ITEM_SPACE_REQ(key->size);
			break;
		case KEY_ON_PAGE:
			len = key->size;
			break;
		case KEY_NONE:
			break;
		}
		switch (data_loc) {
		case DATA_OFF_PAGE:
			len += WT_ITEM_SPACE_REQ(data->size);
			break;
		case DATA_ON_PAGE:
			len += data->size;
			break;
		}
		if (len > new->space_avail) {
			fprintf(stderr, "PAGE GREW, SPLIT\n");
			__wt_abort(toc->env);
		}

		switch (key_loc) {
		case KEY_ON_PAGE:
			memcpy(new->first_free, key->data, key->size);
			new->first_free += key->size;
			new->space_avail -= key->size;
			++hdr->u.entries;
			break;
		case KEY_OFF_PAGE:
			memcpy(new->first_free, &key_item, sizeof(key_item));
			memcpy(new->first_free +
			    sizeof(key_item), key->data, key->size);
			new->first_free += WT_ITEM_SPACE_REQ(key->size);
			new->space_avail -= WT_ITEM_SPACE_REQ(key->size);
			++hdr->u.entries;
			break;
		case KEY_NONE:
			break;
		}
		switch (data_loc) {
		case DATA_ON_PAGE:
			memcpy(new->first_free, data->data, data->size);
			new->first_free += data->size;
			new->space_avail -= data->size;
			++hdr->u.entries;
			break;
		case DATA_OFF_PAGE:
			memcpy(new->first_free, &data_item, sizeof(data_item));
			memcpy(new->first_free +
			    sizeof(data_item), data->data, data->size);
			new->first_free += WT_ITEM_SPACE_REQ(data->size);
			new->space_avail -= WT_ITEM_SPACE_REQ(data->size);
			++hdr->u.entries;
		}
		++new->records;
	}

	WT_ASSERT(toc->env, __wt_bt_verify_page(toc, new, NULL) == 0);

	return (__wt_page_write(db, new));
}

/*
 * __wt_bt_page_discard --
 *	Free all memory associated with a page.
 */
void
__wt_bt_page_discard(ENV *env, WT_PAGE *page)
{
	WT_COL_INDX *cip;
	WT_ROW_INDX *rip;
	u_int32_t i;
	void *bp, *ep, *last_key;

	WT_ASSERT(env, page->modified == 0);
	WT_ENV_FCHK_ASSERT(
	    env, "__wt_bt_page_discard", page->flags, WT_APIMASK_WT_PAGE);

	switch (page->hdr->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		WT_INDX_FOREACH(page, cip, i)
			if (cip->repl != NULL)
				__wt_bt_page_discard_repl(env, cip->repl);
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		bp = (u_int8_t *)page->hdr;
		ep = (u_int8_t *)bp + page->size;
		last_key = NULL;
		WT_INDX_FOREACH(page, rip, i) {
			/*
			 * For each entry, see if the key was an allocation,
			 * that is, if it points somewhere other than the
			 * original page.  If it's an allocation, free it.
			 *
			 * Only handle the first entry for a duplicate, the
			 * others simply point to the same chunk of memory.
			 */
			if (rip->key != last_key &&
			    (rip->key < bp || rip->key >= ep)) {
				last_key = rip->key;
				__wt_free(env, rip->key, rip->size);
			}

			/*
			 * For each entry, see if data replacement was made,
			 * if so, free the replacements.
			 */
			if (rip->repl != NULL)
				__wt_bt_page_discard_repl(env, rip->repl);
		}
		break;
	case WT_PAGE_DESCRIPT:
	case WT_PAGE_OVFL:
	default:
		break;
	}

	if (page->u.indx != NULL)
		__wt_free(env, page->u.indx, 0);

	__wt_free(env, page->hdr, page->size);
	__wt_free(env, page, sizeof(WT_PAGE));
}

/*
 * __wt_bt_page_discard_repl --
 *	Discard the replacement array.
 */
static void
__wt_bt_page_discard_repl(ENV *env, WT_SDBT *repl)
{
	WT_SDBT *trepl;
	u_int i;

	/* Free the data pointers and then the WT_REPL structure itself. */
	while ((trepl = repl) != NULL) {
		for (i = 0; i < WT_REPL_CHUNK; ++i, ++repl)
			if (repl->data != NULL &&
			    !WT_SDBT_DELETED_ISSET(repl->data))
				__wt_free(env, repl->data, repl->size);

		/*
		 * The last slot in the array is fake -- if it's non-NULL,
		 * it points to a previous array which we also walk.
		 */
		repl = repl->data == NULL ? NULL : (WT_SDBT *)repl->data;
		__wt_free(env, trepl, sizeof(WT_SDBT));
	}
}
