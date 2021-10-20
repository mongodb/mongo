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
  u_int modify_type, bool exclusive)
{
    static const WT_ITEM col_fix_remove = {"", 1, NULL, 0, 0};
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_INSERT *ins;
    WT_INSERT_HEAD *ins_head, **ins_headp;
    WT_PAGE *page;
    WT_PAGE_MODIFY *mod;
    WT_SESSION_IMPL *session;
    WT_UPDATE *old_upd, *upd;
    size_t ins_size, upd_size;
    u_int i, skipdepth;
    bool append, logged;

    btree = cbt->btree;
    ins = NULL;
    page = cbt->ref->page;
    session = (WT_SESSION_IMPL *)cbt->iface.session;
    upd = upd_arg;
    append = logged = false;

    if (upd_arg == NULL) {
        if (modify_type == WT_UPDATE_RESERVE || modify_type == WT_UPDATE_TOMBSTONE) {
            /*
             * Fixed-size column-store doesn't have on-page deleted values, it's a nul byte.
             */
            if (modify_type == WT_UPDATE_TOMBSTONE && btree->type == BTREE_COL_FIX) {
                modify_type = WT_UPDATE_STANDARD;
                value = &col_fix_remove;
            }
        }

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
    }

    /* We're going to modify the page, we should have loaded history. */
    WT_ASSERT(session, cbt->ref->state != WT_REF_LIMBO);

    /* If we don't yet have a modify structure, we'll need one. */
    WT_RET(__wt_page_modify_init(session, page));
    mod = page->modify;

    /*
     * If modifying a record not previously modified, but which is in the
     * same update slot as a previously modified record, cursor.ins will
     * not be set because there's no list of update records for this recno,
     * but cursor.ins_head will be set to point to the correct update slot.
     * Acquire the necessary insert information, then create a new update
     * entry and link it into the existing list. We get here if a page has
     * a single cell representing multiple records (the records have the
     * same value), and then a record in the cell is updated or removed,
     * creating the update list for the cell, and then a cursor iterates
     * into that same cell to update/remove a different record. We find the
     * correct slot in the update array, but we don't find an update list
     * (because it doesn't exist), and don't have the information we need
     * to do the insert. Normally, we wouldn't care (we could fail and do
     * a search for the record which would configure everything for the
     * insert), but range truncation does this pattern for every record in
     * the cell, and the performance is terrible. For that reason, catch it
     * here.
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
     * Delete, insert or update a column-store entry.
     *
     * If modifying a previously modified record, cursor.ins will be set to
     * point to the correct update list. Create a new update entry and link
     * it into the existing list.
     *
     * Else, allocate an insert array as necessary, build an insert/update
     * structure pair, and link it into place.
     */
    if (cbt->compare == 0 && cbt->ins != NULL) {
        /*
         * If we are restoring updates that couldn't be evicted, the key must not exist on the new
         * page.
         */
        WT_ASSERT(session, upd_arg == NULL);

        /* Make sure the update can proceed. */
        WT_ERR(__wt_txn_update_check(session, old_upd = cbt->ins->upd));

        /* Allocate a WT_UPDATE structure and transaction ID. */
        WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size, modify_type));
        WT_ERR(__wt_txn_modify(session, upd));
        logged = true;

        /* Avoid a data copy in WT_CURSOR.update. */
        cbt->modify_update = upd;

        /*
         * Point the new WT_UPDATE item to the next element in the list. If we get it right, the
         * serialization function lock acts as our memory barrier to flush this write.
         */
        upd->next = old_upd;

        /* Serialize the update. */
        WT_ERR(__wt_update_serial(session, page, &cbt->ins->upd, &upd, upd_size, false));
    } else {
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
        WT_ASSERT(session, mod->mod_col_split_recno == WT_RECNO_OOB ||
            (recno != WT_RECNO_OOB && mod->mod_col_split_recno > recno));

        if (upd_arg == NULL) {
            WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size, modify_type));
            WT_ERR(__wt_txn_modify(session, upd));
            logged = true;

            /* Avoid a data copy in WT_CURSOR.update. */
            cbt->modify_update = upd;
        } else
            upd_size = __wt_update_list_memsize(upd);
        ins->upd = upd;
        ins_size += upd_size;

        /*
         * If there was no insert list during the search, or there was
         * no search because the record number has not been allocated
         * yet, the cursor's information cannot be correct, search
         * couldn't have initialized it.
         *
         * Otherwise, point the new WT_INSERT item's skiplist to the
         * next elements in the insert list (which we will check are
         * still valid inside the serialization function).
         *
         * The serial mutex acts as our memory barrier to flush these
         * writes before inserting them into the list.
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

    /* If the update was successful, add it to the in-memory log. */
    if (logged && modify_type != WT_UPDATE_RESERVE) {
        WT_ERR(__wt_txn_log_op(session, cbt));

        /*
         * In case of append, the recno (key) for the value is assigned now. Set the recno in the
         * transaction operation to be used incase this transaction is prepared to retrieve the
         * update corresponding to this operation.
         */
        __wt_txn_op_set_recno(session, cbt->recno);
    }

    if (0) {
err:
        /*
         * Remove the update from the current transaction, so we don't try to modify it on rollback.
         */
        if (logged)
            __wt_txn_unmodify(session);
        __wt_free(session, ins);
        if (upd_arg == NULL)
            __wt_free(session, upd);
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
