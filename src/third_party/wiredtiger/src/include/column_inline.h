/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __col_insert_search_gt --
 *     Search a column-store insert list for the next larger record.
 */
static inline WT_INSERT *
__col_insert_search_gt(WT_INSERT_HEAD *ins_head, uint64_t recno)
{
    WT_INSERT *ins, **insp, *ret_ins;
    int i;

    /* If there's no insert chain to search, we're done. */
    if ((ins = WT_SKIP_LAST(ins_head)) == NULL)
        return (NULL);

    /* Fast path check for targets past the end of the skiplist. */
    if (recno >= WT_INSERT_RECNO(ins))
        return (NULL);

    /*
     * The insert list is a skip list: start at the highest skip level, then go as far as possible
     * at each level before stepping down to the next.
     */
    ret_ins = NULL;
    for (i = WT_SKIP_MAXDEPTH - 1, insp = &ins_head->head[i]; i >= 0;) {
        /*
         * Use a local variable to access the insert because the skip list can change across
         * references.
         */
        WT_ORDERED_READ(ins, *insp);
        if (ins != NULL && recno >= WT_INSERT_RECNO(ins)) {
            /* GTE: keep going at this level */
            insp = &ins->next[i];
            ret_ins = ins;
        } else {
            --i; /* LT: drop down a level */
            --insp;
        }
    }

    /*
     * If we didn't find any records greater than or equal to the target, we never set the return
     * value, set it to the first record in the list.
     *
     * Otherwise, it references a record less-than-or-equal to the target, move to a later record,
     * that is, a subsequent record greater than the target. Because inserts happen concurrently,
     * additional records might be inserted after the searched-for record that are still smaller
     * than the target, continue to move forward until reaching a record larger than the target.
     * There isn't any safety testing because we confirmed such a record exists before searching.
     */
    if ((ins = ret_ins) == NULL)
        ins = WT_SKIP_FIRST(ins_head);
    while (recno >= WT_INSERT_RECNO(ins))
        ins = WT_SKIP_NEXT(ins);
    return (ins);
}

/*
 * __col_insert_search_lt --
 *     Search a column-store insert list for the next smaller record.
 */
static inline WT_INSERT *
__col_insert_search_lt(WT_INSERT_HEAD *ins_head, uint64_t recno)
{
    WT_INSERT *ins, **insp, *ret_ins;
    int i;

    /* If there's no insert chain to search, we're done. */
    if ((ins = WT_SKIP_FIRST(ins_head)) == NULL)
        return (NULL);

    /* Fast path check for targets before the skiplist. */
    if (recno <= WT_INSERT_RECNO(ins))
        return (NULL);

    /*
     * The insert list is a skip list: start at the highest skip level, then go as far as possible
     * at each level before stepping down to the next.
     */
    ret_ins = NULL;
    for (i = WT_SKIP_MAXDEPTH - 1, insp = &ins_head->head[i]; i >= 0;) {
        /*
         * Use a local variable to access the insert because the skip list can change across
         * references.
         */
        WT_ORDERED_READ(ins, *insp);
        if (ins != NULL && recno > WT_INSERT_RECNO(ins)) {
            /* GT: keep going at this level */
            insp = &ins->next[i];
            ret_ins = ins;
        } else {
            --i; /* LTE: drop down a level */
            --insp;
        }
    }

    return (ret_ins);
}

/*
 * __col_insert_search_match --
 *     Search a column-store insert list for an exact match.
 */
static inline WT_INSERT *
__col_insert_search_match(WT_INSERT_HEAD *ins_head, uint64_t recno)
{
    WT_INSERT *ins, **insp;
    uint64_t ins_recno;
    int cmp, i;

    /* If there's no insert chain to search, we're done. */
    if ((ins = WT_SKIP_LAST(ins_head)) == NULL)
        return (NULL);

    /* Fast path the check for values at the end of the skiplist. */
    if (recno > WT_INSERT_RECNO(ins))
        return (NULL);
    if (recno == WT_INSERT_RECNO(ins))
        return (ins);

    /*
     * The insert list is a skip list: start at the highest skip level, then go as far as possible
     * at each level before stepping down to the next.
     */
    for (i = WT_SKIP_MAXDEPTH - 1, insp = &ins_head->head[i]; i >= 0;) {
        /*
         * Use a local variable to access the insert because the skip list can change across
         * references.
         */
        WT_ORDERED_READ(ins, *insp);
        if (ins == NULL) {
            --i;
            --insp;
            continue;
        }

        ins_recno = WT_INSERT_RECNO(ins);
        cmp = (recno == ins_recno) ? 0 : (recno < ins_recno) ? -1 : 1;

        if (cmp == 0) /* Exact match: return */
            return (ins);
        if (cmp > 0) /* Keep going at this level */
            insp = &ins->next[i];
        else { /* Drop down a level */
            --i;
            --insp;
        }
    }

    return (NULL);
}

/*
 * __col_insert_search --
 *     Search a column-store insert list, creating a skiplist stack as we go.
 */
