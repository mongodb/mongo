/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

/*
 * WT_STACK --
 *	We maintain a stack of parent pages as we build the tree, encapsulated
 *	in this structure.
 */
typedef struct {
	WT_PAGE	*page;				/* page header */
	uint8_t	*first_free;			/* page's first free byte */
	uint32_t space_avail;			/* page's space available */

	DBT	*tmp;				/* page-in-a-buffer */
	void	*data;				/* last on-page WT_COL/WT_ROW */
} WT_STACK_ELEM;
typedef struct {
	WT_STACK_ELEM *elem;			/* stack */
	u_int size;				/* stack size */
} WT_STACK;

static int __wt_bulk_fix(WT_TOC *, void (*)(const char *,
		uint64_t), int (*)(DB *, DBT **, DBT **));
static int __wt_bulk_ovfl_copy(WT_TOC *, WT_OVFL *, WT_OVFL *);
static int __wt_bulk_ovfl_write(WT_TOC *, DBT *, WT_OVFL *);
static int __wt_bulk_promote(WT_TOC *, WT_PAGE *, WT_STACK *, u_int);
static int __wt_bulk_scratch_page(
		WT_TOC *, uint32_t, uint32_t, uint32_t, WT_PAGE **, DBT **);
static int __wt_bulk_stack_put(WT_TOC *, WT_STACK *);
static int __wt_bulk_var(WT_TOC *, void (*)(const char *,
		uint64_t), int (*)(DB *, DBT **, DBT **));
static int __wt_item_build_key(WT_TOC *, DBT *, WT_ITEM *, WT_OVFL *);

/*
 * __wt_db_bulk_load --
 *	Db.bulk_load method.
 */
int
__wt_db_bulk_load(WT_TOC *toc,
    void (*f)(const char *, uint64_t), int (*cb)(DB *, DBT **, DBT **))
{
	DB *db;
	IDB *idb;
	uint32_t addr;

	db = toc->db;
	idb = db->idb;

	/*
	 * XXX
	 * Write out the description record -- this goes away when we figure
	 * out how the table schema is going to work, but for now, we use the
	 * first sector, and this file extend makes sure we don't allocate it
	 * as a table page.
	 */
	WT_RET(__wt_block_alloc(toc, &addr, 512));

	/*
	 * There are two styles of bulk-load: variable length pages or
	 * fixed-length pages.
	 */
	if (F_ISSET(idb, WT_COLUMN) && db->fixed_len != 0)
		WT_RET(__wt_bulk_fix(toc, f, cb));
	else
		WT_RET(__wt_bulk_var(toc, f, cb));

	/* Get a permanent root page reference. */
	return (__wt_root_pin(toc));
}

/*
 * __wt_bulk_fix
 *	Db.bulk_load method for column-store, fixed-length file pages.
 */
