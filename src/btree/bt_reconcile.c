/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

static inline int __wt_block_free_ovfl(WT_TOC *, WT_OVFL *);
static int __wt_rec_col_fix(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_rec_col_int(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_rec_col_rle(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_rec_col_var(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_rec_page_delete(WT_TOC *, WT_PAGE *);
static int __wt_rec_page_write(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_rec_parent_update(WT_TOC *, WT_PAGE *, uint32_t, uint32_t);
static int __wt_rec_row_int(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_rec_row_leaf(WT_TOC *, WT_PAGE *, WT_PAGE *);
static inline void __wt_rec_set_page_size(WT_TOC *, WT_PAGE *, uint8_t *);
static int __wt_rle_expand_compare(const void *, const void *);

/*
 * __wt_block_free_ovfl --
 *	Free an chunk of space, referenced by an overflow structure, to the
 *	underlying file.
 */
static inline int
__wt_block_free_ovfl(WT_TOC *toc, WT_OVFL *ovfl)
{
	DB *db;

	db = toc->db;
	return (__wt_block_free(
	    toc, ovfl->addr, WT_HDR_BYTES_TO_ALLOC(db, ovfl->size)));
}

/*
 * __wt_rec_set_page_size --
 *	Set the page's size to the minimum number of allocation units.
 */
static inline void
__wt_rec_set_page_size(WT_TOC *toc, WT_PAGE *page, uint8_t *first_free)
{
	DB *db;

	db = toc->db;

	/*
	 * Set the page's size to the minimum number of allocation units needed
	 * (the page size can either grow or shrink).
	 *
	 * Set the page size before verifying the page, the verification code
	 * checks for entries that extend past the end of the page, and expects
	 * the WT_PAGE->size field to be valid.
	 */
	page->size = WT_ALIGN(first_free - (uint8_t *)page->dsk, db->allocsize);
}

/*
 * __wt_page_reconcile --
 *	Format an in-memory page to its on-disk format, and write it.
 */
int
__wt_page_reconcile(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	DBT *tmp;
	ENV *env;
	WT_PAGE *new, _new;
	WT_PAGE_DISK *dsk;
	uint32_t max;
	int ret;

	db = toc->db;
	tmp = NULL;
	env = toc->env;
	dsk = page->dsk;

	/* If the page isn't dirty, we should never have been called. */
	WT_ASSERT(env, WT_PAGE_IS_MODIFIED(page));

	WT_VERBOSE(env, WT_VERB_EVICT,
	    (env, "reconcile addr %lu (page %p, type %s)",
	    (u_long)page->addr, page, __wt_page_type_string(dsk)));

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		/*
		 * Fixed-width pages without run-length encoding cannot change
		 * size.
		 */
		max = page->size;
		break;
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_LEAF:
		/*
		 * Other leaf page types can grow, allocate the maximum leaf
		 * page size.
		 */
		max = db->leafmax;
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		/*
		 * All internal page types can grow, allocate the maximum
		 * internal page size.
		 */
		max = db->intlmax;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * Update the disk generation before reading the page.  The workQ will
	 * update the write generation after it makes a change, and if we have
	 * different disk and write generation numbers, the page may be dirty.
	 * We technically requires a flush (the eviction server might run on a
	 * different core before a flush naturally occurred).
	 */
	WT_PAGE_DISK_WRITE(page);
	WT_MEMORY_FLUSH;

	/*
	 * Initialize a WT_PAGE page on the stack and allocate a scratch buffer
	 * for its contents.  We use two pieces of memory because we want the
	 * page contents to be aligned for direct I/O.  The WT_PAGE structure
	 * is relatively small, the stack is fine.
	 */
	WT_CLEAR(_new);
	new = &_new;
	WT_ERR(__wt_scr_alloc(toc, max, &tmp));
	memset(tmp->data, 0, max);
	new->addr = page->addr;
	new->size = max;
	new->dsk = tmp->data;
	new->dsk->recno = dsk->recno;
	new->dsk->lsn_file = dsk->lsn_file;
	new->dsk->lsn_off = dsk->lsn_off;
	new->dsk->type = dsk->type;
	new->dsk->level = dsk->level;

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		WT_ERR(__wt_rec_col_fix(toc, page, new));
		break;
	case WT_PAGE_COL_RLE:
		WT_ERR(__wt_rec_col_rle(toc, page, new));
		break;
	case WT_PAGE_COL_VAR:
		WT_ERR(__wt_rec_col_var(toc, page, new));
		break;
	case WT_PAGE_COL_INT:
		WT_ERR(__wt_rec_col_int(toc, page, new));
		break;
	case WT_PAGE_ROW_INT:
		WT_ERR(__wt_rec_row_int(toc, page, new));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__wt_rec_row_leaf(toc, page, new));
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

	/* Write the new page to disk. */
	WT_ERR(__wt_rec_page_write(toc, page, new));

	/* Free the original disk page. */
	WT_ERR(__wt_block_free(toc, page->addr, page->size));

	/*
	 * Update the backing address.
	 *
	 * XXX
	 * This is more for diagnostic information than anything else, that is,
	 * it changes the page's addr to match the parent page's WT_REF->addr.
	 *
	 * The parent's WT_REF->size may be different, that is, page->size is
	 * the original page size at the original address and the size of the
	 * page's buffer in memory, NOT the size of the newly written page at
	 * the new address.   Do NOT update the size here, otherwise we could
	 * no longer figure out if WT_ROW/WT_COL items reference on-page data
	 * vs. allocated data.
	 */
	page->addr = new->addr;

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_rec_col_int --
 *	Reconcile a column-store internal page.
 */
static int
__wt_rec_col_int(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	WT_COL_REF *cref;
	WT_OFF_RECORD *from;
	WT_PAGE_DISK *dsk;
	uint32_t i, space_avail;
	uint8_t *first_free;

	dsk = new->dsk;
	__wt_init_ff_and_sa(new, &first_free, &space_avail);

	WT_COL_REF_FOREACH(page, cref, i) {
		from = cref->off_record;

		/*
		 * XXX
		 * We don't yet handle splits:  we allocated the maximum page
		 * size, but it still wasn't enough.  We must allocate another
		 * page and split the parent.
		 */
		if (sizeof(WT_OFF_RECORD) > space_avail) {
			fprintf(stderr,
			   "__wt_rec_col_int: page %lu split\n",
			   (u_long)page->addr);
			__wt_abort(toc->env);
		}

		memcpy(first_free, from, sizeof(WT_OFF_RECORD));
		first_free += sizeof(WT_OFF_RECORD);
		space_avail -= WT_SIZEOF32(WT_OFF_RECORD);
		++dsk->u.entries;
	}

	__wt_rec_set_page_size(toc, new, first_free);

	return (0);
}

/*
 * __wt_rec_row_int --
 *	Reconcile a row-store internal page.
 */
static int
__wt_rec_row_int(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	WT_ITEM *key_item, *data_item, *next;
	WT_PAGE_DISK *dsk;
	WT_ROW_REF *rref;
	uint32_t i, len, space_avail;
	uint8_t *first_free;

	dsk = new->dsk;
	__wt_init_ff_and_sa(new, &first_free, &space_avail);

	/*
	 * We have to walk both the WT_ROW structures and the original page --
	 * see the comment at WT_INDX_AND_KEY_FOREACH for details.
	 */
	WT_ROW_REF_AND_KEY_FOREACH(page, rref, key_item, i) {
		/*
		 * Skip deleted pages; if we delete an overflow key, free the
		 * underlying file space.
		 */
		if (WT_ROW_REF_ADDR(rref) == WT_ADDR_DELETED) {
			if (WT_ITEM_TYPE(key_item) == WT_ITEM_KEY_OVFL)
				WT_RET(__wt_block_free_ovfl(
				    toc, WT_ITEM_BYTE_OVFL(key_item)));
			continue;
		}

		/*
		 * Copy the paired items off the old page into the new page.
		 *
		 * XXX
		 * Internal pages can't grow, yet, so we could more easily just
		 * update the old page.   We do the copy because eventually we
		 * will have to split the internal pages, and they'll be able to
		 * grow.
		 */
		data_item = WT_ITEM_NEXT(key_item);
		next = WT_ITEM_NEXT(data_item);
		len = (uint32_t)((uint8_t *)next - (uint8_t *)key_item);

		/*
		 * XXX
		 * We don't yet handle splits: we allocated the maximum page
		 * size, but it still wasn't enough.  We must allocate another
		 * page and split the parent.
		 */
		if (len > space_avail) {
			fprintf(stderr,
			    "__wt_rec_row_int: page %lu split\n",
			    (u_long)page->addr);
			__wt_abort(toc->env);
		}

		memcpy(first_free, key_item, len);
		first_free += len;
		space_avail -= len;
		dsk->u.entries += 2;
	}

	__wt_rec_set_page_size(toc, new, first_free);

	return (0);
}

/*
 * __wt_rec_col_fix --
 *	Reconcile a fixed-width, column-store leaf page (does not handle
 *	run-length encoding).
 */
static int
__wt_rec_col_fix(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	DB *db;
	DBT *tmp;
	ENV *env;
	WT_COL *cip;
	WT_PAGE_DISK *dsk;
	WT_UPDATE *upd;
	uint32_t i, len, space_avail;
	uint8_t *data, *first_free;
	int ret;
	void *cipdata;

	db = toc->db;
	tmp = NULL;
	env = toc->env;
	dsk = new->dsk;
	ret = 0;

	__wt_init_ff_and_sa(new, &first_free, &space_avail);

	/*
	 * We need a "deleted" data item to store on the page.  Make sure the
	 * WT_TOC's scratch buffer is big enough.  Clear the buffer's contents
	 * and set the delete flag.
	 */
	len = db->fixed_len;
	WT_ERR(__wt_scr_alloc(toc, len, &tmp));
	memset(tmp->data, 0, len);
	WT_FIX_DELETE_SET(tmp->data);

	WT_COL_INDX_FOREACH(page, cip, i) {
		cipdata = WT_COL_PTR(dsk, cip);

		/*
		 * Get a reference to the data, on- or off- page, and see if
		 * it's been deleted.
		 */
		if ((upd = WT_COL_UPDATE(page, cip)) != NULL) {
			if (WT_UPDATE_DELETED_ISSET(upd))
				data = tmp->data;	/* Deleted */
			else				/* Updated */
				data = WT_UPDATE_DATA(upd);
		} else if (WT_FIX_DELETE_ISSET(cipdata))
			data = tmp->data;		/* On-disk deleted */
		else					/* On-disk data */
			data = WT_COL_PTR(dsk, cip);

		/*
		 * When reconciling a fixed-width page that doesn't support
		 * run-length encoding, the on-page information can't change
		 * size -- there's no reason to ever split such a page.
		 */
		WT_ASSERT(env, len <= space_avail);

		memcpy(first_free, data, len);
		first_free += len;
		space_avail -= len;
		++dsk->u.entries;
	}

	__wt_rec_set_page_size(toc, new, first_free);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __wt_rec_col_rle --
 *	Reconcile a fixed-width, run-length encoded, column-store leaf page.
 */
static int
__wt_rec_col_rle(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	DB *db;
	DBT *tmp;
	ENV *env;
	WT_COL *cip;
	WT_PAGE_DISK *dsk;
	WT_RLE_EXPAND *exp, **expsort, **expp;
	WT_UPDATE *upd;
	uint64_t recno;
	uint32_t i, len, n_expsort, space_avail;
	uint16_t n, nrepeat, repeat_count;
	uint8_t *data, *first_free, *last_data;
	int from_upd, ret;
	void *cipdata;

	db = toc->db;
	tmp = NULL;
	env = toc->env;
	expsort = NULL;
	dsk = new->dsk;
	n_expsort = 0;			/* Necessary for the sort function */
	last_data = NULL;
	ret = 0;

	__wt_init_ff_and_sa(new, &first_free, &space_avail);

	/*
	 * We need a "deleted" data item to store on the page.  Make sure the
	 * WT_TOC's scratch buffer is big enough.  Clear the buffer's contents
	 * and set the delete flag.
	 */
	len = db->fixed_len + WT_SIZEOF32(uint16_t);
	WT_ERR(__wt_scr_alloc(toc, len, &tmp));
	memset(tmp->data, 0, len);
	WT_RLE_REPEAT_COUNT(tmp->data) = 1;
	WT_FIX_DELETE_SET(WT_RLE_REPEAT_DATA(tmp->data));

	/* Set recno to the first record on the page. */
	recno = page->dsk->recno;
	WT_COL_INDX_FOREACH(page, cip, i) {
		/*
		 * Get a sorted list of any expansion entries we've created for
		 * this set of records.  The sort function returns a NULL-
		 * terminated array of references to WT_RLE_EXPAND structures,
		 * sorted by record number.
		 */
		WT_ERR(
		    __wt_rle_expand_sort(env, page, cip, &expsort, &n_expsort));

		/*
		 *
		 * Generate entries for the new page: loop through the repeat
		 * records, checking for WT_RLE_EXPAND entries that match the
		 * current record number.
		 */
		cipdata = WT_COL_PTR(dsk, cip);
		nrepeat = WT_RLE_REPEAT_COUNT(cipdata);
		for (expp = expsort, n = 1;
		    n <= nrepeat; n += repeat_count, recno += repeat_count) {
			from_upd = 0;
			if ((exp = *expp) != NULL && recno == exp->recno) {
				++expp;

				/* Use the WT_RLE_EXPAND's WT_UPDATE field. */
				upd = exp->upd;
				if (WT_UPDATE_DELETED_ISSET(upd))
					data = tmp->data;
				else {
					from_upd = 1;
					data = WT_UPDATE_DATA(upd);
				}
				repeat_count = 1;
			} else {
				if (WT_FIX_DELETE_ISSET(cipdata))
					data = tmp->data;
				else
					data = cipdata;
				/*
				 * The repeat count is the number of records
				 * up to the next WT_RLE_EXPAND record, or
				 * up to the end of this entry if we have no
				 * more WT_RLE_EXPAND records.
				 */
				if (exp == NULL)
					repeat_count = (nrepeat - n) + 1;
				else
					repeat_count =
					    (uint16_t)(exp->recno - recno);
			}

			/*
			 * In all cases, check the last entry written on the
			 * page to see if it's identical, and increment its
			 * repeat count where possible.
			 */
			if (last_data != NULL &&
			    memcmp(WT_RLE_REPEAT_DATA(last_data),
			    WT_RLE_REPEAT_DATA(data), db->fixed_len) == 0 &&
			    WT_RLE_REPEAT_COUNT(last_data) < UINT16_MAX) {
				WT_RLE_REPEAT_COUNT(last_data) += repeat_count;
				continue;
			}

			/*
			 * XXX
			 * We don't yet handle splits:  we allocated the maximum
			 * leaf page size, but it still wasn't enough.  We must
			 * allocate another leaf page and split the parent.
			 */
			if (len > space_avail) {
				fprintf(stderr,
				    "__wt_rec_col_rle: page %lu split\n",
				    (u_long)page->addr);
				__wt_abort(env);
			}

			/*
			 * Most of the formats already include a repeat count:
			 * specifically the deleted buffer, or any entry we're
			 * copying from the original page.   However, updated
			 * entries are read from a WT_UPDATE structure, which
			 * has no repeat count.
			 */
			last_data = first_free;
			if (from_upd) {
				WT_RLE_REPEAT_COUNT(last_data) = repeat_count;
				memcpy(WT_RLE_REPEAT_DATA(
				    last_data), data, db->fixed_len);
			} else
				memcpy(last_data, data, len);
			first_free += len;
			space_avail -= len;
			++dsk->u.entries;
		}
	}

	__wt_rec_set_page_size(toc, new, first_free);

	/* Free the sort array. */
err:	if (expsort != NULL)
		__wt_free(env, expsort, n_expsort * sizeof(WT_RLE_EXPAND *));

	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_rle_expand_compare --
 *	Qsort function: sort WT_RLE_EXPAND structures based on the record
 *	offset, in ascending order.
 */
static int
__wt_rle_expand_compare(const void *a, const void *b)
{
	WT_RLE_EXPAND *a_exp, *b_exp;

	a_exp = *(WT_RLE_EXPAND **)a;
	b_exp = *(WT_RLE_EXPAND **)b;

	return (a_exp->recno > b_exp->recno ? 1 : 0);
}

/*
 * __wt_rle_expand_sort --
 *	Return the current on-page index's array of WT_RLE_EXPAND structures,
 *	sorted by record offset.
 */
int
__wt_rle_expand_sort(ENV *env,
    WT_PAGE *page, WT_COL *cip, WT_RLE_EXPAND ***expsortp, uint32_t *np)
{
	WT_RLE_EXPAND *exp;
	uint16_t n;

	/* Figure out how big the array needs to be. */
	for (n = 0,
	    exp = WT_COL_RLEEXP(page, cip); exp != NULL; exp = exp->next, ++n)
		;

	/*
	 * Allocate that big an array -- always allocate at least one slot,
	 * our caller expects NULL-termination.
	 */
	if (n >= *np) {
		if (*expsortp != NULL)
			__wt_free(
			    env, *expsortp, *np * sizeof(WT_RLE_EXPAND *));
		WT_RET(__wt_calloc_def(env, (size_t)n + 10, expsortp));
		*np = n + 10;
	}

	/* Enter the WT_RLE_EXPAND structures into the array. */
	for (n = 0,
	    exp = WT_COL_RLEEXP(page, cip); exp != NULL; exp = exp->next, ++n)
		(*expsortp)[n] = exp;

	/* Sort the entries. */
	if (n != 0)
		qsort(*expsortp, (size_t)n,
		    sizeof(WT_RLE_EXPAND *), __wt_rle_expand_compare);

	/* NULL-terminate the array. */
	(*expsortp)[n] = NULL;

	return (0);
}

/*
 * __wt_rec_col_var --
 *	Reconcile a variable-width column-store leaf page.
 */
static int
__wt_rec_col_var(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	enum { DATA_ON_PAGE, DATA_OFF_PAGE } data_loc;
	DBT *data, data_dbt;
	WT_COL *cip;
	WT_ITEM data_item, *item;
	WT_OVFL data_ovfl;
	WT_PAGE_DISK *dsk;
	WT_UPDATE *upd;
	uint32_t i, len, space_avail;
	uint8_t *first_free;

	dsk = new->dsk;
	__wt_init_ff_and_sa(new, &first_free, &space_avail);

	WT_CLEAR(data_dbt);
	WT_CLEAR(data_item);
	data = &data_dbt;

	WT_COL_INDX_FOREACH(page, cip, i) {
		/*
		 * Get a reference to the data: it's either an update value or
		 * the original on-page item.
		 */
		item = WT_COL_PTR(dsk, cip);
		if ((upd = WT_COL_UPDATE(page, cip)) != NULL) {
			/*
			 * If we update overflow data item, free the underlying
			 * file space.
			 */
			if (WT_ITEM_TYPE(item) == WT_ITEM_DATA_OVFL)
				WT_RET(__wt_block_free_ovfl(
				    toc, WT_ITEM_BYTE_OVFL(item)));

			/*
			 * Check for deletion, else build the data's WT_ITEM
			 * chunk from the most recent update value.
			 */
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				WT_CLEAR(data_item);
				WT_ITEM_SET(&data_item, WT_ITEM_DEL, 0);
				len = WT_ITEM_SPACE_REQ(0);
			} else {
				data->data = WT_UPDATE_DATA(upd);
				data->size = upd->size;
				WT_RET(__wt_item_build_data(
				    toc, data, &data_item, &data_ovfl));
				len = WT_ITEM_SPACE_REQ(data->size);
			}
			data_loc = DATA_OFF_PAGE;
		} else {
			data->data = item;
			data->size = WT_ITEM_SPACE_REQ(WT_ITEM_LEN(item));
			len = data->size;
			data_loc = DATA_ON_PAGE;
		}

		/*
		 * XXX
		 * We don't yet handle splits -- we allocated the maximum leaf
		 * page size, but it still wasn't enough.  We must allocate
		 * another leaf page and split the parent.
		 */
		if (len > space_avail) {
			fprintf(stderr,
			    "__wt_rec_col_var: page %lu split\n",
			    (u_long)page->addr);
			__wt_abort(toc->env);
		}

		switch (data_loc) {
		case DATA_ON_PAGE:
			memcpy(first_free, data->data, data->size);
			first_free += data->size;
			space_avail -= data->size;
			break;
		case DATA_OFF_PAGE:
			memcpy(first_free, &data_item, sizeof(data_item));
			memcpy(first_free +
			    sizeof(data_item), data->data, data->size);
			first_free += len;
			space_avail -= len;
		}
		++dsk->u.entries;
	}

	__wt_rec_set_page_size(toc, new, first_free);

	return (0);
}

/*
 * __wt_rec_row_leaf --
 *	Reconcile a row-store leaf page.
 */
static int
__wt_rec_row_leaf(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	enum { DATA_ON_PAGE, DATA_OFF_PAGE, EMPTY_DATA } data_loc;
	DBT *key, key_dbt, *data, data_dbt;
	WT_ITEM data_item, *empty_item, *key_item;
	WT_OVFL data_ovfl;
	WT_PAGE_DISK *dsk;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i, len, space_avail;
	uint8_t *first_free;

	empty_item = &toc->db->idb->empty_item;

	dsk = new->dsk;
	__wt_init_ff_and_sa(new, &first_free, &space_avail);

	WT_CLEAR(data_dbt);
	WT_CLEAR(key_dbt);
	WT_CLEAR(data_item);

	key = &key_dbt;
	data = &data_dbt;

	/*
	 * Walk the page, accumulating key/data pairs.
	 *
	 * We have to walk both the WT_ROW structures and the original page --
	 * see the comment at WT_INDX_AND_KEY_FOREACH for details.
	 */
	WT_ROW_INDX_AND_KEY_FOREACH(page, rip, key_item, i) {
		len = 0;

		/*
		 * Get a reference to the data.  We get the data first because
		 * it may have been deleted, in which case we ignore the pair.
		 */
		if ((upd = WT_ROW_UPDATE(page, rip)) != NULL) {
			/*
			 * If we update overflow data item, free the underlying
			 * file space.
			 */
			switch (WT_ITEM_TYPE(rip->data)) {
			case WT_ITEM_DATA_OVFL:
				WT_RET(__wt_block_free_ovfl(
				    toc, WT_ITEM_BYTE_OVFL(rip->data)));
				break;
			}

			/*
			 * If this key/data pair was deleted, we're done.  If
			 * the key was an overflow item, free the underlying
			 * file space.
			 */
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				if (WT_ITEM_TYPE(key_item) == WT_ITEM_KEY_OVFL)
					WT_RET(__wt_block_free_ovfl(
					    toc, WT_ITEM_BYTE_OVFL(key_item)));
				continue;
			}

			/*
			 * If no data, nothing needs to be copied.  Otherwise,
			 * build the data's WT_ITEM chunk from the most recent
			 * update value.
			 */
			if (upd->size == 0)
				data_loc = EMPTY_DATA;
			else {
				data->data = WT_UPDATE_DATA(upd);
				data->size = upd->size;
				WT_RET(__wt_item_build_data(
				    toc, data, &data_item, &data_ovfl));
				data_loc = DATA_OFF_PAGE;
				len += WT_ITEM_SPACE_REQ(data->size);
			}
		} else {
			/*
			 * Copy the item off the page -- however, when the page
			 * was read into memory, there may not have been a data
			 * item, that is, it may have been zero length.  Catch
			 * that case.
			 */
			if (rip->data == empty_item)
				data_loc = EMPTY_DATA;
			else {
				data->data = rip->data;
				data->size =
				    WT_ITEM_SPACE_REQ(WT_ITEM_LEN(rip->data));
				data_loc = DATA_ON_PAGE;
				len += data->size;
			}
		}

		/* Take the key's WT_ITEM from the original page. */
		key->data = key_item;
		key->size = WT_ITEM_SPACE_REQ(WT_ITEM_LEN(key_item));
		len += key->size;

		/*
		 * XXX
		 * We don't yet handle splits -- we allocated the maximum leaf
		 * page size, but it still wasn't enough.  We must allocate
		 * another leaf page and split the parent.
		 */
		if (len > space_avail) {
			fprintf(stderr, "__wt_rec_row_leaf: page %lu split\n",
			    (u_long)page->addr);
			__wt_abort(toc->env);
		}

		/* Copy the key onto the page. */
		memcpy(first_free, key->data, key->size);
		first_free += key->size;
		space_avail -= key->size;
		++dsk->u.entries;

		switch (data_loc) {
		case DATA_ON_PAGE:
			memcpy(first_free, data->data, data->size);
			first_free += data->size;
			space_avail -= data->size;
			++dsk->u.entries;
			break;
		case DATA_OFF_PAGE:
			memcpy(first_free, &data_item, sizeof(data_item));
			memcpy(first_free +
			    sizeof(WT_ITEM), data->data, data->size);
			first_free += WT_ITEM_SPACE_REQ(data->size);
			space_avail -= WT_ITEM_SPACE_REQ(data->size);
			++dsk->u.entries;
			break;
		case EMPTY_DATA:
			break;
		}
	}

	__wt_rec_set_page_size(toc, new, first_free);

	return (0);
}

/*
 * __wt_rec_page_write --
 *	Write a newly reconciled page.
 */
static int
__wt_rec_page_write(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	ENV *env;
	int ret;

	env = toc->env;

	if (new->dsk->u.entries == 0) {
		/*
		 * We don't need to allocate file space or write the page if
		 * the page has been emptied.
		 */
		WT_VERBOSE(env, WT_VERB_EVICT,
		    (env, "reconcile delete page %lu, size %lu",
		    (u_long)page->addr, (u_long)page->size));

		WT_RET(__wt_rec_page_delete(toc, page));
	} else {
		WT_VERBOSE(env, WT_VERB_EVICT,
		    (env, "reconcile move %lu to %lu, resize %lu to %lu",
		    (u_long)page->addr, (u_long)new->addr,
		    (u_long)page->size, (u_long)new->size));

		/*
		 * Allocate file space for the page.
		 *
		 * The cache eviction server is the only thread allocating space
		 * from the file, so there's no need to do any serialization.
		 */
		WT_RET(__wt_block_alloc(toc, &new->addr, new->size));

		/*
		 * Write the page to disk.
		 *
		 * !!!
		 * This is safe for now, but it's a problem when we switch to
		 * asynchronous I/O: the scenario is (1) schedule the write,
		 * (2) discard the newly-clean in-memory version, (3) another
		 * thread tries to read down the tree before the write finishes.
		 */
		WT_ERR(__wt_page_write(toc, new));

		/* Update the page's parent. */
		WT_ERR(__wt_rec_parent_update(toc, page, new->addr, new->size));
	}

	return (0);

err:	(void)__wt_block_free(toc, new->addr, new->size);
	return (ret);
}

/*
 * __wt_rec_page_delete --
 *	Delete a page from the tree.
 */
static int
__wt_rec_page_delete(WT_TOC *toc, WT_PAGE *page)
{
	WT_COL_REF *cref;
	WT_PAGE *parent;
	WT_PAGE_DISK *dsk;
	WT_ROW_REF *rref;
	uint32_t i;

	/*
	 * An evicted page has no entries, therefore we want to delete it.
	 *
	 * First, set the parent page's address for the page to a special,
	 * non-existent address; at some point, when the WT_REF->state on the
	 * parent page is reset to WT_REF_DISK, the non-existent address will
	 * cause any future request for the page to return WT_PAGE_DELETED as
	 * an error.
	 */
	 WT_RET(__wt_rec_parent_update(toc, page, WT_ADDR_DELETED, 0));

	/*
	 * Now, the tricky part.  Any future reader/writer of the page has to
	 * deal with deleted pages.
	 *
	 * Threads sequentially reading pages (for example, any dump/statistics
	 * threads), skip deleted pages and continue on.  There are no threads
	 * sequentially writing pages, so this is a pretty simple case.
	 *
	 * Threads binarily searching the tree (for example, threads retrieving
	 * or storing key/data pairs) are a harder problem.  Imagine a thread
	 * ending up on a deleted page entry, in other words, a thread getting
	 * the WT_PAGE_DELETED error returned from the page-get function while
	 * descending the tree.
	 *
	 * If that thread is a reader, it's simple and we can return not-found,
	 * obviously the key doesn't exist in the tree.
	 *
	 * If the thread is a writer, we first try to switch from the deleted
	 * entry to a lower page entry.  This works because deleting a page
	 * from the tree's key range is equivalent to merging the page's key
	 * range into the previous page's key range.
	 *
	 * For example, imagine a parent page with 3 child pages: the parent has
	 * 3 keys A, C, and E, and the child pages have the key ranges A-B, C-D
	 * and E-F.  If the page with C-D is deleted, the parent's reference
	 * for the key C is marked deleted and the page with the key range A-B
	 * immediately extends to the range A-D because future searches for keys
	 * starting with C or D will compare greater than A and less than E, and
	 * will descend into the page referenced by the key A.  In other words,
	 * the search key associated with any page is the lowest key associated
	 * with the page and there's no upper bound.   By switching our writing
	 * thread to the lower entry, we're mimicing what will happen on future
	 * searches.
	 *
	 * If there's no lower page entry (if, in the example, we deleted the
	 * key A and the child page with the range A-B), the thread switches to
	 * the next larger page entry.  This works because the search algorithm
	 * that landed us on this page ignores any lower boundary for the first
	 * entry in the page.  In other words, if there's no lower entry on the
	 * page, then the next larger entry, as the lowest entry on the page,
	 * holds the lowest keys referenced from this page.  Again, by switching
	 * our writing thread to the larger entry, we're mimicing what happens
	 * on future searches.
	 *
	 * If there's no entry at all, either smaller or larger (that is, the
	 * parent no longer references any valid entries at all), it gets hard.
	 * In that case, we can't fix our writer's position: our writer is in
	 * the wrong part of the tree, a section of the tree being discarded.
	 * In this case, we return the WT_RESTART error up the call stack and
	 * restart the search operation.  Unfortunately, that's not sufficient:
	 * a restarted search will only come back into the same page without
	 * any valid entries, encounter the same error, and infinitely loop.
	 *
	 * To break the infinite loop, we mark the parent's parent reference as
	 * deleted, too.  That way, our restarted search will hit a deleted
	 * entry and be indirected to some other entry, and not back down into
	 * the same page.  A few additional points:
	 *
	 * First, we have to repeat this check all the way up the tree, until
	 * we reach the root or a page that has non-deleted entries.  In other
	 * words, deleting our parent in our parent's parent page might just
	 * result in another page in our search path without any valid entries,
	 * and its the existence of a page of deleted references that results
	 * in the infinite loop.
	 *
	 * Second, we're discarding a chunk of the tree, because no search can
	 * ever reach it.  That's OK: those pages will eventually be selected
	 * for eviction, we're re-discover that they have no entries, and we'll
	 * discard their contents then.
	 *
	 * So, let's get to it: walk the chain of parent pages from the current
	 * page, stopping at the root or the first page with valid entries and
	 * deleting as we go.
	 */
	if ((parent = page->parent) == NULL)
		return (0);
	dsk = parent->dsk;

	/*
	 * Search the current parent page for a valid child page entry.  We can
	 * do this because there's a valid child for the page in the tree (so it
	 * can't be evicted), and we're the eviction thread and no other thread
	 * deletes pages from the tree, so the address fields can't change from
	 * underneath us.  If we find a valid page entry, we're done, we've gone
	 * as far up the tree as we need to go.
	 */
	switch (dsk->type) {
	case WT_PAGE_COL_INT:
		WT_COL_REF_FOREACH(parent, cref, i)
			if (WT_COL_REF_ADDR(cref) != WT_ADDR_DELETED)
				return (0);
		break;
	case WT_PAGE_ROW_INT:
		WT_ROW_REF_FOREACH(parent, rref, i)
			if (WT_ROW_REF_ADDR(rref) != WT_ADDR_DELETED)
				return (0);
		break;
	}

	/*
	 * The current page has no valid child page entries -- lather, rinse,
	 * repeat.
	 */
	return (__wt_rec_page_delete(toc, parent));
}

/*
 * __wt_rec_parent_update --
 *	Update a parent page's reference when a page is reconciled.
 */
static int
__wt_rec_parent_update(WT_TOC *toc, WT_PAGE *page, uint32_t addr, uint32_t size)
{
	DB *db;
	IDB *idb;
	WT_PAGE *parent;
	WT_OFF *off;
	WT_OFF_RECORD *off_record;

	db = toc->db;
	idb = db->idb;

	/*
	 * If we're writing the root of the tree, then we have to update the
	 * descriptor record, there's no parent to update.
	 */
	if (page->addr == idb->root_off.addr) {
		  idb->root_off.addr = addr;
		  idb->root_off.size = size;
		  return (__wt_desc_write(toc));
	 }

	/*
	 * Update the relevant WT_OFF/WT_OFF_RECORD structure.  There are two
	 * memory locations that change (address and size), and we could race,
	 * but that's not a problem.   Only a single thread reconciles pages,
	 * and pages cannot leave memory if they have children.
	 */
	parent = page->parent;
	switch (parent->dsk->type) {
	case WT_PAGE_COL_INT:
		off_record = page->parent_off;
		off_record->addr = addr;
		off_record->size = size;
		break;
	case WT_PAGE_ROW_INT:
		off = page->parent_off;
		off->addr = addr;
		off->size = size;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * Mark the parent page as dirty.
	 *
	 * There's no chance we need to flush this write -- the eviction thread
	 * is the only thread that eventually cares if the page is dirty or not,
	 * and it's our update that's making it dirty.   (The workQ thread does
	 * have to flush its set-modified update, of course).
	 *
	 * We don't care if we race with the workQ; if the workQ thread races
	 * with us, the page will still be marked dirty and that's all we care
	 * about.
	 */
	WT_PAGE_SET_MODIFIED(parent);

	return (0);
}
