/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * __col_insert_search_match --
 *	Search an column-store insert list for an exact match.
 */
static inline WT_INSERT *
__col_insert_search_match(WT_INSERT_HEAD *inshead, uint64_t recno)
{
	WT_INSERT **ins;
	uint64_t ins_recno;
	int cmp, i;

	/* If there's no insert chain to search, we're done. */
	if (inshead == NULL)
		return (NULL);

	/* Fast path the check for values at the end of the skiplist. */
	if (inshead->tail[0] != NULL &&
	    recno > WT_INSERT_RECNO(inshead->tail[0]))
		return (NULL);

	/*
	 * The insert list is a skip list: start at the highest skip level, then
	 * go as far as possible at each level before stepping down to the next.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, ins = &inshead->head[i]; i >= 0; ) {
		if (*ins == NULL) {
			--i;
			--ins;
			continue;
		}

		ins_recno = WT_INSERT_RECNO(*ins);
		cmp = (recno == ins_recno) ? 0 : (recno < ins_recno) ? -1 : 1;

		if (cmp == 0)			/* Exact match: return */
			return (*ins);
		else if (cmp > 0)		/* Keep going at this level */
			ins = &(*ins)->next[i];
		else {				/* Drop down a level */
			--i;
			--ins;
		}
	}

	return (NULL);
}

/*
 * __col_insert_search --
 *	Search a column-store insert list, creating a skiplist stack as we go.
 */
static inline WT_INSERT *
__col_insert_search(
    WT_INSERT_HEAD *inshead, WT_INSERT ***ins_stack, uint64_t recno)
{
	WT_INSERT **ins, *ret_ins;
	uint64_t ins_recno;
	int cmp, i;

	/* If there's no insert chain to search, we're done. */
	if (inshead == NULL)
		return (NULL);

	/* Fast path appends. */
	if (inshead->tail[0] != NULL &&
	    recno > WT_INSERT_RECNO(inshead->tail[0])) {
		for (i = 0; i < WT_SKIP_MAXDEPTH; i++)
			ins_stack[i] = (inshead->tail[i] != NULL) ?
			    &inshead->tail[i]->next[i] : &inshead->head[i];
		return (inshead->tail[0]);
	}

	/*
	 * The insert list is a skip list: start at the highest skip level, then
	 * go as far as possible at each level before stepping down to the next.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, ins = &inshead->head[i]; i >= 0; ) {
		if (*ins == NULL) {
			ins_stack[i--] = ins--;
			continue;
		}

		ret_ins = *ins;
		ins_recno = WT_INSERT_RECNO(ret_ins);
		cmp = (recno == ins_recno) ? 0 : (recno < ins_recno) ? -1 : 1;

		if (cmp == 0)			/* Exact match: return */
			break;
		else if (cmp > 0)		/* Keep going at this level */
			ins = &(ret_ins)->next[i];
		else				/* Drop down a level */
			ins_stack[i--] = ins--;
	}
	return (ret_ins);
}

/*
 * __col_last_recno --
 *	Return the last record number for a variable-length column-store page.
 */
static inline uint64_t
__col_last_recno(WT_PAGE *page)
{
	WT_COL_RLE *repeat;

	/*
	 * If there's an append list (the last page), then there may be more
	 * records on the page.  This function ignores those records, so our
	 * callers have to handle that explicitly, if they care.
	 *
	 * WT_PAGE_COL_FIX pages don't have a repeat array, so this works for
	 * fixed-length column-stores without any further check.
	 */
	if (page->u.col_leaf.nrepeats == 0)
		return (page->entries == 0 ? 0 :
		    page->u.col_leaf.recno + (page->entries - 1));

	repeat = &page->u.col_leaf.repeats[page->u.col_leaf.nrepeats - 1];
	return (
	    (repeat->recno + repeat->rle) - 1 +
	    (page->entries - (repeat->indx + 1)));
}

/*
 * __col_var_search --
 *	Search a variable-length column-store page for a record.
 */
static inline WT_COL *
__col_var_search(WT_PAGE *page, uint64_t recno)
{
	WT_COL_RLE *repeat;
	uint64_t start_recno;
	uint32_t base, indx, limit, start_indx;

	/*
	 * Find the matching slot.
	 *
	 * This is done in two stages: first, we do a binary search among any
	 * repeating records to find largest repeating less than the search key.
	 * Once there, we can do a simple offset calculation to find the correct
	 * slot for this record number, because we know any intervening records
	 * have repeat counts of 1.
	 */
	for (base = 0,
	    limit = page->u.col_leaf.nrepeats; limit != 0; limit >>= 1) {
		indx = base + (limit >> 1);

		repeat = page->u.col_leaf.repeats + indx;
		if (recno >= repeat->recno &&
		    recno < repeat->recno + repeat->rle)
			return (page->u.col_leaf.d + repeat->indx);
		if (recno < repeat->recno)
			continue;
		base = indx + 1;
		--limit;
	}

	/*
	 * We didn't find an exact match, move forward from the largest repeat
	 * less than the search key.
	 */
	if (base == 0) {
		start_indx = 0;
		start_recno = page->u.col_leaf.recno;
	} else {
		repeat = page->u.col_leaf.repeats + (base - 1);
		start_indx = repeat->indx + 1;
		start_recno = repeat->recno + repeat->rle;
	}

	if (recno >= start_recno + (page->entries - start_indx))
		return (NULL);

	return (page->u.col_leaf.d +
	    start_indx + (uint32_t)(recno - start_recno));
}