static int
__wt_bulk_fix(WT_TOC *toc,
    void (*f)(const char *, uint64_t), int (*cb)(DB *, DBT **, DBT **))
{
	DB *db;
	DBT *key, *data, *tmp;
	IDB *idb;
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	WT_STACK stack;
	uint64_t insert_cnt;
	uint32_t len, space_avail;
	uint16_t *last_repeat;
	uint8_t *first_free, *last_data;
	int rle, ret;

	db = toc->db;
	idb = db->idb;
	insert_cnt = 0;
	last_data = NULL;	/* Avoid "uninitialized" warning. */
	last_repeat = NULL;	/* Avoid "uninitialized" warning. */
	WT_CLEAR(stack);
	tmp = NULL;

	rle = F_ISSET(idb, WT_RLE) ? 1 : 0;

	/* Figure out how large is the chunk we're storing on the page. */
	len = db->fixed_len;
	if (rle)
		len += WT_SIZEOF32(uint16_t);

	/* Get a scratch buffer and make it look like our work page. */
	WT_ERR(__wt_bulk_scratch_page(toc, db->leafmin,
	    rle ? WT_PAGE_COL_RLE : WT_PAGE_COL_FIX, WT_LLEAF, &page, &tmp));
	dsk = page->dsk;
	dsk->recno = 1;
	__wt_init_ff_and_sa(page, &first_free, &space_avail);

	while ((ret = cb(db, &key, &data)) == 0) {
		if (key != NULL) {
			__wt_api_db_errx(db,
			    "column-store keys are implied and should not "
			    "be set by the bulk load input routine");
			ret = WT_ERROR;
			goto err;
		}
		if (data->size != db->fixed_len)
			WT_ERR(__wt_file_wrong_fixed_size(toc, data->size));

		/*
		 * We use the high bit of the data field as a "deleted" value,
		 * make sure the user's data doesn't set it.
		 */
		if (WT_FIX_DELETE_ISSET(data->data)) {
			__wt_api_db_errx(db,
			    "the first bit may not be stored in fixed-length "
			    "column-store file items");
			ret = WT_ERROR;
			goto err;
		}

		/* Report on progress every 100 inserts. */
		if (f != NULL && ++insert_cnt % 100 == 0)
			f(toc->name, insert_cnt);
		WT_STAT_INCR(idb->stats, FILE_ITEMS_INSERTED);

		/*
		 * If doing run-length encoding, check to see if this record
		 * matches the last data inserted.   If there's a match try
		 * and increment that item's repeat count instead of entering
		 * new data.
		 */
		if (rle && dsk->u.entries != 0)
			if (*last_repeat < UINT16_MAX &&
			    memcmp(last_data, data->data, data->size) == 0) {
				++*last_repeat;
				continue;
			}

		/*
		 * We now have the data item to store on the page.  If there
		 * is insufficient space on the current page, allocate a new
		 * one.
		 */
		if (len > space_avail) {
			/*
			 * We've finished with the page: promote its first key
			 * to its parent and discard it, then switch to the new
			 * page.
			 */
			WT_ERR(__wt_bulk_promote(toc, page, &stack, 0));
			WT_ERR(__wt_page_write(toc, page));
			dsk->u.entries = 0;
			dsk->recno = insert_cnt;
			WT_ERR(__wt_block_alloc(toc, &page->addr, db->leafmin));
			__wt_init_ff_and_sa(page, &first_free, &space_avail);
		}
		++dsk->u.entries;

		/*
		 * Copy the data item onto the page -- if doing run-length
		 * encoding, track the location of the item for comparison.
		 */
		if (rle) {
			last_repeat = (uint16_t *)first_free;
			*last_repeat = 1;
			first_free += sizeof(uint16_t);
			space_avail -= WT_SIZEOF32(uint16_t);
			last_data = first_free;
		}
		memcpy(first_free, data->data, data->size);
		first_free += data->size;
		space_avail -= data->size;
	}

	/* A ret of 1 just means we've reached the end of the input. */
	if (ret != 1)
		goto err;
	ret = 0;

	/* Promote a key from any partially-filled page and write it. */
	if (dsk->u.entries != 0) {
		ret = __wt_bulk_promote(toc, page, &stack, 0);
		WT_ERR(__wt_page_write(toc, page));
	}

	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, insert_cnt);

