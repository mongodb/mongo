/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_rcc_expand_compare(const void *, const void *);
static int __wt_bt_rec_col_fix(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_bt_rec_col_int(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_bt_rec_col_rcc(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_bt_rec_col_var(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_bt_rec_page_write(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_bt_rec_parent_update(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_bt_rec_row(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int __wt_bt_rec_row_int(WT_TOC *, WT_PAGE *, WT_PAGE *);
static inline void __wt_bt_rec_set_size(WT_TOC *, WT_PAGE *, uint8_t *);

/*
 * __wt_bt_rec_set_size --
 *	Set the page's size to the minimum number of allocation units.
 */
static inline void
__wt_bt_rec_set_size(WT_TOC *toc, WT_PAGE *page, uint8_t *first_free)
{
	DB *db;

	db = toc->db;

	/*
	 * Set the page's size to the minimum number of allocation units needed
	 * (note the page size can either grow or shrink).
	 *
	 * Set the page size before verifying the page, the verification code
	 * checks for entries that extend past the end of the page, and expects
	 * the WT_PAGE->size field to be valid.
	 */
	page->size = WT_ALIGN(first_free - (uint8_t *)page->hdr, db->allocsize);
}

/*
 * __wt_bt_rec_page --
 *	Move a page from its in-memory state out to disk.
 */
int
__wt_bt_rec_page(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	DBT *tmp;
	ENV *env;
	WT_PAGE *new;
	WT_PAGE_HDR *hdr;
	uint32_t max;
	int ret;

	db = toc->db;
	tmp = NULL;
	env = toc->env;
	hdr = page->hdr;

	WT_VERBOSE(env, WT_VERB_CACHE,
	    (env, "reconcile addr %lu (page %p, type %s)",
	    (u_long)page->addr, page, __wt_bt_hdr_type(hdr)));

	/* If the page isn't dirty, we should never have been called. */
	WT_ASSERT(env, WT_PAGE_MODIFY_ISSET(page));

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
		/*
		 * The database description page doesn't move, simply write it
		 * into place.  This assumes (1) the description page is only a
		 * single disk sector, and writing a single disk sector is known
		 * to be atomic, that is, it either succeeds or fails, there is
		 * no possibility of a corrupted write.
		 */
		ret = __wt_page_write(db, page);
		goto done;
	case WT_PAGE_COL_FIX:
		/*
		 * Fixed-width pages without repeat count compression cannot
		 * change size.
		 */
		max = page->size;
		break;
	case WT_PAGE_COL_RCC:
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

	/* Make sure the TOC's scratch buffer is big enough. */
	WT_ERR(__wt_scr_alloc(toc, sizeof(WT_PAGE) + max, &tmp));
	memset(tmp->data, 0, sizeof(WT_PAGE) + max);

	/* Initialize the reconciliation buffer as a replacement page. */
	new = tmp->data;
	new->addr = page->addr;
	new->size = max;
	new->hdr = (WT_PAGE_HDR *)((uint8_t *)tmp->data + sizeof(WT_PAGE));
	new->hdr->start_recno = page->hdr->start_recno;
	new->hdr->type = page->hdr->type;
	new->hdr->level = page->hdr->level;

	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
		WT_ERR(__wt_bt_rec_col_fix(toc, page, new));
		break;
	case WT_PAGE_COL_RCC:
		WT_ERR(__wt_bt_rec_col_rcc(toc, page, new));
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

	WT_ERR(__wt_bt_rec_page_write(toc, page, new));

	/*
	 * The original page is no longer referenced, and can be free'd, even
	 * though the in-memory version continues to be in-use.  Update the
	 * page's address and size.   If we're running in diagnostic mode, kill
	 * the page on disk so we catch any future reader doing something bad.
	 */
#ifdef HAVE_DIAGNOSTIC
	hdr->type = WT_PAGE_FREE;
	WT_ERR(__wt_page_write(db, page));
#endif
	WT_ERR(__wt_bt_table_free(toc, page->addr, page->size));
	page->addr = new->addr;
	page->size = new->size;

done:	/*
	 * Clear the modification flag.  This doesn't sound safe, because the
	 * cache thread might decide to discard this page as "clean".  That's
	 * not correct, because we're either the Db.sync method and holding a
	 * hazard reference and we finish our write before releasing our hazard
	 * reference, or we're the cache drain server and no other thread can
	 * discard the page.  Flush the change explicitly in case we're the
	 * Db.sync method, and the cache drain server needs to know the page
	 * is clean.
	 */
	WT_PAGE_MODIFY_CLR(page);
	WT_MEMORY_FLUSH;

err:	F_CLR(toc, WT_READ_DRAIN | WT_READ_PRIORITY);

	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_bt_rec_col_int --
 *	Reconcile a column store internal page.
 */
static int
__wt_bt_rec_col_int(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	WT_COL *cip;
	WT_OFF *from;
	WT_PAGE_HDR *hdr;
	WT_REPL *repl;
	uint32_t i, space_avail;
	uint8_t *first_free;

	hdr = new->hdr;
	__wt_bt_set_ff_and_sa_from_offset(
	    new, WT_PAGE_BYTE(new), &first_free, &space_avail);

	WT_INDX_FOREACH(page, cip, i) {
		if ((repl = WT_COL_REPL(page, cip)) != NULL)
			from = WT_REPL_DATA(repl);
		else
			from = cip->data;

		/*
		 * XXX
		 * We don't yet handle splits:  we allocated the maximum page
		 * size, but it still wasn't enough.  We must allocate another
		 * page and split the parent.
		 */
		if (sizeof(WT_OFF) > space_avail) {
			fprintf(stderr,
			   "__wt_bt_rec_col_int: page %lu split\n",
			   (u_long)page->addr);
			__wt_abort(toc->env);
		}

		memcpy(first_free, from, sizeof(WT_OFF));
		first_free += sizeof(WT_OFF);
		space_avail -= sizeof(WT_OFF);
		++hdr->u.entries;
	}

	__wt_bt_rec_set_size(toc, new, first_free);
	new->records = page->records;

	return (0);
}

/*
 * __wt_bt_rec_row_int --
 *	Reconcile a row store, or off-page duplicate tree, internal page.
 */
static int
__wt_bt_rec_row_int(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	WT_ITEM *key_item, *data_item, *next;
	WT_PAGE_HDR *hdr;
	WT_REPL *repl;
	WT_ROW *rip;
	uint32_t i, len, space_avail;
	uint8_t *first_free;

	hdr = new->hdr;
	__wt_bt_set_ff_and_sa_from_offset(
	    new, WT_PAGE_BYTE(new), &first_free, &space_avail);

	/*
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
	key_item = WT_PAGE_BYTE(page);
	WT_INDX_FOREACH(page, rip, i) {
		/*
		 * Copy the paired items off the old page into the new page; if
		 * the page has been replaced, update its information.
		 *
		 * XXX
		 * Internal pages can't grow, yet, so we could more easily just
		 * update the old page.   We do the copy because eventually we
		 * will have to split the internal pages, and they'll be able to
		 * grow.
		 */
		data_item = WT_ITEM_NEXT(key_item);
		if ((repl = WT_ROW_REPL(page, rip)) != NULL)
			memcpy(WT_ITEM_BYTE(data_item),
			    WT_REPL_DATA(repl), sizeof(WT_OFF));
		next = WT_ITEM_NEXT(data_item);
		len = (uint32_t)((uint8_t *)next - (uint8_t *)key_item);

		/*
		 * XXX
		 * We don't yet handle splits:  we allocated the maximum page
		 * size, but it still wasn't enough.  We must allocate another
		 * page and split the parent.
		 */
		if (len > space_avail) {
			fprintf(stderr,
			    "__wt_bt_rec_row_int: page %lu split\n",
			    (u_long)page->addr);
			__wt_abort(toc->env);
		}

		memcpy(first_free, key_item, len);
		first_free += len;
		space_avail -= len;
		++hdr->u.entries;

		key_item = next;
	}

	__wt_bt_rec_set_size(toc, new, first_free);
	new->records = page->records;

	return (0);
}

/*
 * __wt_bt_rec_col_fix --
 *	Reconcile a fixed-width column-store leaf page (does not handle
 *	repeat-count compression).
 */
static int
__wt_bt_rec_col_fix(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	DB *db;
	DBT *tmp;
	ENV *env;
	WT_COL *cip;
	WT_PAGE_HDR *hdr;
	WT_REPL *repl;
	uint32_t i, len, space_avail;
	uint8_t *data, *first_free;
	int ret;

	db = toc->db;
	tmp = NULL;
	env = toc->env;
	hdr = new->hdr;
	ret = 0;

	__wt_bt_set_ff_and_sa_from_offset(
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
		 * repeat count compression, the on-page information cannot
		 * change size -- there's no reason to ever split such a page.
		 */
		WT_ASSERT(env, len <= space_avail);

		memcpy(first_free, data, len);
		first_free += len;
		space_avail -= len;
		++hdr->u.entries;
	}

	__wt_bt_rec_set_size(toc, new, first_free);
	new->records = page->records;

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __wt_bt_rec_col_rcc --
 *	Reconcile a repeat-count compressed, fixed-width column-store leaf page.
 */
static int
__wt_bt_rec_col_rcc(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	DB *db;
	DBT *tmp;
	ENV *env;
	WT_COL *cip;
	WT_PAGE_HDR *hdr;
	WT_RCC_EXPAND *exp, **expsort, **expp;
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
	hdr = new->hdr;
	n_expsort = 0;			/* Necessary for the sort function */
	last_data = NULL;
	ret = 0;

	__wt_bt_set_ff_and_sa_from_offset(
	    new, WT_PAGE_BYTE(new), &first_free, &space_avail);

	/*
	 * We need a "deleted" data item to store on the page.  Make sure the
	 * WT_TOC's scratch buffer is big enough.  Clear the buffer's contents
	 * and set the delete flag.
	 */
	len = db->fixed_len + sizeof(uint16_t);
	WT_ERR(__wt_scr_alloc(toc, len, &tmp));
	memset(tmp->data, 0, len);
	WT_RCC_REPEAT_COUNT(tmp->data) = 1;
	WT_FIX_DELETE_SET(WT_RCC_REPEAT_DATA(tmp->data));

	/* Set recno to the first record on the page. */
	recno = page->hdr->start_recno;
	WT_INDX_FOREACH(page, cip, i) {
		/*
		 * Get a sorted list of any expansion entries we've created for
		 * this set of records.  The sort function returns a NULL-
		 * terminated array of references to WT_RCC_EXPAND structures,
		 * sorted by record number.
		 */
		WT_ERR(__wt_bt_rcc_expand_sort(
		    env, page, cip, &expsort, &n_expsort));

		/*
		 *
		 * Generate entries for the new page: loop through the repeat
		 * records, checking for WT_RCC_EXPAND entries that match the
		 * current record number.
		 */
		nrepeat = WT_RCC_REPEAT_COUNT(cip->data);
		for (expp = expsort, n = 1;
		    n <= nrepeat; n += repeat_count, recno += repeat_count) {
			from_repl = 0;
			if ((exp = *expp) != NULL && recno == exp->recno) {
				++expp;

				/* Use the WT_RCC_EXPAND's WT_REPL field. */
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
				 * up to the next WT_RCC_EXPAND record, or
				 * up to the end of this entry if we have no
				 * more WT_RCC_EXPAND records.
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
			    memcmp(WT_RCC_REPEAT_DATA(last_data),
			    WT_RCC_REPEAT_DATA(data), db->fixed_len) == 0 &&
			    WT_RCC_REPEAT_COUNT(last_data) < UINT16_MAX) {
				WT_RCC_REPEAT_COUNT(last_data) += repeat_count;
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
				    "__wt_bt_rec_col_rcc: page %lu split\n",
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
				WT_RCC_REPEAT_COUNT(last_data) = repeat_count;
				memcpy(WT_RCC_REPEAT_DATA(
				    last_data), data, db->fixed_len);
			} else
				memcpy(last_data, data, len);
			first_free += len;
			space_avail -= len;
			++hdr->u.entries;
		}
	}

	__wt_bt_rec_set_size(toc, new, first_free);
	new->records = page->records;

	/* Free the sort array. */
err:	if (expsort != NULL)
		__wt_free(env, expsort, n_expsort * sizeof(WT_RCC_EXPAND *));

	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_bt_rcc_expand_compare --
 *	Qsort function: sort WT_RCC_EXPAND structures based on the record
 *	offset, in ascending order.
 */
static int
__wt_bt_rcc_expand_compare(const void *a, const void *b)
{
	WT_RCC_EXPAND *a_exp, *b_exp;

	a_exp = *(WT_RCC_EXPAND **)a;
	b_exp = *(WT_RCC_EXPAND **)b;

	return (a_exp->recno > b_exp->recno ? 1 : 0);
}

/*
 * __wt_bt_rcc_expand_sort --
 *	Return the current on-page index's array of WT_RCC_EXPAND structures,
 *	sorted by record offset.
 */
int
__wt_bt_rcc_expand_sort(ENV *env,
    WT_PAGE *page, WT_COL *cip, WT_RCC_EXPAND ***expsortp, uint32_t *np)
{
	WT_RCC_EXPAND *exp;
	uint16_t n;

	/* Figure out how big the array needs to be. */
	for (n = 0,
	    exp = WT_COL_RCCEXP(page, cip); exp != NULL; exp = exp->next, ++n)
		;

	/*
	 * Allocate that big an array -- always allocate at least one slot,
	 * our caller expects NULL-termination.
	 */
	if (n >= *np) {
		if (*expsortp != NULL)
			__wt_free(
			    env, *expsortp, *np * sizeof(WT_RCC_EXPAND *));
		WT_RET(__wt_calloc(
		    env, n + 10, sizeof(WT_RCC_EXPAND *), expsortp));
		*np = n + 10;
	}

	/* Enter the WT_RCC_EXPAND structures into the array. */
	for (n = 0,
	    exp = WT_COL_RCCEXP(page, cip); exp != NULL; exp = exp->next, ++n)
		(*expsortp)[n] = exp;

	/* Sort the entries. */
	if (n != 0)
		qsort(*expsortp, (size_t)n,
		    sizeof(WT_RCC_EXPAND *), __wt_bt_rcc_expand_compare);

	/* NULL-terminate the array. */
	(*expsortp)[n] = NULL;

	return (0);
}

/*
 * __wt_bt_rec_col_var --
 *	Reconcile a variable-width column-store leaf page.
 */
static int
__wt_bt_rec_col_var(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	enum { DATA_ON_PAGE, DATA_OFF_PAGE } data_loc;
	DBT *data, data_dbt;
	WT_COL *cip;
	WT_ITEM data_item;
	WT_OVFL data_ovfl;
	WT_PAGE_HDR *hdr;
	WT_REPL *repl;
	uint32_t i, len, space_avail;
	uint8_t *first_free;

	hdr = new->hdr;
	__wt_bt_set_ff_and_sa_from_offset(
	    new, WT_PAGE_BYTE(new), &first_free, &space_avail);

	WT_CLEAR(data_dbt);
	WT_CLEAR(data_item);
	data = &data_dbt;

	WT_INDX_FOREACH(page, cip, i) {
		/*
		 * Get a reference to the data: it's either a replacement value
		 * or the original on-page item.
		 */
		if ((repl = WT_COL_REPL(page, cip)) != NULL) {
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
				WT_RET(__wt_bt_build_data_item(
				    toc, data, &data_item, &data_ovfl, 0));
				len = WT_ITEM_SPACE_REQ(data->size);
			}
			data_loc = DATA_OFF_PAGE;
		} else {
			data->data = cip->data;
			data->size = WT_ITEM_SPACE_REQ(WT_ITEM_LEN(cip->data));
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
			    "__wt_bt_rec_col_var: page %lu split\n",
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
		++hdr->u.entries;
	}

	__wt_bt_rec_set_size(toc, new, first_free);
	new->records = page->records;

	return (0);
}

/*
 * __wt_bt_rec_row --
 *	Reconcile a row-store leaf page.
 */
static int
__wt_bt_rec_row(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	enum { DATA_ON_PAGE, DATA_OFF_PAGE } data_loc;
	enum { KEY_ON_PAGE, KEY_NONE } key_loc;
	DB *db;
	DBT *key, key_dbt, *data, data_dbt;
	WT_ITEM key_item, data_item, *item;
	WT_OVFL data_ovfl;
	WT_PAGE_HDR *hdr;
	WT_ROW *rip;
	WT_REPL *repl;
	uint32_t i, len, space_avail, type;
	uint8_t *first_free;

	db = toc->db;
	hdr = new->hdr;
	__wt_bt_set_ff_and_sa_from_offset(
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
			if (WT_REPL_DELETED_ISSET(repl))
				continue;

			/*
			 * Build the data's WT_ITEM chunk from the most recent
			 * replacement value.
			 */
			data->data = WT_REPL_DATA(repl);
			data->size = repl->size;
			WT_RET(__wt_bt_build_data_item(
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
		if (i > 0 && rip->key == (rip - 1)->key) {
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
			fprintf(stderr, "__wt_bt_rec_row: page %lu split\n",
			    (u_long)page->addr);
			__wt_abort(toc->env);
		}

		switch (key_loc) {
		case KEY_ON_PAGE:
			memcpy(first_free, key->data, key->size);
			first_free += key->size;
			space_avail -= key->size;
			++hdr->u.entries;
			break;
		case KEY_NONE:
			break;
		}
		switch (data_loc) {
		case DATA_ON_PAGE:
			memcpy(first_free, data->data, data->size);
			first_free += data->size;
			space_avail -= data->size;
			++hdr->u.entries;
			break;
		case DATA_OFF_PAGE:
			memcpy(first_free, &data_item, sizeof(data_item));
			memcpy(first_free +
			    sizeof(WT_ITEM), data->data, data->size);
			first_free += WT_ITEM_SPACE_REQ(data->size);
			space_avail -= WT_ITEM_SPACE_REQ(data->size);
			++hdr->u.entries;
			break;
		}
	}

	__wt_bt_rec_set_size(toc, new, first_free);

	return (0);
}

/*
 * __wt_bt_rec_page_write --
 *	Write a newly reconciled page.
 */
static int
__wt_bt_rec_page_write(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	DB *db;
	ENV *env;
	int ret;

	db = toc->db;
	env = toc->env;

	if (new->hdr->u.entries == 0) {
		new->addr = WT_ADDR_INVALID;
		WT_VERBOSE(env, WT_VERB_CACHE, (env,
		    "reconcile removing empty page %lu", (u_long)page->addr));
		fprintf(stderr, "PAGE %lu EMPTIED\n", (u_long)page->addr);
		__wt_abort(env);
	} else {
		/* Verify the page to catch reconciliation errors early on. */
		WT_ASSERT(env, __wt_bt_verify_page(toc, new, NULL) == 0);

		/*
		 * Allocate file space for the page.
		 *
		 * The cache drain server is the only thread allocating space
		 * from the file, so there's no need to do any serialization.
		 *
		 * Don't overwrite pages -- if a write only partially succeeds
		 * in Berkeley DB, you're forced into catastrophic recovery.
		 * I never saw Berkeley DB fail in the field with a torn write,
		 * but that may be because Berkeley DB's maximum page size is
		 * only 64KB.  WiredTiger supports much larger page sizes, and
		 * is far more likely to see torn write failures.
		 *
		 * XXX
		 * We do not relocate the page in the cache after its address
		 * changes, which means any thread trying to get to the page
		 * we're draining will have to do a real read, which will be
		 * satisfied in the OS buffer cache, in all likelihood.  But,
		 * if Db.sync is called to flush the file while the file is
		 * being read, or if trickle-write functionality is added in
		 * the future it's going to hurt.   The alternative would be
		 * to copy the reconciled page to cache memory and relocate it
		 * in the cache for those methods.  There's little reason to
		 * relocate the reconciled page for the cache drain code --
		 * this page was LRU-selected because no thread wanted it.
		 */
		WT_RET(__wt_bt_table_alloc(toc, &new->addr, new->size));

		/*
		 * Write the page.
		 *
		 * XXX
		 * We write the page before we update the parent's reference as
		 * a reader can immediately search the parent page after it's
		 * updated, and the newly reconciled page must exist to satisfy
		 * those reads.
		 *
		 */
		WT_RET(__wt_page_write(db, new));

		WT_VERBOSE(env, WT_VERB_CACHE,
		    (env, "reconcile move %lu to %lu, resize %lu to %lu",
		    (u_long)page->addr, (u_long)new->addr,
		    (u_long)page->size, (u_long)new->size));
	}

	/* Update the page's parent. */
	if ((ret = __wt_bt_rec_parent_update(toc, page, new)) != 0) {
		(void)__wt_bt_table_free(toc, new->addr, new->size);
		return (ret);
	}

	return (0);
}

/*
 * __wt_bt_rec_parent_update --
 *	Return a parent page and WT_ROW reference for the first key on the
 *	argument page.
 */
static int
__wt_bt_rec_parent_update(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	DB *db;
	DBT *key, *key_scratch, off_dbt, tmp;
	ENV *env;
	IDB *idb;
	WT_ITEM *item;
	WT_OFF off;
	WT_PAGE *ovfl_page, *parent;
	WT_PAGE_HDR *hdr;
	WT_REPL **new_repl, *repl;
	int ret, slot;

	db = toc->db;
	key_scratch = NULL;
	env = toc->env;
	idb = db->idb;
	ovfl_page = parent = NULL;
	hdr = page->hdr;
	new_repl = NULL;
	repl = NULL;
	ret = 0;

	/*
	 * If we're writing the root of the tree, then we have to update the
	 * descriptor record, there's no parent to update.
	 */
	if (page->addr == idb->root_addr) {
		  idb->root_addr = new->addr;
		  idb->root_size = new->size;
		  return (__wt_bt_desc_write(toc));
	 }

	/* Get the parent page. */
	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RCC:
	case WT_PAGE_COL_VAR:
		WT_RET(__wt_bt_search_col(
		    toc, hdr->start_recno, hdr->level + 1, 0));
		parent = toc->srch_page;
		slot = WT_COL_SLOT(parent, toc->srch_ip);
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
		/*
		 * XXX
		 * We don't have a key for offpage-duplicate trees, which kind
		 * of screws this whole approach.  I'm going to wait for Michael
		 * to be on board, because he's really good at solving horrible
		 * problems like this one.
		 */
		WT_ASSERT(env,
		    hdr->type != WT_PAGE_DUP_INT &&
		    hdr->type != WT_PAGE_DUP_LEAF);
		/* FALLTHROUGH */
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		/*
		 * We need to search for this page, and that means setting up a
		 * DBT to point to a key for which a search will descend to this
		 * page.  We use the original on-disk page because we know it
		 * had a valid key on it when we read it from disk, and the new,
		 * rewritten page might be empty.
		 *
		 * Get a reference to the first item on the page (it better be a
		 * key), process it as necessary, and then search for it.
		 */
		item = (WT_ITEM *)WT_PAGE_BYTE(page);
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_DUP:
			if ((hdr->type == WT_PAGE_DUP_INT ?
			    idb->huffman_data : idb->huffman_key) == NULL) {
				WT_CLEAR(tmp);
				tmp.data = WT_ITEM_BYTE(item);
				tmp.size = WT_ITEM_LEN(item);
				WT_ERR(__wt_bt_search_row(
				    toc, &tmp, hdr->level + 1, 0));
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_KEY_DUP_OVFL:
			WT_ERR(__wt_scr_alloc(toc, 0, &key_scratch));
			WT_ERR(__wt_bt_item_process(
			    toc, item, &ovfl_page, key_scratch));
			if (ovfl_page != NULL) {
				WT_CLEAR(tmp);
				tmp.data = WT_PAGE_BYTE(ovfl_page);
				tmp.size = ovfl_page->hdr->u.datalen;
				key = &tmp;
			} else
				key = key_scratch;
			WT_ERR(__wt_bt_search_row(toc, key, hdr->level + 1, 0));
			break;
		WT_ILLEGAL_FORMAT_ERR(db, ret);
		}
		parent = toc->srch_page;
		slot = WT_ROW_SLOT(parent, toc->srch_ip);
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * We have the parent page.   We can't just update the WT_OFF structure
	 * because there are two memory locations that can change (address and
	 * size), and we could race.  We already have functions that take a DBT
	 * and move it into a WT_REPL structure for an item -- use them.
	 *
	 * Create and initialize the WT_OFF structure.
	 */
	WT_RECORDS(&off) = new->records;
	off.addr = new->addr;
	off.size = new->size;

	/* Set up a DBT to point to the WT_OFF structure. */
	WT_CLEAR(off_dbt);
	off_dbt.data = &off;
	off_dbt.size = sizeof(WT_OFF);

	/* Allocate the parent's page replacement array as necessary. */
	if (parent->ur.repl == NULL)
		WT_ERR(__wt_calloc(
		    env, parent->indx_count, sizeof(WT_REPL *), &new_repl));

	/* Allocate room for the new data item from per-thread memory. */
	WT_ERR(__wt_bt_repl_alloc(toc, &repl, &off_dbt));

	/* Schedule the workQ to insert the WT_REPL structure. */
	__wt_bt_item_update_serial(toc,
	    parent, toc->srch_write_gen, slot, new_repl, repl, ret);

	if (ret != 0) {
err:           if (repl != NULL)
			__wt_bt_repl_free(toc, repl);
	}

	/* Free any replacement array unless the workQ used it. */
	if (new_repl != NULL && new_repl != parent->ur.repl)
		__wt_free(
		    env, new_repl, parent->indx_count * sizeof(WT_REPL *));

	if (parent != NULL && parent != idb->root_page)
		__wt_bt_page_out(toc, &parent, 0);
	if (ovfl_page != NULL)
		__wt_bt_page_out(toc, &ovfl_page, 0);
	if (key_scratch != NULL)
		__wt_scr_release(&key_scratch);

	return (ret);
}
