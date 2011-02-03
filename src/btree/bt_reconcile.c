/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_rle_expand_compare(const void *, const void *);
static int __wt_rec_col_fix(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_rec_col_int(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_rec_col_rle(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_rec_col_var(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_rec_page_write(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_rec_parent_update(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_rec_row(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_rec_row_int(WT_TOC *, WT_PAGE *, WT_PAGE *);
static inline void __wt_rec_set_page_size(WT_TOC *, WT_PAGE *, uint8_t *);

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

	/*
	 * Update the disk generation before reading the page.  The workQ will
	 * update the write generation after it makes a change, and if we have
	 * different disk and write generation numbers, the page may be dirty.
	 * We technically requires a flush (the eviction server might run on a
	 * different core before a flush naturally occurred).
	 */
	WT_PAGE_DISK_WRITE(page);
	WT_MEMORY_FLUSH;

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
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		/*
		 * Other leaf page types can grow, allocate the maximum leaf
		 * page size.
		 */
		max = db->leafmax;
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		/*
		 * All internal page types can grow, allocate the maximum
		 * internal page size.
		 */
		max = db->intlmax;
		break;
	case WT_PAGE_OVFL:
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

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
	new->dsk->start_recno = dsk->start_recno;
	new->dsk->type = dsk->type;
	new->dsk->level = dsk->level;
	new->dsk->lsn_file = dsk->lsn_file;
	new->dsk->lsn_off = dsk->lsn_off;

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
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_ERR(__wt_rec_row_int(toc, page, new));
		break;
	case WT_PAGE_ROW_LEAF:
	case WT_PAGE_DUP_LEAF:
		WT_ERR(__wt_rec_row(toc, page, new));
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

	/* Write the new page to disk. */
	WT_ERR(__wt_rec_page_write(toc, page, new));

	/* Free the original page -- update the address and size. */
	WT_ERR(__wt_block_free(toc, page->addr, page->size));

	/*
	 * Update the backing address.
	 *
	 * XXX
	 * This is more for diagnostic information than anything else, that is,
	 * this will match the WT_REF->addr in the parent.
	 *
	 * The parent's WT_REF->size may be different, that is, page->size is
	 * the original page size at the original address and the size of the
	 * page's buffer in memory, NOT the size of the newly written page at
	 * the new address.   We may NOT update the size here, otherwise we
	 * can no longer figure out if WT_ROW/WT_COL items reference on-page
	 * data vs. allocated data.
	 */
	page->addr = new->addr;

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_rec_col_int --
 *	Reconcile a column store internal page.
 */
static int
__wt_rec_col_int(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	WT_COL *cip;
	WT_OFF *from;
	WT_PAGE_DISK *dsk;
	uint32_t i, space_avail;
	uint8_t *first_free;

	dsk = new->dsk;
	__wt_set_ff_and_sa_from_offset(
	    new, WT_PAGE_BYTE(new), &first_free, &space_avail);

	WT_INDX_FOREACH(page, cip, i) {
		from = cip->data;

		/*
		 * XXX
		 * We don't yet handle splits:  we allocated the maximum page
		 * size, but it still wasn't enough.  We must allocate another
		 * page and split the parent.
		 */
		if (sizeof(WT_OFF) > space_avail) {
			fprintf(stderr,
			   "__wt_rec_col_int: page %lu split\n",
			   (u_long)page->addr);
			__wt_abort(toc->env);
		}

		memcpy(first_free, from, sizeof(WT_OFF));
		first_free += sizeof(WT_OFF);
		space_avail -= sizeof(WT_OFF);
		++dsk->u.entries;
	}

	new->records = page->records;
	__wt_rec_set_page_size(toc, new, first_free);

	return (0);
}

/*
 * __wt_rec_row_int --
 *	Reconcile a row store, or off-page duplicate tree, internal page.
 */
static int
__wt_rec_row_int(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	WT_ITEM *key_item, *data_item, *next;
	WT_PAGE_DISK *dsk;
	WT_ROW *rip;
	uint32_t i, len, space_avail;
	uint8_t *first_free;

	dsk = new->dsk;
	__wt_set_ff_and_sa_from_offset(
	    new, WT_PAGE_BYTE(new), &first_free, &space_avail);

	/*
	 * We have to walk the original disk page as well as the current page:
	 * the problem is keys that require processing.
	 *
	 * When a page is read into memory from a file, the WT_ROW is set to
	 * reference an on-page group of bytes in the key and data WT_ITEM
	 * structures.  As Btree keys are immutable, and the key's WT_OFF data
	 * is updated in place, those original WT_ITEMs are usually what we
	 * want write, and we can find them pretty easily.
	 *
	 * When we have to use a key that requires processing (for example, a
	 * Huffman encoded key), we set the WT_ROW key/size pair to reference
	 * the allocated memory that holds the key.  At that point we've lost
	 * any reference to the original WT_ITEM structure.
	 *
	 * We don't want grow the WT_ROW structure by another (void *) bytes,
	 * so walk both the original page and the current WT_PAGE array when
	 * reconciling the page so we can find the original WT_ITEM.
	 */
	key_item = WT_PAGE_BYTE(page);
	WT_INDX_FOREACH(page, rip, i) {
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

		key_item = next;
	}

	new->records = page->records;
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
	WT_REPL *repl;
	uint32_t i, len, space_avail;
	uint8_t *data, *first_free;
	int ret;

	db = toc->db;
	tmp = NULL;
	env = toc->env;
	dsk = new->dsk;
	ret = 0;

	__wt_set_ff_and_sa_from_offset(
	    new, WT_PAGE_BYTE(new), &first_free, &space_avail);

	/*
	 * We need a "deleted" data item to store on the page.  Make sure the
	 * WT_TOC's scratch buffer is big enough.  Clear the buffer's contents
	 * and set the delete flag.
	 */
	len = db->fixed_len;
	WT_ERR(__wt_scr_alloc(toc, len, &tmp));
	memset(tmp->data, 0, len);
	WT_FIX_DELETE_SET(tmp->data);

	WT_INDX_FOREACH(page, cip, i) {
		/*
		 * Get a reference to the data, on- or off- page, and see if
		 * it's been deleted.
		 */
		if ((repl = WT_COL_REPL(page, cip)) != NULL) {
			if (WT_REPL_DELETED_ISSET(repl))
				data = tmp->data;	/* Replaced deleted */
			else				/* Replaced data */
				data = WT_REPL_DATA(repl);
		} else if (WT_FIX_DELETE_ISSET(cip->data))
			data = tmp->data;		/* On-page deleted */
		else
			data = cip->data;		/* On-page data */

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

	new->records = page->records;
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
	WT_REPL *repl;
	uint64_t recno;
	uint32_t i, len, n_expsort, space_avail;
	uint16_t n, nrepeat, repeat_count;
	uint8_t *data, *first_free, *last_data;
	int from_repl, ret;

	db = toc->db;
	tmp = NULL;
	env = toc->env;
	expsort = NULL;
	dsk = new->dsk;
	n_expsort = 0;			/* Necessary for the sort function */
	last_data = NULL;
	ret = 0;

	__wt_set_ff_and_sa_from_offset(
	    new, WT_PAGE_BYTE(new), &first_free, &space_avail);

	/*
	 * We need a "deleted" data item to store on the page.  Make sure the
	 * WT_TOC's scratch buffer is big enough.  Clear the buffer's contents
	 * and set the delete flag.
	 */
	len = db->fixed_len + sizeof(uint16_t);
	WT_ERR(__wt_scr_alloc(toc, len, &tmp));
	memset(tmp->data, 0, len);
	WT_RLE_REPEAT_COUNT(tmp->data) = 1;
	WT_FIX_DELETE_SET(WT_RLE_REPEAT_DATA(tmp->data));

	/* Set recno to the first record on the page. */
	recno = page->dsk->start_recno;
	WT_INDX_FOREACH(page, cip, i) {
		/*
		 * Get a sorted list of any expansion entries we've created for
		 * this set of records.  The sort function returns a NULL-
		 * terminated array of references to WT_RLE_EXPAND structures,
		 * sorted by record number.
		 */
		WT_ERR(__wt_rle_expand_sort(
		    env, page, cip, &expsort, &n_expsort));

		/*
		 *
		 * Generate entries for the new page: loop through the repeat
		 * records, checking for WT_RLE_EXPAND entries that match the
		 * current record number.
		 */
		nrepeat = WT_RLE_REPEAT_COUNT(cip->data);
		for (expp = expsort, n = 1;
		    n <= nrepeat; n += repeat_count, recno += repeat_count) {
			from_repl = 0;
			if ((exp = *expp) != NULL && recno == exp->recno) {
				++expp;

				/* Use the WT_RLE_EXPAND's WT_REPL field. */
				repl = exp->repl;
				if (WT_REPL_DELETED_ISSET(repl))
					data = tmp->data;
				else {
					from_repl = 1;
					data = WT_REPL_DATA(repl);
				}
				repeat_count = 1;
			} else {
				if (WT_FIX_DELETE_ISSET(cip->data))
					data = tmp->data;
				else
					data = cip->data;
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
			 * copying from the original page.   However, entries
			 * that were deleted or replaced are read from a WT_REPL
			 * structure, which has no repeat count.
			 */
			last_data = first_free;
			if (from_repl) {
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

	new->records = page->records;
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
		WT_RET(__wt_calloc(
		    env, n + 10, sizeof(WT_RLE_EXPAND *), expsortp));
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
	DB *db;
	DBT *data, data_dbt;
	WT_COL *cip;
	WT_ITEM data_item, *item;
	WT_OVFL data_ovfl, *ovfl;
	WT_PAGE_DISK *dsk;
	WT_REPL *repl;
	uint32_t i, len, space_avail;
	uint8_t *first_free;

	db = toc->db;
	dsk = new->dsk;
	__wt_set_ff_and_sa_from_offset(
	    new, WT_PAGE_BYTE(new), &first_free, &space_avail);

	WT_CLEAR(data_dbt);
	WT_CLEAR(data_item);
	data = &data_dbt;

	WT_INDX_FOREACH(page, cip, i) {
		/*
		 * Get a reference to the data: it's either a replacement value
		 * or the original on-page item.
		 */
		item = cip->data;
		if ((repl = WT_COL_REPL(page, cip)) != NULL) {
			/*
			 * If we replaced an overflow item, we need to free the
			 * file space used by the original.
			 */
			if (WT_ITEM_TYPE(item) == WT_ITEM_DATA_OVFL) {
				ovfl = WT_ITEM_BYTE_OVFL(item);
				WT_RET(__wt_block_free(toc, ovfl->addr,
				    WT_HDR_BYTES_TO_ALLOC(db, ovfl->size)));
			}

			/*
			 * Check for deletion, else build the data's WT_ITEM
			 * chunk from the most recent replacement value.
			 */
			if (WT_REPL_DELETED_ISSET(repl)) {
				WT_CLEAR(data_item);
				WT_ITEM_SET(&data_item, WT_ITEM_DEL, 0);
				len = WT_ITEM_SPACE_REQ(0);
			} else {
				data->data = WT_REPL_DATA(repl);
				data->size = repl->size;
				WT_RET(__wt_item_build_data(
				    toc, data, &data_item, &data_ovfl, 0));
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

	new->records = page->records;
	__wt_rec_set_page_size(toc, new, first_free);

	return (0);
}

/*
 * __wt_rec_row --
 *	Reconcile a row-store leaf page.
 */
static int
__wt_rec_row(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	enum { DATA_ON_PAGE, DATA_OFF_PAGE } data_loc;
	enum { KEY_ON_PAGE, KEY_NONE } key_loc;
	DB *db;
	DBT *key, key_dbt, *data, data_dbt;
	WT_ITEM key_item, data_item, *item;
	WT_OVFL data_ovfl, *ovfl;
	WT_PAGE_DISK *dsk;
	WT_ROW *rip;
	WT_REPL *repl;
	uint32_t i, len, space_avail, type;
	uint8_t *first_free;

	db = toc->db;
	dsk = new->dsk;
	__wt_set_ff_and_sa_from_offset(
	    new, WT_PAGE_BYTE(new), &first_free, &space_avail);

	WT_CLEAR(data_dbt);
	WT_CLEAR(key_dbt);
	WT_CLEAR(data_item);
	WT_CLEAR(key_item);

	key = &key_dbt;
	data = &data_dbt;

	/*
	 * Walk the page, accumulating key/data groups (groups, because a key
	 * can reference a duplicate data set).
	 *
	 * We have to walk both the WT_ROW structures as well as the original
	 * page: the problem is keys that require processing.  When a page is
	 * read into memory from a simple database, the WT_ROW key/size pair
	 * is set to reference an on-page group of bytes in the key's WT_ITEM
	 * structure.  As Btree keys are immutable, that original WT_ITEM is
	 * usually what we want to write, and we can pretty easily find it by
	 * moving to immediately before the on-page key.
	 *
	 * Keys that require processing are harder (for example, a Huffman
	 * encoded key).  When we have to use a key that requires processing,
	 * we process the key and set the WT_ROW key/size pair to reference
	 * the allocated memory that holds the key.  At that point we've lost
	 * any reference to the original WT_ITEM structure, which is what we
	 * want to re-write when reconciling the page.  We don't want to make
	 * the WT_ROW structure bigger by another sizeof(void *) bytes, so we
	 * walk the original page at the same time we walk the WT_PAGE array
	 * when reconciling the page so we can find the original WT_ITEM.
	 */
	item = NULL;
	WT_INDX_FOREACH(page, rip, i) {
		/* Move to the next key on the original page. */
		if (item == NULL)
			item = (WT_ITEM *)WT_PAGE_BYTE(page);
		else
			do {
				item = WT_ITEM_NEXT(item);
			} while (WT_ITEM_TYPE(item) != WT_ITEM_KEY &&
			    WT_ITEM_TYPE(item) != WT_ITEM_KEY_OVFL);

		/*
		 * Get a reference to the data.  We get the data first because
		 * it may have been deleted, in which case we ignore the pair.
		 */
		if ((repl = WT_ROW_REPL(page, rip)) != NULL) {
			/*
			 * If we replaced an overflow item, we need to free the
			 * file space used by the original.
			 */
			switch (WT_ITEM_TYPE(rip->data)) {
			case WT_ITEM_DATA_OVFL:
			case WT_ITEM_DATA_DUP_OVFL:
				ovfl = WT_ITEM_BYTE_OVFL(rip->data);
				WT_RET(__wt_block_free(toc, ovfl->addr,
				    WT_HDR_BYTES_TO_ALLOC(db, ovfl->size)));
			}

			/* If this pair were deleted, we're done. */
			if (WT_REPL_DELETED_ISSET(repl))
				continue;

			/*
			 * Build the data's WT_ITEM chunk from the most recent
			 * replacement value.
			 */
			data->data = WT_REPL_DATA(repl);
			data->size = repl->size;
			WT_RET(__wt_item_build_data(
			    toc, data, &data_item, &data_ovfl, 0));
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
		if (WT_ROW_INDX_IS_DUPLICATE(page, rip)) {
			type = data_loc == DATA_ON_PAGE ?
			    WT_ITEM_TYPE(rip->data) : WT_ITEM_TYPE(&data_item);
			switch (type) {
				case WT_ITEM_DATA:
				case WT_ITEM_DATA_DUP:
					type = WT_ITEM_DATA_DUP;
					break;
				case WT_ITEM_DATA_OVFL:
				case WT_ITEM_DATA_DUP_OVFL:
					type = WT_ITEM_DATA_DUP_OVFL;
					break;
				WT_ILLEGAL_FORMAT(db);
				}
			if (data_loc == DATA_ON_PAGE)
				WT_ITEM_SET_TYPE(rip->data, type);
			else
				WT_ITEM_SET_TYPE(&data_item, type);
			key_loc = KEY_NONE;
		} else {
			/* Take the key's WT_ITEM from the original page. */
			key->data = item;
			key->size = WT_ITEM_SPACE_REQ(WT_ITEM_LEN(item));
			key_loc = KEY_ON_PAGE;
		}

		len = 0;
		switch (key_loc) {
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

		/*
		 * XXX
		 * We don't yet handle splits -- we allocated the maximum leaf
		 * page size, but it still wasn't enough.  We must allocate
		 * another leaf page and split the parent.
		 */
		if (len > space_avail) {
			fprintf(stderr, "__wt_rec_row: page %lu split\n",
			    (u_long)page->addr);
			__wt_abort(toc->env);
		}

		switch (key_loc) {
		case KEY_ON_PAGE:
			memcpy(first_free, key->data, key->size);
			first_free += key->size;
			space_avail -= key->size;
			++dsk->u.entries;
			break;
		case KEY_NONE:
			break;
		}
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
		}
	}

	new->records = page->records;
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

	/*
	 * XXX
	 * We fail if the page gets emptied -- we'll need to do some kind of
	 * reverse split where the internal page disappears.   That shouldn't
	 * be difficult, but I haven't written it yet.
	 */
	if (new->dsk->u.entries == 0) {
		new->addr = WT_ADDR_INVALID;
		WT_VERBOSE(env, WT_VERB_EVICT, (env,
		    "reconcile removing empty page %lu", (u_long)page->addr));
		fprintf(stderr, "PAGE %lu EMPTIED\n", (u_long)page->addr);
		__wt_abort(env);
	} else {
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
		WT_RET(__wt_page_write(toc, new));

		WT_VERBOSE(env, WT_VERB_EVICT,
		    (env, "reconcile move %lu to %lu, resize %lu to %lu",
		    (u_long)page->addr, (u_long)new->addr,
		    (u_long)page->size, (u_long)new->size));
	}

	/* Update the page's parent. */
	if ((ret = __wt_rec_parent_update(toc, page, new)) != 0) {
		(void)__wt_block_free(toc, new->addr, new->size);
		return (ret);
	}

	return (0);
}

/*
 * __wt_rec_parent_update --
 *	Update a parent page's reference when a page is reconciled.
 */
static int
__wt_rec_parent_update(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	IDB *idb;
	WT_OFF *parent_off;

	idb = toc->db->idb;

	/*
	 * If we're writing the root of the tree, then we have to update the
	 * descriptor record, there's no parent to update.
	 */
	if (page->addr == idb->root_off.addr) {
		  idb->root_off.addr = new->addr;
		  idb->root_off.size = new->size;
		  WT_RECORDS(&idb->root_off) = new->records;
		  return (__wt_desc_write(toc));
	 }

	/*
	 * Update the relevant WT_OFF structure.  There are two memory locations
	 * that change (address and size), and we could race, but that's not a
	 * problem.   Only a single thread ever reconciles a page at a time, and
	 * pages cannot leave memory while they have children.
	 */
	parent_off = page->parent_off;
	WT_RECORDS(parent_off) = new->records;
	parent_off->addr = new->addr;
	parent_off->size = new->size;

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
	WT_PAGE_SET_MODIFIED(page->parent);

	return (0);
}
