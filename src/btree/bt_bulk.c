/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_bulk_fix(SESSION *, void (*)(const char *,
		uint64_t), int (*)(BTREE *, WT_ITEM **, WT_ITEM **));
static int __wt_bulk_ovfl_copy(SESSION *, WT_OVFL *, WT_OVFL *);
static int __wt_bulk_ovfl_write(SESSION *, WT_ITEM *, WT_OVFL *);
static int __wt_bulk_promote(SESSION *, WT_PAGE *, WT_STACK *, u_int);
static int __wt_bulk_scratch_page(
		SESSION *, uint32_t, uint32_t, WT_PAGE **, WT_BUF **);
static int __wt_bulk_stack_put(SESSION *, WT_STACK *);
static int __wt_bulk_var(SESSION *, void (*)(const char *,
		uint64_t), int (*)(BTREE *, WT_ITEM **, WT_ITEM **));

/*
 * __wt_init_ff_and_sa --
 *	Initialize first-free and space-available values for a page.
 */
static inline void
__wt_init_ff_and_sa(
    WT_PAGE *page, uint8_t **first_freep, uint32_t *space_availp)
{
	uint8_t *p;

	*first_freep = p = WT_PAGE_DISK_BYTE(page->XXdsk);
	*space_availp = page->size - (uint32_t)(p - (uint8_t *)page->XXdsk);
}

/*
 * __wt_page_write --
 *	Write a file page.
 */
static inline int
__wt_page_write(SESSION *session, WT_PAGE *page)
{
	return (
	    __wt_disk_write(session, page->XXdsk, page->addr, page->size));
}

/*
 * __wt_btree_bulk_load --
 *	Db.bulk_load method.
 */
int
__wt_btree_bulk_load(SESSION *session,
    void (*f)(const char *, uint64_t),
    int (*cb)(BTREE *, WT_ITEM **, WT_ITEM **))
{
	BTREE *btree;
	uint32_t addr;

	btree = session->btree;

	/*
	 * XXX
	 * Write out the description record -- this goes away when we figure
	 * out how the table schema is going to work, but for now, we use the
	 * first sector, and this file extend makes sure we don't allocate it
	 * as a table page.
	 */
	WT_RET(__wt_block_alloc(session, &addr, btree->allocsize));

	/*
	 * There are two styles of bulk-load: variable length pages or
	 * fixed-length pages.
	 */
	if (F_ISSET(btree, WT_COLUMN) && btree->fixed_len != 0)
		WT_RET(__wt_bulk_fix(session, f, cb));
	else
		WT_RET(__wt_bulk_var(session, f, cb));

	/* Get a permanent root page reference. */
	return (__wt_root_pin(session));
}

/*
 * __wt_bulk_fix
 *	Db.bulk_load method for column-store, fixed-length file pages.
 */