err:	WT_TRET(__wt_bulk_stack_put(toc, &stack));
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
__wt_bulk_var(WT_TOC *toc,
    void (*f)(const char *, uint64_t), int (*cb)(DB *, DBT **, DBT **))
{
	DB *db;
	DBT *key, *data, key_copy, data_copy;
	DBT *tmp;
	IDB *idb;
	WT_ITEM key_item, data_item;
	WT_OVFL key_ovfl, data_ovfl;
	WT_PAGE *page;
	WT_STACK stack;
	uint64_t insert_cnt;
	uint32_t space_avail, space_req;
	uint8_t *first_free, page_type;
	int is_column, ret;

	db = toc->db;
	tmp = NULL;
	idb = db->idb;
	ret = 0;

	WT_CLEAR(stack);
	insert_cnt = 0;
	is_column = F_ISSET(idb, WT_COLUMN) ? 1 : 0;

	WT_CLEAR(data_copy);
	WT_CLEAR(key_copy);
	WT_CLEAR(key_item);

	/* Get a scratch buffer and make it look like our work page. */
	page_type = is_column ? WT_PAGE_COL_VAR : WT_PAGE_ROW_LEAF;
	WT_ERR(__wt_bulk_scratch_page(
	    toc, db->leafmin, page_type, WT_LLEAF, &page, &tmp));
	__wt_init_ff_and_sa(page, &first_free, &space_avail);
	if (is_column)
		page->dsk->recno = 1;

	while ((ret = cb(db, &key, &data)) == 0) {
		if (F_ISSET(idb, WT_COLUMN) ) {
			if (key != NULL) {
				__wt_api_db_errx(db,
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
				__wt_api_db_errx(db,
				    "zero-length keys are not supported");
				ret = WT_ERROR;
				goto err;
			}
		}

		/* Report on progress every 100 inserts. */
		if (f != NULL && ++insert_cnt % 100 == 0)
			f(toc->name, insert_cnt);
		WT_STAT_INCR(idb->stats, FILE_ITEMS_INSERTED);

		/*
		 * We don't have a key to store on the page if we're building a
		 * column-store; the check from here on is if "key == NULL".
		 *
		 * We don't store data items if the length of the data item is
		 * 0; the check from here on is if "data == NULL".
		 *
		 * Copy the caller's DBTs, we don't want to modify them.  But,
		 * copy them carefully, all we want is a pointer and a length.
		 */
		if (key != NULL) {
			key_copy.data = key->data;
			key_copy.size = key->size;
			key = &key_copy;
		}
		if (data->size == 0 && !is_column)
			data = NULL;
		else {
			data_copy.data = data->data;
			data_copy.size = data->size;
			data = &data_copy;
		}

		/* Build the key/data items we're going to store on the page. */
		if (key != NULL)
			WT_ERR(__wt_item_build_key(
			    toc, key, &key_item, &key_ovfl));
		if (data != NULL)
			WT_ERR(__wt_item_build_data(
			    toc, data, &data_item, &data_ovfl));

		/*
		 * We now have the key/data items to store on the page.  If
		 * there is insufficient space on the current page, allocate
		 * a new one.
		 */
		space_req = 0;
		if (key != NULL)
			space_req += WT_ITEM_SPACE_REQ(key->size);
		if (data != NULL)
			space_req += WT_ITEM_SPACE_REQ(data->size);
		if (space_req > space_avail) {
			/*
			 * We've finished with the page: promote its first key
			 * to its parent and discard it, then switch to the new
			 * page.
			 */
			WT_ERR(__wt_bulk_promote(toc, page, &stack, 0));
			WT_ERR(__wt_page_write(toc, page));
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
			WT_ERR(__wt_bulk_scratch_page(toc,
			    db->leafmin, page_type, WT_LLEAF, &page, &tmp));
			__wt_init_ff_and_sa(page, &first_free, &space_avail);
			if (is_column)
				page->dsk->recno = insert_cnt;
		}

		/* Copy the key item onto the page. */
		if (key != NULL) {
			++page->dsk->u.entries;
			memcpy(first_free, &key_item, sizeof(key_item));
			memcpy(first_free +
			    sizeof(key_item), key->data, key->size);
			space_avail -= WT_ITEM_SPACE_REQ(key->size);
			first_free += WT_ITEM_SPACE_REQ(key->size);
		}

		/* Copy the data item onto the page. */
		if (data != NULL) {
			++page->dsk->u.entries;
			memcpy(first_free, &data_item, sizeof(data_item));
			memcpy(first_free +
			    sizeof(data_item), data->data, data->size);
			space_avail -= WT_ITEM_SPACE_REQ(data->size);
			first_free += WT_ITEM_SPACE_REQ(data->size);
		}
	}

	/* A ret of 1 just means we've reached the end of the input. */
	if (ret != 1)
		goto err;
	ret = 0;

	/* Promote a key from any partially-filled page and write it. */
	if (page->dsk->u.entries != 0) {
		WT_ERR(__wt_bulk_promote(toc, page, &stack, 0));
		WT_ERR(__wt_page_write(toc, page));
	}

	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, insert_cnt);

err:	WT_TRET(__wt_bulk_stack_put(toc, &stack));
	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_bulk_promote --
 *	Promote the first entry on a page to its parent.
 */
static int
__wt_bulk_promote(WT_TOC *toc, WT_PAGE *page, WT_STACK *stack, u_int level)
{
	DB *db;
	DBT *key, key_build, *next_tmp;
	ENV *env;
	WT_ITEM *key_item, item;
	WT_OFF off;
	WT_OFF_RECORD off_record;
	WT_OVFL tmp_ovfl;
	WT_PAGE *next, *parent;
	WT_PAGE_DISK *dsk;
	WT_STACK_ELEM *elem;
	uint32_t next_space_avail;
	uint8_t *next_first_free;
	u_int type;
	int need_promotion, ret;
	void *parent_data;

	db = toc->db;
	env = toc->env;
	dsk = page->dsk;
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

		key_item = WT_PAGE_BYTE(page);
		switch (WT_ITEM_TYPE(key_item)) {
		case WT_ITEM_KEY:
			key->data = WT_ITEM_BYTE(key_item);
			key->size = WT_ITEM_LEN(key_item);
			WT_ITEM_SET(&item, WT_ITEM_KEY, key->size);
			break;
		case WT_ITEM_KEY_OVFL:
			/*
			 * Assume overflow keys remain overflow keys when they
			 * are promoted; not necessarily true if internal nodes
			 * are larger than leaf nodes), but that's unlikely.
			 */
			WT_CLEAR(tmp_ovfl);
			WT_RET(__wt_bulk_ovfl_copy(toc,
			    WT_ITEM_BYTE_OVFL(key_item), &tmp_ovfl));
			key->data = &tmp_ovfl;
			key->size = sizeof(tmp_ovfl);
			WT_ITEM_SET(&item, WT_ITEM_KEY_OVFL, sizeof(WT_OVFL));
			break;
		WT_ILLEGAL_FORMAT(db);
		}
		break;
	WT_ILLEGAL_FORMAT(db);
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
		WT_RET(__wt_realloc(env, &bytes_allocated,
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
		}

		WT_ERR(__wt_bulk_scratch_page(toc, db->intlmin,
		    type, (uint32_t)dsk->level + 1, &next, &next_tmp));
		__wt_init_ff_and_sa(next, &next_first_free, &next_space_avail);

		/*
		 * Column-store pages set the starting record number to the
		 * starting record number of the promoted leaf -- the new leaf
		 * is always the first record in the new parent's page.  Ignore
		 * the type of the file, it's simpler to just promote 0 up the
		 * tree in in row-store files.
		 */
		next->dsk->recno = page->dsk->recno;

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
				    toc, parent, stack, level + 1));
			need_promotion = 1;

			/* Write the last parent page, we have a new one. */
			WT_ERR(__wt_page_write(toc, parent));
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
	switch (parent->dsk->type) {
	case WT_PAGE_COL_INT:
		if (elem->space_avail < sizeof(WT_OFF_RECORD))
			goto split;

		/*
		 * Create the WT_OFF_RECORD reference, taking the starting recno
		 * from the child page.
		 */
		off_record.addr = page->addr;
		off_record.size =
		    dsk->level == WT_LLEAF ? db->leafmin : db->intlmin;
		WT_RECNO(&off_record) = page->dsk->recno;

		/* Store the data item. */
		++parent->dsk->u.entries;
		parent_data = elem->first_free;
		memcpy(elem->first_free, &off_record, sizeof(off_record));
		elem->first_free += sizeof(WT_OFF_RECORD);
		elem->space_avail -= WT_SIZEOF32(WT_OFF_RECORD);

		/* Track the last entry on the page for record count updates. */
		stack->elem[level].data = parent_data;
		break;
	case WT_PAGE_ROW_INT:
		if (elem->space_avail <
		    WT_ITEM_SPACE_REQ(sizeof(WT_OFF)) +
		    WT_ITEM_SPACE_REQ(key->size))
			goto split;

		/* Store the key. */
		++parent->dsk->u.entries;
		memcpy(elem->first_free, &item, sizeof(item));
		memcpy(elem->first_free + sizeof(item), key->data, key->size);
		elem->first_free += WT_ITEM_SPACE_REQ(key->size);
		elem->space_avail -= WT_ITEM_SPACE_REQ(key->size);

		/* Create the WT_ITEM(WT_OFF) reference. */
		WT_ITEM_SET(&item, WT_ITEM_OFF, sizeof(WT_OFF));
		off.addr = page->addr;
		off.size = dsk->level == WT_LLEAF ? db->leafmin : db->intlmin;

		/* Store the data item. */
		++parent->dsk->u.entries;
		parent_data = elem->first_free;
		memcpy(elem->first_free, &item, sizeof(item));
		memcpy(elem->first_free + sizeof(item), &off, sizeof(off));
		elem->first_free += WT_ITEM_SPACE_REQ(sizeof(WT_OFF));
		elem->space_avail -= WT_ITEM_SPACE_REQ(sizeof(WT_OFF));

		/* Track the last entry on the page for record count updates. */
		stack->elem[level].data = parent_data;
		break;
	}

	/*
	 * The promotion for case #2 and the second part of case #3 -- promote
	 * the key from the newly allocated internal page to its parent.
	 */
	if (need_promotion)
		WT_RET(__wt_bulk_promote(toc, parent, stack, level + 1));

err:	if (next_tmp != NULL)
		__wt_scr_release(&next_tmp);

	return (ret);
}