static inline WT_INSERT *
__col_insert_search(
  WT_INSERT_HEAD *ins_head, WT_INSERT ***ins_stack, WT_INSERT **next_stack, uint64_t recno)
{
    WT_INSERT **insp, *ret_ins;
    uint64_t ins_recno;
    int cmp, i;

    /* If there's no insert chain to search, we're done. */
    if ((ret_ins = WT_SKIP_LAST(ins_head)) == NULL)
        return (NULL);

    /* Fast path appends. */
    if (recno >= WT_INSERT_RECNO(ret_ins)) {
        for (i = 0; i < WT_SKIP_MAXDEPTH; i++) {
            ins_stack[i] = (i == 0) ?
              &ret_ins->next[0] :
              (ins_head->tail[i] != NULL) ? &ins_head->tail[i]->next[i] : &ins_head->head[i];
            next_stack[i] = NULL;
        }
        return (ret_ins);
    }

    /*
     * The insert list is a skip list: start at the highest skip level, then go as far as possible
     * at each level before stepping down to the next.
     */
    for (i = WT_SKIP_MAXDEPTH - 1, insp = &ins_head->head[i]; i >= 0;) {
        if ((ret_ins = *insp) == NULL) {
            next_stack[i] = NULL;
            ins_stack[i--] = insp--;
            continue;
        }

        /*
         * When no exact match is found, the search returns the smallest key larger than the
         * searched-for key, or the largest key smaller than the searched-for key, if there is no
         * larger key. Our callers depend on that: specifically, the fixed-length column store
         * cursor code interprets returning a key smaller than the searched-for key to mean the
         * searched-for key is larger than any key on the page. Don't change that behavior, things
         * will break.
         */
        ins_recno = WT_INSERT_RECNO(ret_ins);
        cmp = (recno == ins_recno) ? 0 : (recno < ins_recno) ? -1 : 1;

        if (cmp > 0) /* Keep going at this level */
            insp = &ret_ins->next[i];
        else if (cmp == 0) /* Exact match: return */
            for (; i >= 0; i--) {
                next_stack[i] = ret_ins->next[i];
                ins_stack[i] = &ret_ins->next[i];
            }
        else { /* Drop down a level */
            next_stack[i] = ret_ins;
            ins_stack[i--] = insp--;
        }
    }
    return (ret_ins);
}

/*
 * __col_var_last_recno --
 *     Return the last record number for a variable-length column-store page.
 */
static inline uint64_t
__col_var_last_recno(WT_REF *ref)
{
    WT_COL_RLE *repeat;
    WT_PAGE *page;

    page = ref->page;

    /*
     * If there's an append list, there may be more records on the page. This function ignores those
     * records, our callers must handle that explicitly, if they care.
     */
    if (!WT_COL_VAR_REPEAT_SET(page))
        return (page->entries == 0 ? WT_RECNO_OOB : ref->ref_recno + (page->entries - 1));

    repeat = &page->pg_var_repeats[page->pg_var_nrepeats - 1];
    return ((repeat->recno + repeat->rle) - 1 + (page->entries - (repeat->indx + 1)));
}

/*
 * __col_fix_last_recno --
 *     Return the last record number for a fixed-length column-store page.
 */
static inline uint64_t
__col_fix_last_recno(WT_REF *ref)
{
    WT_PAGE *page;

    page = ref->page;

    /*
     * If there's an append list, there may be more records on the page. This function ignores those
     * records, our callers must handle that explicitly, if they care.
     */
    return (page->entries == 0 ? WT_RECNO_OOB : ref->ref_recno + (page->entries - 1));
}

/*
 * __col_var_search --
 *     Search a variable-length column-store page for a record.
 */
static inline WT_COL *
__col_var_search(WT_REF *ref, uint64_t recno, uint64_t *start_recnop)
{
    WT_COL_RLE *repeat;
    WT_PAGE *page;
    uint64_t start_recno;
    uint32_t base, indx, limit, start_indx;

    page = ref->page;

    /*
     * Find the matching slot.
     *
     * This is done in two stages: first, we do a binary search among any repeating records to find
     * largest repeating less than the search key. Once there, we can do a simple offset calculation
     * to find the correct slot for this record number, because we know any intervening records have
     * repeat counts of 1.
     */
    for (base = 0, limit = WT_COL_VAR_REPEAT_SET(page) ? page->pg_var_nrepeats : 0; limit != 0;
         limit >>= 1) {
        indx = base + (limit >> 1);

        repeat = page->pg_var_repeats + indx;
        if (recno >= repeat->recno && recno < repeat->recno + repeat->rle) {
            if (start_recnop != NULL)
                *start_recnop = repeat->recno;
            return (page->pg_var + repeat->indx);
        }
        if (recno < repeat->recno)
            continue;
        base = indx + 1;
        --limit;
    }

    /*
     * We didn't find an exact match, move forward from the largest repeat less than the search key.
     */
    if (base == 0) {
        start_indx = 0;
        start_recno = ref->ref_recno;
    } else {
        repeat = page->pg_var_repeats + (base - 1);
        start_indx = repeat->indx + 1;
        start_recno = repeat->recno + repeat->rle;
    }

    /*
     * !!!
     * The test could be written more simply as:
     *
     * 	(recno >= start_recno + (page->entries - start_indx))
     *
     * It's split into two parts because the simpler test will overflow if
     * searching for large record numbers.
     */
    if (recno >= start_recno && recno - start_recno >= page->entries - start_indx)
        return (NULL);

    return (page->pg_var + start_indx + (uint32_t)(recno - start_recno));
}
