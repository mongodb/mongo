/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_bt_page_size_minimum(DB *, WT_PAGE *, WT_PAGE *);
static int  __wt_bt_rcc_expand_compare(const void *, const void *);
static int  __wt_bt_rcc_expand_sort(
	ENV *, WT_PAGE *, WT_COL *, WT_COL_EXPAND ***, u_int32_t *);
static int  __wt_bt_rec_col_fix(WT_TOC *, WT_PAGE *, WT_PAGE *);
static int  __wt_bt_rec_col_fix_rcc(WT_TOC *, WT_PAGE *, WT_PAGE *);
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
	IDB *idb;
	ENV *env;
	WT_PAGE *new;
	WT_PAGE_HDR *hdr;
	u_int32_t max;
	int ret;

	db = toc->db;
	idb = db->idb;
	env = toc->env;
	hdr = page->hdr;

	WT_VERBOSE(env, WT_VERB_CACHE,
	    (env, "reconcile addr %lu (page %p)", (u_long)page->addr, page));

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
		if (F_ISSET(idb, WT_REPEAT_COMP))
			WT_ERR(__wt_bt_rec_col_fix_rcc(toc, page, new));
		else
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
	 * discard the page.  Flush the change explicitly in case we're the
	 * Db.sync method, and the cache drain server needs to know the page
	 * is clean.
	 */
	WT_PAGE_MODIFY_CLR(page);
	WT_MEMORY_FLUSH;

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
 *	Reconcile a fixed-width column-store leaf page (does not handle
 *	repeat-count compression).
 */
static int
__wt_bt_rec_col_fix(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	DB *db;
	ENV *env;
	WT_COL *cip;
	WT_PAGE_HDR *hdr;
	WT_REPL *repl;
	u_int32_t i, len;
	u_int8_t *data;

	db = toc->db;
	env = toc->env;
	hdr = new->hdr;

	/*
	 * We need a "deleted" data item to store on the page.  Make sure the
	 * WT_TOC's scratch buffer is big enough (our caller is using tmp1 so
	 * we use tmp2).   Clear the buffer's contents and set the delete flag.
	 */
	len = db->fixed_len;
	if (toc->tmp2.mem_size < len)
		WT_RET(__wt_realloc(
		    env, &toc->tmp2.mem_size, len, &toc->tmp2.data));
	memset(toc->tmp2.data, 0, len);
	WT_FIX_DELETE_SET(toc->tmp2.data);

	WT_INDX_FOREACH(page, cip, i) {
		/*
		 * Get a reference to the data, on- or off- page, and see if
		 * it's been deleted.
		 */
		if ((repl = WT_COL_REPL(page, cip)) != NULL) {
			if (WT_REPL_DELETED_ISSET(repl))
				data = toc->tmp2.data;	/* Replaced deleted */
			else				/* Replaced data */
				data = WT_REPL_DATA(repl);
		} else if (WT_FIX_DELETE_ISSET(cip->data))
			data = toc->tmp2.data;		/* On-page deleted */
		else
			data = cip->data;		/* On-page data */

		if (len > new->space_avail) {
			fprintf(stderr, "PAGE %lu SPLIT\n", (u_long)page->addr);
			__wt_abort(toc->env);
		}

		memcpy(new->first_free, data, len);
		new->first_free += len;
		new->space_avail -= len;

		++hdr->u.entries;
	}

	__wt_bt_page_size_minimum(db, page, new);

	WT_ASSERT(toc->env, __wt_bt_verify_page(toc, new, NULL) == 0);

	return (__wt_page_write(db, new));
}

/*
 * __wt_bt_rec_col_fix_rcc --
 *	Reconcile a repeat-count compressed, fixed-width column-store leaf page.
 */
