/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __col_insert_alloc(WT_SESSION_IMPL *, uint64_t, u_int, WT_INSERT **, size_t *);

/*
 * __wt_col_modify --
 *     Column-store delete, insert, and update.
 */
int
__wt_col_modify(WT_CURSOR_BTREE *cbt, uint64_t recno, const WT_ITEM *value, WT_UPDATE *upd_arg,
  u_int modify_type, bool exclusive
#ifdef HAVE_DIAGNOSTIC
  ,
  bool restore
#endif
)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_INSERT *ins;
    WT_INSERT_HEAD *ins_head, **ins_headp;
    WT_PAGE *page;
    WT_PAGE_MODIFY *mod;
    WT_SESSION_IMPL *session;
    WT_UPDATE *last_upd, *old_upd, *upd;
    wt_timestamp_t prev_upd_ts;
    size_t ins_size, upd_size;
    u_int i, skipdepth;
    bool append, inserted_to_update_chain, logged;

    btree = CUR2BT(cbt);
    ins = NULL;
    page = cbt->ref->page;
    session = CUR2S(cbt);
    last_upd = NULL;
    upd = upd_arg;
    prev_upd_ts = WT_TS_NONE;
    append = inserted_to_update_chain = logged = false;

    /*
     * We should have one of the following:
     * - A full update list to instantiate.
     * - An update to append to the existing update list.
     * - A key/value pair to create an update with and append to the update list.
     * - A key with no value to create a reserved or tombstone update to append to the update list.
     *
     * A "full update list" is distinguished from "an update" by checking whether it has a "next"
     * update.
     */
    WT_ASSERT(session,
      ((modify_type == WT_UPDATE_RESERVE || modify_type == WT_UPDATE_TOMBSTONE) && value == NULL &&
        upd_arg == NULL) ||
        (!(modify_type == WT_UPDATE_RESERVE || modify_type == WT_UPDATE_TOMBSTONE) &&
          ((value == NULL && upd_arg != NULL) || (value != NULL && upd_arg == NULL))));

    /* If we don't yet have a modify structure, we'll need one. */
    WT_RET(__wt_page_modify_init(session, page));
    mod = page->modify;

    if (upd_arg == NULL) {
        /*
         * There's a chance the application specified a record past the last record on the page. If
         * that's the case and we're inserting a new WT_INSERT/WT_UPDATE pair, it goes on the append
         * list, not the update list. Also, an out-of-band recno implies an append operation, we're
         * allocating a new row. Ignore any information obtained from the search.
         */
        WT_ASSERT(session, recno != WT_RECNO_OOB || cbt->compare != 0);
        if (cbt->compare != 0 &&
          (recno == WT_RECNO_OOB ||
            recno > (btree->type == BTREE_COL_VAR ? __col_var_last_recno(cbt->ref) :
                                                    __col_fix_last_recno(cbt->ref)))) {
            append = true;
            cbt->ins = NULL;
            cbt->ins_head = NULL;
        }
    } else {
        /* Since on this path we never set append, make sure we aren't appending. */
        WT_ASSERT(session, recno != WT_RECNO_OOB);
        WT_ASSERT(session,
          cbt->compare == 0 ||
            recno <= (btree->type == BTREE_COL_VAR ? __col_var_last_recno(cbt->ref) :
                                                     __col_fix_last_recno(cbt->ref)));
    }

    /*
     * If modifying a record not previously modified, but which is in the same update slot as a
     * previously modified record, cursor.ins will not be set because there's no list of update
     * records for this recno, but cursor.ins_head will be set to point to the correct update slot.
     * Acquire the necessary insert information, then create a new update entry and link it into the
     * existing list. We get here if a page has a single cell representing multiple records (the
     * records have the same value), and then a record in the cell is updated or removed, creating
     * the update list for the cell, and then a cursor iterates into that same cell to update/remove
     * a different record. We find the correct slot in the update array, but we don't find an update
     * list (because it doesn't exist), and don't have the information we need to do the insert.
     * Normally, we wouldn't care (we could fail and do a search for the record which would
     * configure everything for the insert), but range truncation does this pattern for every record
     * in the cell, and the performance is terrible. For that reason, catch it here.
     */
    if (cbt->ins == NULL && cbt->ins_head != NULL) {
        cbt->ins = __col_insert_search(cbt->ins_head, cbt->ins_stack, cbt->next_stack, recno);
        if (cbt->ins != NULL) {
            if (WT_INSERT_RECNO(cbt->ins) == recno)
                cbt->compare = 0;
            else {
                /*
                 * The test below is for cursor.compare set to 0 and cursor.ins set: cursor.compare
                 * wasn't set by the search we just did, and has an unknown value. Clear cursor.ins
                 * to avoid the test.
                 */
                cbt->ins = NULL;
            }
        }
    }

    /*
     * Modify a column-store entry. If modifying a previously modified record, cursor.ins will point
     * to the correct update list; create a new update and link it into the already existing list.
     * Otherwise, we have to insert a new insert/update pair into the column-store insert list.
     */
    if (cbt->compare == 0 && cbt->ins != NULL) {
        old_upd = cbt->ins->upd;
        if (upd_arg == NULL) {
            /* Make sure the modify can proceed. */
            WT_ERR(__wt_txn_modify_check(session, cbt, old_upd, &prev_upd_ts, modify_type));

            /* Allocate a WT_UPDATE structure and transaction ID. */
            WT_ERR(__wt_upd_alloc(session, value, modify_type, &upd, &upd_size));
            upd->prev_durable_ts = prev_upd_ts;
            WT_ERR(__wt_txn_modify(session, upd));
            logged = true;

            /* Avoid WT_CURSOR.update data copy. */
            __wt_upd_value_assign(cbt->modify_update, upd);
        } else {
            upd_size = __wt_update_list_memsize(upd);

            /* If there are existing updates, append them after the new updates. */
            for (last_upd = upd; last_upd->next != NULL; last_upd = last_upd->next)
                ;
            last_upd->next = old_upd;

            /*
             * If we restore an update chain in update restore eviction, there should be no update
             * on the existing update chain.
             */
            WT_ASSERT(session, !restore || old_upd == NULL);

            /*
             * We can either put multiple new updates or a single update on the update chain.
             *
             * Set the "old" entry to the second update in the list so that the serialization
             * function succeeds in swapping the first update into place.
             */
            if (upd->next != NULL)
                cbt->ins->upd = upd->next;
            old_upd = cbt->ins->upd;
        }

        /*
         * Point the new WT_UPDATE item to the next element in the list. If we get it right, the
         * serialization function lock acts as our memory barrier to flush this write.
         */
        upd->next = old_upd;

        /* Serialize the update. */
        WT_ERR(__wt_update_serial(session, cbt, page, &cbt->ins->upd, &upd, upd_size, false));
    } else {
        /* Make sure the modify can proceed. */
        if (cbt->compare == 0 && upd_arg == NULL)
            WT_ERR(__wt_txn_modify_check(session, cbt, NULL, &prev_upd_ts, modify_type));

        /* Allocate the append/update list reference as necessary. */
        if (append) {
            WT_PAGE_ALLOC_AND_SWAP(session, page, mod->mod_col_append, ins_headp, 1);
            ins_headp = &mod->mod_col_append[0];
        } else if (page->type == WT_PAGE_COL_FIX) {
            WT_PAGE_ALLOC_AND_SWAP(session, page, mod->mod_col_update, ins_headp, 1);
            ins_headp = &mod->mod_col_update[0];
        } else {
            WT_PAGE_ALLOC_AND_SWAP(session, page, mod->mod_col_update, ins_headp, page->entries);
            ins_headp = &mod->mod_col_update[cbt->slot];
        }

        /* Allocate the WT_INSERT_HEAD structure as necessary. */
        WT_PAGE_ALLOC_AND_SWAP(session, page, *ins_headp, ins_head, 1);
        ins_head = *ins_headp;

        /* Choose a skiplist depth for this insert. */
        skipdepth = __wt_skip_choose_depth(session);

        /*
         * Allocate a WT_INSERT/WT_UPDATE pair and transaction ID, and update the cursor to
         * reference it (the WT_INSERT_HEAD might be allocated, the WT_INSERT was allocated).
         */
        WT_ERR(__col_insert_alloc(session, recno, skipdepth, &ins, &ins_size));
        cbt->ins_head = ins_head;
        cbt->ins = ins;

        /*
         * Check for insert split and checkpoint races in column-store: it's easy (as opposed to in
         * row-store) and a difficult bug to otherwise diagnose.
         */
        WT_ASSERT(session,
          mod->mod_col_split_recno == WT_RECNO_OOB ||
            (recno != WT_RECNO_OOB && mod->mod_col_split_recno > recno));

        if (upd_arg == NULL) {
            WT_ERR(__wt_upd_alloc(session, value, modify_type, &upd, &upd_size));
            upd->prev_durable_ts = prev_upd_ts;
            WT_ERR(__wt_txn_modify(session, upd));
            logged = true;

            /* Avoid WT_CURSOR.update data copy. */
            __wt_upd_value_assign(cbt->modify_update, upd);
        } else
            upd_size = __wt_update_list_memsize(upd);
        ins->upd = upd;
        ins_size += upd_size;

        /*
         * If there was no insert list during the search, or there was no search because the record
         * number has not been allocated yet, the cursor's information cannot be correct, search
         * couldn't have initialized it.
         *
         * Otherwise, point the new WT_INSERT item's skiplist to the next elements in the insert
         * list (which we will check are still valid inside the serialization function).
         *
         * The serial mutex acts as our memory barrier to flush these writes before inserting them
         * into the list.
         */
        if (cbt->ins_stack[0] == NULL || recno == WT_RECNO_OOB)
            for (i = 0; i < skipdepth; i++) {
                cbt->ins_stack[i] = &ins_head->head[i];
                ins->next[i] = cbt->next_stack[i] = NULL;
            }
        else
            for (i = 0; i < skipdepth; i++)
                ins->next[i] = cbt->next_stack[i];

        /* Append or insert the WT_INSERT structure. */
        if (append)
            WT_ERR(__wt_col_append_serial(session, page, cbt->ins_head, cbt->ins_stack, &ins,
              ins_size, &cbt->recno, skipdepth, exclusive));
        else
            WT_ERR(__wt_insert_serial(
              session, page, cbt->ins_head, cbt->ins_stack, &ins, ins_size, skipdepth, exclusive));
    }

    inserted_to_update_chain = true;

    /* If the update was successful, add it to the in-memory log. */
    if (logged && modify_type != WT_UPDATE_RESERVE) {
        if (__wt_log_op(session))
            WT_ERR(__wt_txn_log_op(session, cbt));

        /*
         * In case of append, the recno (key) for the value is assigned now. Set the key in the
         * transaction operation to be used in case this transaction is prepared to retrieve the
         * update corresponding to this operation.
         */
        __wt_txn_op_set_recno(session, cbt->recno);
    }

    if (0) {
err:
        /* Remove the update from the current transaction; don't try to modify it on rollback. */
        if (logged)
            __wt_txn_unmodify(session);

        /* Free any allocated insert list object. */
        __wt_free(session, ins);

        cbt->ins = NULL;

        /* Discard any allocated update, unless we failed after linking it into page memory. */
        if (upd_arg == NULL && !inserted_to_update_chain)
            __wt_free(session, upd);

        /*
         * When prepending a list of updates to an update chain, we link them together; sever that
         * link so our callers list doesn't point into page memory.
         */
        if (last_upd != NULL)
            last_upd->next = NULL;
    }

    return (ret);
}

/*
 * __col_insert_alloc --
 *     Column-store insert: allocate a WT_INSERT structure and fill it in.
 */
static int
__col_insert_alloc(
  WT_SESSION_IMPL *session, uint64_t recno, u_int skipdepth, WT_INSERT **insp, size_t *ins_sizep)
{
    WT_INSERT *ins;
    size_t ins_size;

    /*
     * Allocate the WT_INSERT structure and skiplist pointers, then copy the record number into
     * place.
     */
    ins_size = sizeof(WT_INSERT) + skipdepth * sizeof(WT_INSERT *);
    WT_RET(__wt_calloc(session, 1, ins_size, &ins));

    WT_INSERT_RECNO(ins) = recno;

    *insp = ins;
    *ins_sizep = ins_size;
    return (0);
}