/*
 * __wt_item_build_key --
 *	Process an inserted key item and return an WT_ITEM structure and byte
 *	string to be stored on the page.
 */
static int
__wt_item_build_key(WT_TOC *toc, DBT *dbt, WT_ITEM *item, WT_OVFL *ovfl)
{
	DB *db;
	IDB *idb;
	WT_STATS *stats;

	db = toc->db;
	idb = db->idb;
	stats = idb->stats;

	/*
	 * We're called with a DBT that references a data/size pair.  We can
	 * re-point that DBT's data and size fields to other memory, but we
	 * cannot allocate memory in that DBT -- all we can do is re-point it.
	 *
	 * For Huffman-encoded key/data items, we need a chunk of new space;
	 * use the WT_TOC key/data return memory: this routine is called during
	 * bulk insert and reconciliation, we aren't returning key/data pairs.
	 */

	/* Optionally compress the data using the Huffman engine. */
	if (idb->huffman_key != NULL) {
		WT_RET(__wt_huffman_encode(
		    idb->huffman_key, dbt->data, dbt->size,
		    &toc->key.data, &toc->key.mem_size, &toc->key.size));
		if (toc->key.size > dbt->size)
			WT_STAT_INCRV(stats,
			    FILE_HUFFMAN_KEY, toc->key.size - dbt->size);
		dbt->data = toc->key.data;
		dbt->size = toc->key.size;
	}

	/* Create an overflow object if the data won't fit. */
	if (dbt->size > db->leafitemsize) {
		WT_RET(__wt_bulk_ovfl_write(toc, dbt, ovfl));

		dbt->data = ovfl;
		dbt->size = sizeof(*ovfl);
		WT_ITEM_SET(item, WT_ITEM_KEY_OVFL, dbt->size);
		WT_STAT_INCR(stats, FILE_OVERFLOW_KEY);
	} else
		WT_ITEM_SET(item, WT_ITEM_KEY, dbt->size);
	return (0);
}