static int
__wt_bt_rec_col_fix_rcc(WT_TOC *toc, WT_PAGE *page, WT_PAGE *new)
{
	DB *db;
	ENV *env;
	WT_COL *cip;
	WT_COL_EXPAND *exp, **expsort, **expp;
	WT_PAGE_HDR *hdr;
	WT_REPL *repl;
	u_int32_t i, len, n_expsort;
	u_int16_t n, repeat_count, total;
	u_int8_t *data, *last_data;
	int rc_prefix;

	db = toc->db;
	env = toc->env;
	expsort = NULL;
	hdr = new->hdr;
	n_expsort = 0;			/* Necessary for the sort function */
	last_data = NULL;

	/*
	 * We need a "deleted" data item to store on the page.  Make sure the
	 * WT_TOC's scratch buffer is big enough (our caller is using tmp1 so
	 * we use tmp2).   Clear the buffer's contents and set the delete flag.
	 */
	len = db->fixed_len + sizeof(u_int16_t);
	if (toc->tmp2.mem_size < len)
		WT_RET(__wt_realloc(
		    env, &toc->tmp2.mem_size, len, &toc->tmp2.data));
	memset(toc->tmp2.data, 0, len);
	WT_FIX_REPEAT_COUNT(toc->tmp2.data) = 1;
	WT_FIX_DELETE_SET(WT_FIX_REPEAT_DATA(toc->tmp2.data));

	WT_INDX_FOREACH(page, cip, i) {
		/*
		 * Get a sorted list of any expansion entries we've created for
		 * this set of records.  The sort function returns a NULL-
		 * terminated array of references to WT_COL_EXPAND structures,
		 * sorted by record offset.
		 */
		WT_RET(__wt_bt_rcc_expand_sort(
		    env, page, cip, &expsort, &n_expsort));

		/*
		 * Generate entries on the page: using the WT_REPL entry for
		 * records listed in the WT_COL_EXPAND array, and original data
		 * otherwise.
		 */
		total = WT_FIX_REPEAT_COUNT(cip->data);
		for (expp = expsort, n = 1; n <= total; n += repeat_count) {
			rc_prefix = 0;
			if ((exp = *expp) != NULL && n == exp->rcc_offset) {
				++expp;

				repl = exp->repl;
				if (WT_REPL_DELETED_ISSET(repl))
					data = toc->tmp2.data;
				else {
					rc_prefix = 1;
					data = WT_REPL_DATA(repl);
				}
				repeat_count = 1;
			} else {
				if (WT_FIX_DELETE_ISSET(cip->data))
					data = toc->tmp2.data;
				else
					data = cip->data;
				/*
				 * The repeat count is the number of records
				 * up to the next WT_COL_EXPAND record, or
				 * up to the end of this entry if we have no
				 * more WT_COL_EXPAND records.
				 */
				if (exp == NULL)
					repeat_count = (total - n) + 1;
				else
					repeat_count = exp->rcc_offset - n;
			}

			/*
			 * In all cases, check the last entry written on the
			 * page to see if it's identical, and increment its
			 * repeat count where possible.
			 */
			if (last_data != NULL &&
			    memcmp(WT_FIX_REPEAT_DATA(last_data),
			    WT_FIX_REPEAT_DATA(data), db->fixed_len) == 0 &&
			    WT_FIX_REPEAT_COUNT(last_data) < UINT16_MAX) {
				WT_FIX_REPEAT_COUNT(last_data) += repeat_count;
				continue;
			}

			if (len > new->space_avail) {
				fprintf(stderr,
				    "PAGE %lu SPLIT\n", (u_long)page->addr);
				__wt_abort(toc->env);
			}

			/*
			 * Most of the formats already include a repeat count:
			 * specifically the deleted buffer, or any entry we're
			 * copying from the original page.   However, entries
			 * that were deleted or replaced are read from a WT_REPL
			 * structure, which has no repeat count.
			 */
			last_data = new->first_free;
			if (rc_prefix) {
				WT_FIX_REPEAT_COUNT(last_data) = repeat_count;
				memcpy(WT_FIX_REPEAT_DATA(
				    last_data), data, db->fixed_len);
			} else
				memcpy(last_data, data, len);
			new->first_free += len;
			new->space_avail -= len;

			++hdr->u.entries;
		}
	}

	__wt_bt_page_size_minimum(db, page, new);

	/* Free the sort array. */
	if (expsort != NULL)
		__wt_free(env, expsort, n_expsort * sizeof(WT_COL_EXPAND *));

	WT_ASSERT(toc->env, __wt_bt_verify_page(toc, new, NULL) == 0);

	return (__wt_page_write(db, new));
}

/*
 * __wt_bt_rcc_expand_compare --
 *	Qsort function: sort WT_COL_EXPAND structures based on the record
 *	offset, in ascending order.
 */
static int
__wt_bt_rcc_expand_compare(const void *a, const void *b)
{
	WT_COL_EXPAND *a_exp, *b_exp;

	a_exp = *(WT_COL_EXPAND **)a;
	b_exp = *(WT_COL_EXPAND **)b;

	return (a_exp->rcc_offset > b_exp->rcc_offset ? 1 : 0);
}

/*
 * __wt_bt_rcc_expand_sort --
 *	Return the current on-page index's array of WT_COL_EXPAND structures,
 *	sorted by record offset.
 */