static int
__wt_bulk_fix(SESSION *session,
    void (*f)(const char *, uint64_t),
    int (*cb)(BTREE *, WT_ITEM **, WT_ITEM **))
{
	BTREE *btree;
	WT_ITEM *key, *value;
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	WT_BUF *tmp;
	WT_STACK stack;
	uint64_t insert_cnt;
	uint32_t len, space_avail;
	uint16_t *last_repeat;
	uint8_t *first_free, *last_value;
	int rle, ret;

	btree = session->btree;
	insert_cnt = 0;
	last_value = NULL;	/* Avoid "uninitialized" warning. */
	last_repeat = NULL;	/* Avoid "uninitialized" warning. */
	WT_CLEAR(stack);
	tmp = NULL;

	rle = F_ISSET(btree, WT_RLE) ? 1 : 0;

	/* Figure out how large is the chunk we're storing on the page. */
	len = btree->fixed_len;
	if (rle)
		len += WT_SIZEOF32(uint16_t);

	/* Get a scratch buffer and make it look like our work page. */
	WT_ERR(__wt_bulk_scratch_page(session, btree->leafmin,
	    rle ? WT_PAGE_COL_RLE : WT_PAGE_COL_FIX,&page, &tmp));
	dsk = page->XXdsk;
	dsk->recno = 1;
	__wt_init_ff_and_sa(page, &first_free, &space_avail);

	while ((ret = cb(btree, &key, &value)) == 0) {
		if (key != NULL) {
			__wt_err(session, 0,
			    "column-store keys are implied and should not "
			    "be set by the bulk load input routine");
			ret = WT_ERROR;
			goto err;
		}
		if (value->size != btree->fixed_len)
			WT_ERR(__wt_file_wrong_fixed_size(
			    session, value->size, btree->fixed_len));

		/*
		 * We use the high bit of the value as a "deleted" value,
		 * make sure the user's value doesn't set it.
		 */
		if (WT_FIX_DELETE_ISSET(value->data)) {
			__wt_err(session, 0,
			    "the first bit may not be stored in fixed-length "
			    "column-store file items");
			ret = WT_ERROR;
			goto err;
		}

		/* Report on progress every 100 inserts. */
		if (f != NULL && ++insert_cnt % 100 == 0)
			f(session->name, insert_cnt);
		WT_STAT_INCR(btree->stats, FILE_ITEMS_INSERTED);

		/*
		 * If doing run-length encoding, check to see if this record
		 * matches the last value inserted.   If there's a match try
		 * and increment that item's repeat count instead of entering
		 * new value.
		 */
		if (rle && dsk->u.entries != 0)
			if (*last_repeat < UINT16_MAX &&
			    memcmp(last_value, value->data, value->size) == 0) {
				++*last_repeat;
				continue;
			}

		/*
		 * We now have the value to store on the page.  If there
		 * is insufficient space on the current page, allocate a new
		 * one.
		 */
		if (len > space_avail) {
			/*
			 * We've finished with the page: promote its first key
			 * to its parent and discard it, then switch to the new
			 * page.
			 */
			WT_ERR(__wt_bulk_promote(session, page, &stack, 0));
			WT_ERR(__wt_page_write(session, page));
			dsk->u.entries = 0;
			dsk->recno = insert_cnt;
			WT_ERR(__wt_block_alloc(session,
			    &page->addr, btree->leafmin));
			__wt_init_ff_and_sa(page, &first_free, &space_avail);
		}
		++dsk->u.entries;

		/*
		 * Copy the value onto the page -- if doing run-length
		 * encoding, track the location of the item for comparison.
		 */
		if (rle) {
			last_repeat = (uint16_t *)first_free;
			*last_repeat = 1;
			first_free += sizeof(uint16_t);
			space_avail -= WT_SIZEOF32(uint16_t);
			last_value = first_free;
		}
		memcpy(first_free, value->data, value->size);
		first_free += value->size;
		space_avail -= value->size;
	}

	/* A ret of 1 just means we've reached the end of the input. */
	if (ret != 1)
		goto err;
	ret = 0;

	/* Promote a key from any partially-filled page and write it. */
	if (dsk->u.entries != 0) {
		ret = __wt_bulk_promote(session, page, &stack, 0);
		WT_ERR(__wt_page_write(session, page));
	}

	/* Wrap up reporting. */
	if (f != NULL)
		f(session->name, insert_cnt);

err:	WT_TRET(__wt_bulk_stack_put(session, &stack));
	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_bulk_var --
 *	Db.bulk_load method for row or column-store variable-length file
 *	pages.
 */
static int
__wt_bulk_var(SESSION *session,
    void (*f)(const char *, uint64_t),
    int (*cb)(BTREE *, WT_ITEM **, WT_ITEM **))
{
	BTREE *btree;
	WT_ITEM *key, *value, key_copy, value_copy;
	WT_CELL key_item, value_item;
	WT_OVFL key_ovfl, value_ovfl;
	WT_PAGE *page;
	WT_BUF *tmp;
	WT_STACK stack;
	uint64_t insert_cnt;
	uint32_t space_avail, space_req;
	uint8_t *first_free, page_type;
	int is_column, ret;

	btree = session->btree;
	tmp = NULL;
	ret = 0;

	WT_CLEAR(stack);
	insert_cnt = 0;
	is_column = F_ISSET(btree, WT_COLUMN) ? 1 : 0;

	WT_CLEAR(key_copy);
	WT_CLEAR(key_item);
	WT_CLEAR(value_copy);

	/* Get a scratch buffer and make it look like our work page. */
	page_type = is_column ? WT_PAGE_COL_VAR : WT_PAGE_ROW_LEAF;
	WT_ERR(__wt_bulk_scratch_page(
	    session, btree->leafmin, page_type,&page, &tmp));
	__wt_init_ff_and_sa(page, &first_free, &space_avail);
	if (is_column)
		page->XXdsk->recno = 1;

	while ((ret = cb(btree, &key, &value)) == 0) {
		if (F_ISSET(btree, WT_COLUMN) ) {
			if (key != NULL) {
				__wt_err(session, 0,
				    "column-store keys are implied and should "
				    "not be returned by the bulk load input "
				    "routine");
				ret = WT_ERROR;
				goto err;
			}
		} else {
			if (key == NULL) {
				ret = WT_ERROR;
				goto err;
			}
			if (key != NULL && key->size == 0) {
				__wt_err(session, 0,
				    "zero-length keys are not supported");
				ret = WT_ERROR;
				goto err;
			}
		}

		/* Report on progress every 100 inserts. */
		if (f != NULL && ++insert_cnt % 100 == 0)
			f(session->name, insert_cnt);
		WT_STAT_INCR(btree->stats, FILE_ITEMS_INSERTED);

		/*
		 * We don't have a key to store on the page if we're building a
		 * column-store; the check from here on is if "key == NULL".
		 *
		 * We don't store values if the length of the value is 0;
		 * the check from here on is if "value == NULL".
		 *
		 * Copy the caller's key/value pair, we don't want to modify
		 * them.  But, copy them carefully, all we want is a pointer
		 * and a length.
		 */
		if (key != NULL) {
			key_copy.data = key->data;
			key_copy.size = key->size;
			key = &key_copy;
		}
		if (value->size == 0 && !is_column)
			value = NULL;
		else {
			value_copy.data = value->data;
			value_copy.size = value->size;
			value = &value_copy;
		}

		/* Build the key/value items we'll store on the page. */
		if (key != NULL)
			WT_ERR(__wt_item_build_key(session,
			    (WT_ITEM *)key, &key_item, &key_ovfl));
		if (value != NULL)
			WT_ERR(__wt_item_build_value(session,
			    (WT_ITEM *)value, &value_item, &value_ovfl));

		/*
		 * We now have the key/value items to store on the page.  If
		 * there is insufficient space on the current page, allocate
		 * a new one.
		 */
		space_req = 0;
		if (key != NULL)
			space_req += WT_CELL_SPACE_REQ(key->size);
		if (value != NULL)
			space_req += WT_CELL_SPACE_REQ(value->size);
		if (space_req > space_avail) {
			/*
			 * We've finished with the page: promote its first key
			 * to its parent and discard it, then switch to the new
			 * page.
			 */
			WT_ERR(__wt_bulk_promote(session, page, &stack, 0));
			WT_ERR(__wt_page_write(session, page));
			__wt_scr_release(&tmp);

			/*
			 * XXX
			 * The obvious speed-up here is to re-initialize page
			 * instead of discarding it and acquiring it again as
			 * as soon as the just-allocated page fills up.  I am
			 * not doing that deliberately: eventually we'll use
			 * asynchronous I/O in bulk load, which means the page
			 * won't be reusable until the I/O completes.
			 */
			WT_ERR(__wt_bulk_scratch_page(session,
			    btree->leafmin, page_type, &page, &tmp));
			__wt_init_ff_and_sa(page, &first_free, &space_avail);
			if (is_column)
				page->XXdsk->recno = insert_cnt;
		}

		/* Copy the key item onto the page. */
		if (key != NULL) {
			++page->XXdsk->u.entries;
			memcpy(first_free, &key_item, sizeof(key_item));
			memcpy(first_free +
			    sizeof(key_item), key->data, key->size);
			space_avail -= WT_CELL_SPACE_REQ(key->size);
			first_free += WT_CELL_SPACE_REQ(key->size);
		}

		/* Copy the data item onto the page. */
		if (value != NULL) {
			++page->XXdsk->u.entries;
			memcpy(first_free, &value_item, sizeof(value_item));
			memcpy(first_free +
			    sizeof(value_item), value->data, value->size);
			space_avail -= WT_CELL_SPACE_REQ(value->size);
			first_free += WT_CELL_SPACE_REQ(value->size);
		}
	}

	/* A ret of 1 just means we've reached the end of the input. */
	if (ret != 1)
		goto err;
	ret = 0;

	/* Promote a key from any partially-filled page and write it. */
	if (page->XXdsk->u.entries != 0) {
		WT_ERR(__wt_bulk_promote(session, page, &stack, 0));
		WT_ERR(__wt_page_write(session, page));
	}

	/* Wrap up reporting. */
	if (f != NULL)
		f(session->name, insert_cnt);

err:	WT_TRET(__wt_bulk_stack_put(session, &stack));
	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_bulk_init --
 *	Start a bulk load.
 */
int
__wt_bulk_init(CURSOR_BULK *cbulk)
{
	BTREE *btree;
	SESSION *session;
	uint32_t addr;
	int is_column;

	btree = cbulk->cbt.btree;
	session = (SESSION *)cbulk->cbt.iface.session;
	is_column = F_ISSET(btree, WT_COLUMN) ? 1 : 0;

	session->btree = btree;

	/*
	 * XXX
	 * Write out the description record -- this goes away when we figure
	 * out how the table schema is going to work, but for now, we use the
	 * first sector, and this file extend makes sure we don't allocate it
	 * as a table page.
	 */
	WT_RET(__wt_block_alloc(session, &addr, 512));

	cbulk->tmp = NULL;

	WT_CLEAR(cbulk->stack);
	cbulk->insert_cnt = 0;

	/* Get a scratch buffer and make it look like our work page. */
	WT_RET(__wt_bulk_scratch_page(session, btree->leafmin,
	    is_column ? WT_PAGE_COL_VAR : WT_PAGE_ROW_LEAF,
	    &cbulk->page, &cbulk->tmp));
	__wt_init_ff_and_sa(cbulk->page,
	    &cbulk->first_free, &cbulk->space_avail);
	if (is_column)
		cbulk->page->XXdsk->recno = 1;

	return (0);
}

/*
 * __wt_bulk_var_insert --
 *	Db.bulk_load method for row or column-store variable-length file pages.
 */
int
__wt_bulk_var_insert(CURSOR_BULK *cbulk)
{
	BTREE *btree;
	SESSION *session;
	WT_CURSOR *cursor;
	WT_ITEM *key, *value, key_copy, value_copy;
	WT_CELL key_item, value_item;
	WT_OVFL key_ovfl, value_ovfl;
	uint32_t space_req;
	int is_column;

	cursor = &cbulk->cbt.iface;
	btree = cbulk->cbt.btree;
	session = (SESSION *)cursor->session;
	is_column = F_ISSET(btree, WT_COLUMN) ? 1 : 0;

	WT_CLEAR(key_copy);
	WT_CLEAR(key_item);
	WT_CLEAR(value_copy);

#if 0
	/* XXX move checking to the cursor layer */
	if (F_ISSET(btree, WT_COLUMN) ) {
		if (key != NULL) {
			__wt_errx(session,
			    "column-store keys are implied and should "
			    "not be returned by the bulk load input "
			    "routine");
			ret = WT_ERROR;
			goto err;
		}
	} else {
		if (key == NULL) {
			ret = WT_ERROR;
			goto err;
		}
		if (key != NULL && key->size == 0) {
			__wt_errx(session,
			    "zero-length keys are not supported");
			ret = WT_ERROR;
			goto err;
		}
	}
#endif

	WT_STAT_INCR(btree->stats, FILE_ITEMS_INSERTED);

	/*
	 * We don't have a key to store on the page if we're building a
	 * column-store; the check from here on is if "key == NULL".
	 *
	 * We don't store values if the length of the value is 0;
	 * the check from here on is if "value == NULL".
	 */
	if (is_column)
		key = NULL;
	else {
		key_copy = *(WT_ITEM *)&cursor->key;
		key = &key_copy;
	}
	if (cursor->value.size == 0 && !is_column)
		value = NULL;
	else {
		value_copy = *(WT_ITEM *)&cursor->value;
		value = &value_copy;
	}

	/* Build the key/value items we're going to store on the page. */
	if (key != NULL)
		WT_RET(__wt_item_build_key(session, key, &key_item, &key_ovfl));
	if (value != NULL)
		WT_RET(__wt_item_build_value(session,
		    value, &value_item, &value_ovfl));

	/*
	 * We now have the key/value items to store on the page.  If
	 * there is insufficient space on the current page, allocate
	 * a new one.
	 */
	space_req = 0;
	if (key != NULL)
		space_req += WT_CELL_SPACE_REQ(key->size);
	if (value != NULL)
		space_req += WT_CELL_SPACE_REQ(value->size);
	if (space_req > cbulk->space_avail) {
		/*
		 * We've finished with the page: promote its first key
		 * to its parent and discard it, then switch to the new
		 * page.
		 */
		WT_RET(__wt_bulk_promote(session,
		    cbulk->page, &cbulk->stack, 0));
		WT_RET(__wt_page_write(session, cbulk->page));
		__wt_scr_release(&cbulk->tmp);

		/*
		 * XXX
		 * The obvious speed-up here is to re-initialize page
		 * instead of discarding it and acquiring it again as
		 * as soon as the just-allocated page fills up.  I am
		 * not doing that deliberately: eventually we'll use
		 * asynchronous I/O in bulk load, which means the page
		 * won't be reusable until the I/O completes.
		 */
		WT_RET(__wt_bulk_scratch_page(session, btree->leafmin,
		    is_column ? WT_PAGE_COL_VAR : WT_PAGE_ROW_LEAF,
		    &cbulk->page, &cbulk->tmp));
		__wt_init_ff_and_sa(cbulk->page,
		    &cbulk->first_free, &cbulk->space_avail);
		if (is_column)
			cbulk->page->XXdsk->recno = ++cbulk->insert_cnt;
	}

	/* Copy the key item onto the page. */
	if (key != NULL) {
		++cbulk->page->XXdsk->u.entries;
		memcpy(cbulk->first_free, &key_item, sizeof(key_item));
		memcpy(cbulk->first_free +
		    sizeof(key_item), key->data, key->size);
		cbulk->space_avail -= WT_CELL_SPACE_REQ(key->size);
		cbulk->first_free += WT_CELL_SPACE_REQ(key->size);
	}

	/* Copy the data item onto the page. */
	if (value != NULL) {
		++cbulk->page->XXdsk->u.entries;
		memcpy(cbulk->first_free, &value_item, sizeof(value_item));
		memcpy(cbulk->first_free +
		    sizeof(value_item), value->data, value->size);
		cbulk->space_avail -= WT_CELL_SPACE_REQ(value->size);
		cbulk->first_free += WT_CELL_SPACE_REQ(value->size);
	}

	return (0);
}

/*
 * __wt_bulk_end --
 *	Clean up after a bulk load.
 */
int
__wt_bulk_end(CURSOR_BULK *cbulk)
{
	SESSION *session;
	int ret;

	session = (SESSION *)cbulk->cbt.iface.session;
	ret = 0;

	/* Promote a key from any partially-filled page and write it. */
	if (cbulk->page->XXdsk->u.entries != 0) {
		WT_ERR(__wt_bulk_promote(session,
		    cbulk->page, &cbulk->stack, 0));
		WT_ERR(__wt_page_write(session, cbulk->page));
	}

err:	WT_TRET(__wt_bulk_stack_put(session, &cbulk->stack));
	if (cbulk->tmp != NULL)
		__wt_scr_release(&cbulk->tmp);

	/* Get a permanent root page reference. */
	if (ret == 0)
		ret = __wt_root_pin(session);

	return (ret);
}

/*
 * __wt_bulk_promote --
 *	Promote the first entry on a page to its parent.
 */
static int
__wt_bulk_promote(SESSION *session, WT_PAGE *page, WT_STACK *stack, u_int level)
{
	BTREE *btree;
	WT_ITEM *key, key_build;
	WT_CELL *key_item, item;
	WT_OFF off;
	WT_OFF_RECORD off_record;
	WT_OVFL tmp_ovfl;
	WT_PAGE *next, *parent;
	WT_PAGE_DISK *dsk;
	WT_BUF *next_tmp;
	WT_STACK_ELEM *elem;
	uint32_t next_space_avail;
	uint8_t *next_first_free;
	u_int type;
	int need_promotion, ret;
	void *parent_data;

	btree = session->btree;
	dsk = page->XXdsk;
	WT_CLEAR(item);
	next_tmp = NULL;
	next = parent = NULL;
	ret = 0;

	/*
	 * If it's a row-store, get a copy of the first item on the page -- it
	 * might be an overflow item, in which case we need to make a copy for
	 * the parent.  Most versions of Berkeley DB tried to reference count
	 * overflow items if they were promoted to internal pages.  That turned
	 * out to be hard to get right, so I'm not doing it again.
	 *
	 * If it's a column-store page, we don't promote a key at all.
	 */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		key = NULL;
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		key = &key_build;
		WT_CLEAR(key_build);

		key_item = WT_PAGE_DISK_BYTE(page->XXdsk);
		switch (WT_CELL_TYPE(key_item)) {
		case WT_CELL_KEY:
			key->data = WT_CELL_BYTE(key_item);
			key->size = WT_CELL_LEN(key_item);
			WT_CELL_SET(&item, WT_CELL_KEY, key->size);
			break;
		case WT_CELL_KEY_OVFL:
			/*
			 * Assume overflow keys remain overflow keys when they
			 * are promoted; not necessarily true if internal nodes
			 * are larger than leaf nodes), but that's unlikely.
			 */
			WT_CLEAR(tmp_ovfl);
			WT_RET(__wt_bulk_ovfl_copy(session,
			    WT_CELL_BYTE_OVFL(key_item), &tmp_ovfl));
			key->data = &tmp_ovfl;
			key->size = sizeof(tmp_ovfl);
			WT_CELL_SET(&item, WT_CELL_KEY_OVFL, sizeof(WT_OVFL));
			break;
		WT_ILLEGAL_FORMAT(session);
		}
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/*
	 * There are two paths into this code based on whether the page already
	 * has a parent.
	 *
	 * If we have a page with no parent page, create the parent page.  In
	 * this path, there's not much to do -- allocate a parent page, copy
	 * reference information from the page to the parent, and we're done.
	 * This is a modified root-split: we're putting a single key on an
	 * internal page, which is illegal, but we know another page on this
	 * page's level will be created, and it will be promoted to the parent
	 * at some point.  This is case #1.
	 *
	 * The second path into this code is if we have a page and its parent,
	 * but the page's reference information doesn't fit on the parent and
	 * we have to split the parent.  This path has two different cases,
	 * based on whether the page's parent itself has a parent.
	 *
	 * Here's a diagram of case #2, where the parent also has a parent:
	 *
	 * P2 -> P1 -> L	(case #2)
	 *
	 * The promoted key from leaf L won't fit onto P1, and so we split P1:
	 *
	 * P2 -> P1
	 *    -> P3 -> L
	 *
	 * In case #2, allocate P3 and copy reference information from the leaf
	 * page to it, then recursively call the promote code to promote the
	 * first entry from P3 to P2.
	 *
	 * Here's a diagram of case #3, where the parent does not have a parent,
	 * in other words, a root split:
	 *
	 * P1 -> L		(case #3)
	 *
	 * The promoted key from leaf L won't fit onto P1, and so we split P1:
	 *
	 * P1 ->
	 * P2 -> L
	 *
	 * In case #3, we allocate P2, copy reference information from the page
	 * to it, and then recursively call the promote code twice: first to
	 * promote the first entry from P1 to a new page, and again to promote
	 * the first entry from P2 to a new page, creating a new root level of
	 * the tree:
	 *
	 * P3 -> P1
	 *    -> P2 -> L
	 */
	/*
	 * To simplify the rest of the code, check to see if there's room for
	 * another entry in our stack structure.  Allocate the stack in groups
	 * of 20, which is probably big enough for any tree we'll ever see in
	 * the field, we'll never test the realloc code unless we work at it.
	 */
#ifdef HAVE_DIAGNOSTIC
#define	WT_STACK_ALLOC_INCR	2
#else
#define	WT_STACK_ALLOC_INCR	20
#endif
	if (stack->size == 0 || level == stack->size - 1) {
		uint32_t bytes_allocated =
		    stack->size * WT_SIZEOF32(WT_STACK_ELEM);
		WT_RET(__wt_realloc(session, &bytes_allocated,
		    (stack->size + WT_STACK_ALLOC_INCR) * sizeof(WT_STACK_ELEM),
		    &stack->elem));
		stack->size += WT_STACK_ALLOC_INCR;
		/*
		 * Note, the stack structure may be entirely uninitialized here,
		 * that is, everything set to 0 bytes.  That's OK: the level of
		 * the stack starts out at 0, that is, the 0th element of the
		 * stack is the 1st level of internal/parent pages in the tree.
		 */
	}

	elem = &stack->elem[level];
	parent = elem->page;
	if (parent == NULL) {
split:		switch (dsk->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_INT:
		case WT_PAGE_COL_RLE:
		case WT_PAGE_COL_VAR:
			type = WT_PAGE_COL_INT;
			break;
		case WT_PAGE_ROW_INT:
		case WT_PAGE_ROW_LEAF:
			type = WT_PAGE_ROW_INT;
			break;
		WT_ILLEGAL_FORMAT(session);
		}

		WT_ERR(__wt_bulk_scratch_page(
		    session, btree->intlmin, type, &next, &next_tmp));
		__wt_init_ff_and_sa(next, &next_first_free, &next_space_avail);

		/*
		 * Column-store pages set the starting record number to the
		 * starting record number of the promoted leaf -- the new leaf
		 * is always the first record in the new parent's page.  Ignore
		 * the type of the file, it's simpler to just promote 0 up the
		 * tree in in row-store files.
		 */
		next->XXdsk->recno = page->XXdsk->recno;

		/*
		 * If we don't have a parent page, it's case #1 -- allocate the
		 * parent page immediately.
		 */
		if (parent == NULL) {
			/*
			 * Case #1 -- there's no parent, it's a root split, no
			 * additional work.
			 */
			need_promotion = 0;
		} else {
			/*
			 * Case #2 and #3.
			 *
			 * Case #3: a root split, so we have to promote a key
			 * from both of the parent pages: promote the key from
			 * the existing parent page.
			 */
			if (stack->elem[level + 1].page == NULL)
				WT_ERR(__wt_bulk_promote(
				    session, parent, stack, level + 1));
			need_promotion = 1;

			/* Write the last parent page, we have a new one. */
			WT_ERR(__wt_page_write(session, parent));
			__wt_scr_release(&stack->elem[level].tmp);
		}

		/* There's a new parent page, reset the stack. */
		elem = &stack->elem[level];
		elem->page = parent = next;
		elem->first_free = next_first_free;
		elem->space_avail = next_space_avail;
		elem->tmp = next_tmp;
		next = NULL;
		next_first_free = NULL;
		next_space_avail = 0;
		next_tmp = NULL;
	} else
		need_promotion = 0;

	/*
	 * See if the promoted data will fit (if they don't, we have to split).
	 * We don't need to check for overflow keys: if the key was an overflow,
	 * we already created a smaller, on-page version of it.
	 *
	 * If there's room, copy the promoted data onto the parent's page.
	 */
	switch (parent->XXdsk->type) {
	case WT_PAGE_COL_INT:
		if (elem->space_avail < sizeof(WT_OFF_RECORD))
			goto split;

		/*
		 * Create the WT_OFF_RECORD reference, taking the starting recno
		 * from the child page.
		 */
		off_record.addr = page->addr;
		off_record.size = page->size;
		WT_RECNO(&off_record) = page->XXdsk->recno;

		/* Store the data item. */
		++parent->XXdsk->u.entries;
		parent_data = elem->first_free;
		memcpy(elem->first_free, &off_record, sizeof(off_record));
		elem->first_free += sizeof(WT_OFF_RECORD);
		elem->space_avail -= WT_SIZEOF32(WT_OFF_RECORD);

		/* Track the last entry on the page for record count updates. */
		stack->elem[level].data = parent_data;
		break;
	case WT_PAGE_ROW_INT:
		if (elem->space_avail <
		    WT_CELL_SPACE_REQ(sizeof(WT_OFF)) +
		    WT_CELL_SPACE_REQ(key->size))
			goto split;

		/* Store the key. */
		++parent->XXdsk->u.entries;
		memcpy(elem->first_free, &item, sizeof(item));
		memcpy(elem->first_free + sizeof(item), key->data, key->size);
		elem->first_free += WT_CELL_SPACE_REQ(key->size);
		elem->space_avail -= WT_CELL_SPACE_REQ(key->size);

		/* Create the WT_CELL(WT_OFF) reference. */
		WT_CELL_SET(&item, WT_CELL_OFF, sizeof(WT_OFF));
		off.addr = page->addr;
		off.size = page->size;

		/* Store the data item. */
		++parent->XXdsk->u.entries;
		parent_data = elem->first_free;
		memcpy(elem->first_free, &item, sizeof(item));
		memcpy(elem->first_free + sizeof(item), &off, sizeof(off));
		elem->first_free += WT_CELL_SPACE_REQ(sizeof(WT_OFF));
		elem->space_avail -= WT_CELL_SPACE_REQ(sizeof(WT_OFF));

		/* Track the last entry on the page for record count updates. */
		stack->elem[level].data = parent_data;
		break;
	}

	/*
	 * The promotion for case #2 and the second part of case #3 -- promote
	 * the key from the newly allocated internal page to its parent.
	 */
	if (need_promotion)
		WT_RET(__wt_bulk_promote(session, parent, stack, level + 1));

err:	if (next_tmp != NULL)
		__wt_scr_release(&next_tmp);

	return (ret);
}

/*
 * __wt_item_build_key --
 *	Process an inserted key item and return an WT_CELL structure and byte
 *	string to be stored on the page.
 */
int
__wt_item_build_key(
    SESSION *session, WT_ITEM *item, WT_CELL *cell, WT_OVFL *ovfl)
{
	BTREE *btree;
	WT_CURSOR *cursor;
	WT_STATS *stats;

	btree = session->btree;
	cursor = session->cursor;
	stats = btree->stats;

	WT_CELL_CLEAR(cell);

	/*
	 * We're called with a WT_ITEM that references a data/size pair.  We can
	 * re-point that WT_ITEM's data and size fields to other memory, but we
	 * cannot allocate memory in that WT_ITEM -- all we can do is re-point
	 * it.
	 *
	 * Optionally compress the value using the Huffman engine.  For Huffman-
	 * encoded key/data cells, we need additional memory; use the SESSION
	 * key/value return memory: this routine is called during bulk insert
	 * and reconciliation, we aren't returning key/data pairs.
	 */
	if (btree->huffman_key != NULL) {
		WT_RET(__wt_huffman_encode(btree->huffman_key,
		    item->data, item->size, &cursor->key));
		if (cursor->key.size > item->size)
			WT_STAT_INCRV(stats, FILE_HUFFMAN_KEY,
			    cursor->key.size - item->size);
		*item = *(WT_ITEM *)&cursor->key;
	}

	/* Create an overflow object if the data won't fit. */
	if (item->size > btree->leafitemsize) {
		WT_RET(__wt_bulk_ovfl_write(session, item, ovfl));

		item->data = ovfl;
		item->size = sizeof(*ovfl);
		WT_CELL_SET(cell, WT_CELL_KEY_OVFL, item->size);
		WT_STAT_INCR(stats, FILE_OVERFLOW_KEY);
	} else
		WT_CELL_SET(cell, WT_CELL_KEY, item->size);
	return (0);
}

/*
 * __wt_item_build_value --
 *	Process an inserted data item and return an WT_CELL structure and byte
 *	string to be stored on the page.
 */
int
__wt_item_build_value(
    SESSION *session, WT_ITEM *item, WT_CELL *cell, WT_OVFL *ovfl)
{
	BTREE *btree;
	WT_CURSOR *cursor;
	WT_STATS *stats;

	btree = session->btree;
	cursor = session->cursor;
	stats = btree->stats;

	WT_CELL_CLEAR(cell);

	/*
	 * We're called with a WT_ITEM that references a data/size pair.  We can
	 * re-point that WT_ITEM's data and size fields to other memory, but we
	 * cannot allocate memory in that WT_ITEM -- all we can do is re-point
	 * it.
	 *
	 * Optionally compress the value using the Huffman engine.  For Huffman-
	 * encoded key/data cells, we need additional memory; use the SESSION
	 * key/value return memory: this routine is called during bulk insert
	 * and reconciliation, we aren't returning key/data pairs.
	 */
	WT_CELL_SET_TYPE(cell, WT_CELL_DATA);

	/*
	 * Handle zero-length cells quickly -- this is a common value, it's
	 * a deleted column-store variable length cell.
	 */
	if (item->size == 0) {
		WT_CELL_SET_LEN(cell, 0);
		return (0);
	}

	/* Optionally compress the data using the Huffman engine. */
	if (btree->huffman_value != NULL) {
		WT_RET(__wt_huffman_encode(btree->huffman_value,
		    item->data, item->size, &cursor->value));
		if (cursor->value.size > item->size)
			WT_STAT_INCRV(stats, FILE_HUFFMAN_VALUE,
			    cursor->value.size - item->size);
		*item = *(WT_ITEM *)&cursor->value;
	}

	/* Create an overflow object if the data won't fit. */
	if (item->size > btree->leafitemsize) {
		WT_RET(__wt_bulk_ovfl_write(session, item, ovfl));

		item->data = ovfl;
		item->size = sizeof(*ovfl);
		WT_CELL_SET_TYPE(cell, WT_CELL_DATA_OVFL);
		WT_STAT_INCR(stats, FILE_OVERFLOW_DATA);
	}

	WT_CELL_SET_LEN(cell, item->size);
	return (0);
}

/*
 * __wt_bulk_ovfl_copy --
 *	Copy bulk-loaded overflow items in the file, returning the WT_OVFL
 *	structure, filled in.
 */
static int
__wt_bulk_ovfl_copy(SESSION *session, WT_OVFL *from, WT_OVFL *to)
{
	BTREE *btree;
	WT_BUF *tmp;
	uint32_t size;
	int ret;

	btree = session->btree;
	tmp = NULL;

	/* Get a scratch buffer of the appropriate size. */
	size = WT_ALIGN(WT_PAGE_DISK_SIZE + from->size, btree->allocsize);
	WT_RET(__wt_scr_alloc(session, size, &tmp));

	/*
	 * Fill in the return information.
	 *
	 * We don't run the pages through the cache -- that means passing a lot
	 * of messages we don't want to bother with.  We're the only user of the
	 * file, which means we can grab file space whenever we want.
	 */
	WT_ERR(__wt_block_alloc(session, &to->addr, size));
	to->size = from->size;

	/*
	 * Read the overflow page into our scratch buffer and write it out to
	 * the new location, without change.
	 */
	if ((ret = __wt_disk_read(session, tmp->mem, from->addr, size)) == 0)
		ret = __wt_disk_write(session, tmp->mem, to->addr, size);

err:	__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_bulk_ovfl_write --
 *	Store bulk-loaded overflow items in the file, returning the page addr.
 */
static int
__wt_bulk_ovfl_write(SESSION *session, WT_ITEM *item, WT_OVFL *to)
{
	BTREE *btree;
	WT_BUF *tmp;
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	uint32_t size;
	int ret;

	btree = session->btree;
	tmp = NULL;

	/* Get a scratch buffer and make it look like our work page. */
	size = WT_ALIGN(WT_PAGE_DISK_SIZE + item->size, btree->allocsize);
	WT_ERR(
	    __wt_bulk_scratch_page(session, size, WT_PAGE_OVFL, &page, &tmp));

	/* Fill in the return information. */
	to->addr = page->addr;
	to->size = item->size;

	/* Initialize the page header and copy the record into place. */
	dsk = page->XXdsk;
	dsk->u.datalen = item->size;
	memcpy((uint8_t *)dsk + WT_PAGE_DISK_SIZE, item->data, item->size);

	ret = __wt_page_write(session, page);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_bulk_scratch_page --
 *	Allocate a scratch buffer and make it look like a file page.
 */
static int
__wt_bulk_scratch_page(SESSION *session, uint32_t page_size,
    uint32_t page_type, WT_PAGE **page_ret, WT_BUF **tmp_ret)
{
	WT_BUF *tmp;
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	uint32_t size;
	int ret;

	ret = 0;

	/*
	 * Allocate a scratch buffer and make sure it's big enough to hold a
	 * WT_PAGE structure plus the page itself, and clear the memory so
	 * it's never random bytes.
	 */
	size = page_size + WT_SIZEOF32(WT_PAGE);
	WT_ERR(__wt_scr_alloc(session, size, &tmp));
	memset(tmp->mem, 0, size);

	/*
	 * Set up the page and allocate a file address.
	 *
	 * We don't run the leaf pages through the cache -- that means passing
	 * a lot of messages we don't want to bother with.  We're the only user
	 * of the file, which means we can grab file space whenever we want.
	 */
	page = tmp->mem;
	page->XXdsk = dsk =
	    (WT_PAGE_DISK *)((uint8_t *)tmp->mem + sizeof(WT_PAGE));
	WT_ERR(__wt_block_alloc(session, &page->addr, page_size));
	page->size = page_size;
	dsk->type = (uint8_t)page_type;

	*page_ret = page;
	*tmp_ret = tmp;
	return (0);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __wt_bulk_stack_put --
 *	Push out the tree's stack of pages.
 */
static int
__wt_bulk_stack_put(SESSION *session, WT_STACK *stack)
{
	BTREE *btree;
	WT_STACK_ELEM *elem;
	int ret;

	btree = session->btree;
	ret = 0;

	if (stack->elem == NULL)
		return (0);

	for (elem = stack->elem; elem->page != NULL; ++elem) {
		WT_TRET(__wt_page_write(session, elem->page));

		/*
		 * If we've reached the last element in the stack, it's the
		 * root page of the tree.  Update the in-memory root address
		 * and the descriptor record.
		 *
		 * An empty page was allocated when the btree handle was opened,
		 * free it here.
		 */
		if ((elem + 1)->page == NULL) {
			if (btree->root_page.page != NULL) {
				WT_ASSERT(session, !WT_PAGE_IS_MODIFIED(
				    btree->root_page.page));
				__wt_free(session, btree->root_page.page);
				/* Rely on __wt_free to set page to NULL. */
			}

			btree->root_page.addr = elem->page->addr;
			btree->root_page.size = elem->page->size;
			btree->root_page.state = WT_REF_DISK;
			WT_TRET(__wt_desc_write(session));
		}

		__wt_scr_release(&elem->tmp);
	}
	__wt_free(session, stack->elem);

	return (0);
}