/*
 * __wt_item_build_data --
 *	Process an inserted data item and return an WT_ITEM structure and byte
 *	string to be stored on the page.
 */
int
__wt_item_build_data(WT_TOC *toc, DBT *dbt, WT_ITEM *item, WT_OVFL *ovfl)
{
	DB *db;
	IDB *idb;
	WT_STATS *stats;

	db = toc->db;
	idb = db->idb;
	stats = idb->stats;

	/*
	 * We're called with a DBT that references a data/size pair.  We can
	 * re-point that DBT's data and size fields to other memory, but we
	 * cannot allocate memory in that DBT -- all we can do is re-point it.
	 *
	 * For Huffman-encoded key/data items, we need a chunk of new space;
	 * use the WT_TOC key/data return memory: this routine is called during
	 * bulk insert and reconciliation, we aren't returning key/data pairs.
	 */
	WT_CLEAR(*item);
	WT_ITEM_SET_TYPE(item, WT_ITEM_DATA);

	/*
	 * Handle zero-length items quickly -- this is a common value, it's
	 * a deleted column-store variable length item.
	 */
	if (dbt->size == 0) {
		WT_ITEM_SET_LEN(item, 0);
		return (0);
	}

	/* Optionally compress the data using the Huffman engine. */
	if (idb->huffman_data != NULL) {
		WT_RET(__wt_huffman_encode(
		    idb->huffman_data, dbt->data, dbt->size,
		    &toc->data.data, &toc->data.mem_size, &toc->data.size));
		if (toc->data.size > dbt->size)
			WT_STAT_INCRV(stats,
			    FILE_HUFFMAN_DATA, toc->data.size - dbt->size);
		dbt->data = toc->data.data;
		dbt->size = toc->data.size;
	}

	/* Create an overflow object if the data won't fit. */
	if (dbt->size > db->leafitemsize) {
		WT_RET(__wt_bulk_ovfl_write(toc, dbt, ovfl));

		dbt->data = ovfl;
		dbt->size = sizeof(*ovfl);
		WT_ITEM_SET_TYPE(item, WT_ITEM_DATA_OVFL);
		WT_STAT_INCR(stats, FILE_OVERFLOW_DATA);
	}

	WT_ITEM_SET_LEN(item, dbt->size);
	return (0);
}