static int
__wt_bt_rcc_expand_sort(ENV *env,
    WT_PAGE *page, WT_COL *cip, WT_COL_EXPAND ***expsortp, u_int32_t *np)
{
	WT_COL_EXPAND *exp;
	u_int16_t n;

	/* Figure out how big the array needs to be. */
	for (n = 0,
	    exp = WT_COL_EXPCOL(page, cip); exp != NULL; exp = exp->next, ++n)
		;

	/*
	 * Allocate that big an array -- always allocate at least one slot,
	 * our caller expects NULL-termination.
	 */
	if (n >= *np) {
		if (*expsortp != NULL)
			__wt_free(
			    env, *expsortp, *np * sizeof(WT_COL_EXPAND *));
		WT_RET(__wt_calloc(
		    env, n + 10, sizeof(WT_COL_EXPAND *), expsortp));
		*np = n + 10;
	}

	/* Enter the WT_COL_EXPAND structures into the array. */
	for (n = 0,
	    exp = WT_COL_EXPCOL(page, cip); exp != NULL; exp = exp->next, ++n)
		(*expsortp)[n] = exp;

	/* Sort the entries. */
	if (n != 0)
		qsort(*expsortp, (size_t)n,
		    sizeof(WT_COL_EXPAND *), __wt_bt_rcc_expand_compare);

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
	DB *db;
	DBT *data, data_dbt;
	WT_COL *cip;
	WT_ITEM data_item;
	WT_OVFL data_ovfl;
	WT_PAGE_HDR *hdr;
	WT_REPL *repl;
	u_int32_t i, len;

	db = toc->db;
	hdr = new->hdr;

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
				WT_ITEM_TYPE_SET(&data_item, WT_ITEM_DEL);
				WT_ITEM_LEN_SET(&data_item, 0);
				len = WT_ITEM_SPACE_REQ(0);
			} else {
				data->data = WT_REPL_DATA(repl);
				data->size = repl->size;
				WT_RET(__wt_bt_build_data_item(
				    toc, data, &data_item, &data_ovfl));
				len = WT_ITEM_SPACE_REQ(data->size);
			}
			data_loc = DATA_OFF_PAGE;
		} else {
			data->data = cip->data;
			data->size = WT_ITEM_SPACE_REQ(WT_ITEM_LEN(cip->data));
			len = data->size;
			data_loc = DATA_ON_PAGE;
		}

		if (len > new->space_avail) {
			fprintf(stderr, "PAGE %lu SPLIT\n", (u_long)page->addr);
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
		++hdr->u.entries;
	}

	__wt_bt_page_size_minimum(db, page, new);

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
	enum { KEY_ON_PAGE, KEY_NONE } key_loc;
	DB *db;
	DBT *key, key_dbt, *data, data_dbt;
	WT_ITEM key_item, data_item, *item;
	WT_OVFL data_ovfl;
	WT_PAGE_HDR *hdr;
	WT_ROW *rip;
	WT_REPL *repl;
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
		if (len > new->space_avail) {
			fprintf(stderr, "PAGE %lu SPLIT\n", (u_long)page->addr);
			__wt_abort(toc->env);
		}

		switch (key_loc) {
		case KEY_ON_PAGE:
			memcpy(new->first_free, key->data, key->size);
			new->first_free += key->size;
			new->space_avail -= key->size;
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
			    sizeof(WT_ITEM), data->data, data->size);
			new->first_free += WT_ITEM_SPACE_REQ(data->size);
			new->space_avail -= WT_ITEM_SPACE_REQ(data->size);
			++hdr->u.entries;
			break;
		}
	}

	__wt_bt_page_size_minimum(db, page, new);

	WT_ASSERT(toc->env, __wt_bt_verify_page(toc, new, NULL) == 0);

	return (__wt_page_write(db, new));
}

/*
 * __wt_bt_page_size_minimum --
 *	Reset a reconciled page's size.
 */
static void
__wt_bt_page_size_minimum(DB *db, WT_PAGE *old, WT_PAGE *new)
{
	/*
	 * Reset the page's size to the minimum required, and if the resulting
	 * page size is larger or smaller than the original page, we allocate
	 * a new page.  The difference is that if the page is smaller, we don't
	 * extend the file to get a replacement page, we just waste the space.
	 *
	 * XXX
	 * That's the plan -- none of it is implemented.  For the time being,
	 * we simply reset the new size down to the old size.
	 */
	new->size = WT_MAX(old->size,
	    (new->size + (db->allocsize - 1)) % db->allocsize);

	if (new->size > old->size) {
		fprintf(stderr, "PAGE GREW: %lu\n", (u_long)new->addr);
		__wt_abort(db->env);
	}
}