/*
 * __wt_bulk_ovfl_copy --
 *	Copy bulk-loaded overflow items in the file, returning the WT_OVFL
 *	structure, filled in.
 */
static int
__wt_bulk_ovfl_copy(WT_TOC *toc, WT_OVFL *from, WT_OVFL *to)
{
	DB *db;
	DBT *tmp;
	uint32_t size;
	int ret;

	db = toc->db;
	tmp = NULL;

	/* Get a scratch buffer of the appropriate size. */
	size = WT_ALIGN(WT_PAGE_DISK_SIZE + from->size, db->allocsize);
	WT_RET(__wt_scr_alloc(toc, size, &tmp));

	/*
	 * Fill in the return information.
	 *
	 * We don't run the pages through the cache -- that means passing a lot
	 * of messages we don't want to bother with.  We're the only user of the
	 * file, which means we can grab file space whenever we want.
	 */
	WT_ERR(__wt_block_alloc(toc, &to->addr, size));
	to->size = from->size;

	/*
	 * Read the overflow page into our scratch buffer and write it out to
	 * the new location, without change.
	 */
	if ((ret = __wt_disk_read(toc, tmp->data, from->addr, size)) == 0)
		ret = __wt_disk_write(toc, tmp->data, to->addr, size);

err:	__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_bulk_ovfl_write --
 *	Store bulk-loaded overflow items in the file, returning the page addr.
 */
static int
__wt_bulk_ovfl_write(WT_TOC *toc, DBT *dbt, WT_OVFL *to)
{
	DB *db;
	DBT *tmp;
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	uint32_t size;
	int ret;

	db = toc->db;
	tmp = NULL;

	/* Get a scratch buffer and make it look like our work page. */
	size = WT_ALIGN(WT_PAGE_DISK_SIZE + dbt->size, db->allocsize);
	WT_ERR(__wt_bulk_scratch_page(
	    toc, size, WT_PAGE_OVFL, WT_NOLEVEL, &page, &tmp));

	/* Fill in the return information. */
	to->addr = page->addr;
	to->size = dbt->size;

	/* Initialize the page header and copy the record into place. */
	dsk = page->dsk;
	dsk->u.datalen = dbt->size;
	memcpy((uint8_t *)dsk + WT_PAGE_DISK_SIZE, dbt->data, dbt->size);

	ret = __wt_page_write(toc, page);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_bulk_scratch_page --
 *	Allocate a scratch buffer and make it look like a file page.
 */
static int
__wt_bulk_scratch_page(WT_TOC *toc, uint32_t page_size,
    uint32_t page_type, uint32_t page_level, WT_PAGE **page_ret, DBT **tmp_ret)
{
	DBT *tmp;
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
	WT_ERR(__wt_scr_alloc(toc, size, &tmp));
	memset(tmp->data, 0, size);

	/*
	 * Set up the page and allocate a file address.
	 *
	 * We don't run the leaf pages through the cache -- that means passing
	 * a lot of messages we don't want to bother with.  We're the only user
	 * of the file, which means we can grab file space whenever we want.
	 */
	page = tmp->data;
	page->dsk = dsk =
	    (WT_PAGE_DISK *)((uint8_t *)tmp->data + sizeof(WT_PAGE));
	WT_ERR(__wt_block_alloc(toc, &page->addr, page_size));
	page->size = page_size;
	dsk->type = (uint8_t)page_type;
	dsk->level = (uint8_t)page_level;

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
__wt_bulk_stack_put(WT_TOC *toc, WT_STACK *stack)
{
	ENV *env;
	IDB *idb;
	WT_STACK_ELEM *elem;
	int ret;

	env = toc->env;
	idb = toc->db->idb;
	ret = 0;

	if (stack->elem == NULL)
		return (0);

	for (elem = stack->elem; elem->page != NULL; ++elem) {
		WT_TRET(__wt_page_write(toc, elem->page));

		/*
		 * If we've reached the last element in the stack, it's the
		 * root page of the tree.  Update the in-memory root address
		 * and the descriptor record.
		 */
		if ((elem + 1)->page == NULL) {
			idb->root_page.addr = elem->page->addr;
			idb->root_page.size = elem->page->size;
			WT_TRET(__wt_desc_write(toc));
		}

		__wt_scr_release(&elem->tmp);
	}
	__wt_free(env, stack->elem, stack->size * sizeof(WT_STACK_ELEM));

	return (0);
}
