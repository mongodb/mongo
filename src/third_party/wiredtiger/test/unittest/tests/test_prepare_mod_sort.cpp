/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <algorithm>
#include <list>

#include <catch2/catch.hpp>

#include "utils.h"
#include "wt_internal.h"
#include "wrappers/item_wrapper.h"
#include "wrappers/connection_wrapper.h"
#include "wiredtiger.h"

namespace {

bool
has_key(WT_TXN_TYPE type)
{
    switch (type) {
    case (WT_TXN_OP_NONE):
    case (WT_TXN_OP_REF_DELETE):
    case (WT_TXN_OP_TRUNCATE_COL):
    case (WT_TXN_OP_TRUNCATE_ROW):
        return (false);
    case (WT_TXN_OP_BASIC_COL):
    case (WT_TXN_OP_BASIC_ROW):
    case (WT_TXN_OP_INMEM_COL):
    case (WT_TXN_OP_INMEM_ROW):
        return (true);
    }

    return (false);
}

/* Verify the given modifications are sorted. */
static bool WT_CDECL
__mod_ops_sorted(WT_TXN_OP *ops, int op_count, size_t key_size)
{
    WT_TXN_OP *aopt, *bopt;

    for (int i = 0; i < op_count - 1; i++) {
        aopt = &ops[i];
        bopt = &ops[i + 1];

        /* Non key'd operations can separate any modifications with keys. */
        if ((aopt->btree->id == bopt->btree->id) && (!has_key(bopt->type) || !has_key(aopt->type)))
            return (true);

        /* B-tree ids must be in ascending order. */
        if ((aopt->btree->id > bopt->btree->id) && has_key(bopt->type))
            return (false);

        /* Check the key/recno if btree ids are the same. */
        if (aopt->btree->id == bopt->btree->id) {
            if (aopt->btree->type == BTREE_ROW && bopt->btree->type == BTREE_ROW) {
                auto a_key = aopt->u.op_row.key.data;
                auto b_key = bopt->u.op_row.key.data;

                if (strncmp((char *)a_key, (char *)b_key, key_size) > 0)
                    return (false);
            }

            if (aopt->btree->type == BTREE_COL_VAR && bopt->btree->type == BTREE_COL_VAR) {
                if (aopt->u.op_col.recno > bopt->u.op_col.recno)
                    return (false);
            }
        }
    }
    return (true);
}

/* Return a random non-key'd optype. */
WT_TXN_TYPE
rand_non_keyd_type()
{
    WT_TXN_TYPE types[4] = {
      WT_TXN_OP_NONE, WT_TXN_OP_REF_DELETE, WT_TXN_OP_TRUNCATE_COL, WT_TXN_OP_TRUNCATE_ROW};
    return (types[rand() % 4]);
}

/* Initialize a b-tree with a given type and ID. */
void
init_btree(WT_BTREE *btree, WT_BTREE_TYPE type, uint32_t id)
{
    btree->type = type;
    btree->id = id;
    btree->collator = NULL;
}

/* Initialize a mod operation. */
void
init_op(WT_TXN_OP *op, WT_BTREE *btree, WT_TXN_TYPE type, uint64_t recno, WT_ITEM *key)
{
    op->btree = btree;
    op->type = type;
    if (op->type == WT_TXN_OP_BASIC_COL || op->type == WT_TXN_OP_INMEM_COL) {
        REQUIRE(recno != WT_RECNO_OOB);
        op->u.op_col.recno = recno;
    } else if (op->type == WT_TXN_OP_BASIC_ROW || op->type == WT_TXN_OP_INMEM_ROW) {
        REQUIRE(key != NULL);
        op->u.op_row.key = *key;
    } else
        REQUIRE(!(has_key(op->type)));
}

/* Initialize a row-store key. */
void
init_key(WT_SESSION_IMPL *session, WT_ITEM *key, std::string key_str)
{
    WT_DECL_RET;

    ret = __wt_buf_init(session, key, key_str.size());
    REQUIRE(ret == 0);
    ret = __wt_buf_set(session, key, key_str.c_str(), key_str.size());
    REQUIRE(ret == 0);
}

/* Generate random keys. */
std::string
random_keys(size_t length)
{
    auto randchar = []() -> char {
        const char charset[] = "abcdefghijklmnopqrstuvwxyz";
        const size_t max_index = (sizeof(charset) - 1);
        return charset[rand() % max_index];
    };
    static std::string str(length, 0);
    std::generate_n(str.begin(), length, randchar);
    return str;
}

/* Allocate space for row-store keys. */
void
allocate_key_space(WT_SESSION_IMPL *session, int key_count, WT_ITEM *keys[])
{
    for (int i = 0; i < key_count; i++) {
        WT_DECL_ITEM(key);
        REQUIRE(__wt_scr_alloc(session, 0, &key) == 0);
        keys[i] = key;
    }
}

// Test sorting with column and non-key'd operations.
TEST_CASE("Basic cols and non key'd op", "[mod_compare]")
{
    WT_BTREE btrees[2];
    WT_TXN_OP ops[2];

    init_btree(&btrees[0], BTREE_ROW, 1);
    init_btree(&btrees[1], BTREE_COL_VAR, 2);

    init_op(&ops[0], &btrees[0], WT_TXN_OP_NONE, WT_RECNO_OOB, NULL);
    init_op(&ops[1], &btrees[1], WT_TXN_OP_BASIC_COL, 54, NULL);

    __wt_qsort(&ops, 2, sizeof(WT_TXN_OP), __ut_txn_mod_compare);
    REQUIRE(__mod_ops_sorted(ops, 2, 0) == true);
}

// Test sorting with row and non-key'd operations.
TEST_CASE("Basic rows and non key'd op", "[mod_compare]")
{
    ConnectionWrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.createSession();

    WT_BTREE btrees[2];
    WT_TXN_OP ops[4];
    const int key_count = 3, key_size = 2;
    bool ret;
    WT_ITEM *keys[key_count];

    allocate_key_space(session, key_count, keys);

    init_key(session, keys[0], "51");
    init_key(session, keys[1], "40");
    init_key(session, keys[2], "54");

    init_btree(&btrees[0], BTREE_COL_VAR, 1);
    init_btree(&btrees[1], BTREE_ROW, 2);

    // Initialize row ops with different keys.
    for (int i = 0; i < key_count; i++)
        init_op(&ops[i], &btrees[1], WT_TXN_OP_BASIC_ROW, WT_RECNO_OOB, keys[i]);

    init_op(&ops[3], &btrees[0], WT_TXN_OP_NONE, WT_RECNO_OOB, NULL);

    __wt_qsort(&ops, 4, sizeof(WT_TXN_OP), __ut_txn_mod_compare);
    ret = __mod_ops_sorted(ops, 4, key_size);

    // Free the allocated scratch buffers.
    for (int i = 0; i < key_count; i++)
        __wt_scr_free(session, &keys[i]);

    REQUIRE(ret == true);
}

// Test sorting with row, column and operations with no keys.
TEST_CASE("Row, column, and non key'd operations", "[mod_compare]")
{
    ConnectionWrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.createSession();

    WT_BTREE btrees[2];
    WT_TXN_OP ops[10];
    const int key_count = 6, key_size = 3;
    bool ret;
    WT_ITEM *keys[key_count];

    allocate_key_space(session, key_count, keys);

    for (int i = 0; i < 6; i++)
        init_key(session, keys[i], random_keys(3));

    init_btree(&btrees[0], BTREE_COL_VAR, 1);
    init_btree(&btrees[1], BTREE_ROW, 2);

    // Column operations.
    init_op(&ops[0], &btrees[0], WT_TXN_OP_BASIC_COL, 12, NULL);
    init_op(&ops[1], &btrees[0], WT_TXN_OP_BASIC_COL, 45, NULL);

    // Row operations.
    init_op(&ops[2], &btrees[1], WT_TXN_OP_REF_DELETE, WT_RECNO_OOB, keys[0]);
    init_op(&ops[3], &btrees[1], WT_TXN_OP_BASIC_ROW, WT_RECNO_OOB, keys[1]);
    init_op(&ops[4], &btrees[1], WT_TXN_OP_BASIC_ROW, WT_RECNO_OOB, keys[2]);
    init_op(&ops[5], &btrees[1], WT_TXN_OP_BASIC_ROW, WT_RECNO_OOB, keys[3]);
    init_op(&ops[6], &btrees[1], WT_TXN_OP_BASIC_ROW, WT_RECNO_OOB, keys[4]);
    init_op(&ops[7], &btrees[1], WT_TXN_OP_BASIC_ROW, WT_RECNO_OOB, keys[5]);

    // Non key'd operations.
    init_op(&ops[8], &btrees[0], WT_TXN_OP_TRUNCATE_COL, WT_RECNO_OOB, NULL);
    init_op(&ops[9], &btrees[1], WT_TXN_OP_REF_DELETE, WT_RECNO_OOB, NULL);

    __wt_qsort(&ops, 10, sizeof(WT_TXN_OP), __ut_txn_mod_compare);
    ret = __mod_ops_sorted(ops, 10, key_size);

    // Free the allocated scratch buffers.
    for (int i = 0; i < key_count; i++)
        __wt_scr_free(session, &keys[i]);

    REQUIRE(ret == true);
}

// Test sorting by b-tree ID. All operations have the same key.
TEST_CASE("B-tree ID sort test", "[mod_compare]")
{
    ConnectionWrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.createSession();

    WT_BTREE btrees[6];
    WT_TXN_OP ops[6];
    const int key_count = 1, key_size = 1;
    bool ret;
    WT_ITEM *keys[key_count];
    std::string key_str = "1";

    allocate_key_space(session, key_count, keys);

    init_key(session, keys[0], key_str);

    for (int i = 0; i < 6; i++)
        init_btree(&btrees[i], BTREE_ROW, rand() % 400);

    for (int i = 0; i < 6; i++)
        init_op(&ops[i], &btrees[i], WT_TXN_OP_BASIC_ROW, WT_RECNO_OOB, keys[0]);

    __wt_qsort(&ops, 6, sizeof(WT_TXN_OP), __ut_txn_mod_compare);
    ret = __mod_ops_sorted(ops, 6, key_size);

    // Free the allocated scratch buffers first.
    for (int i = 0; i < key_count; i++)
        __wt_scr_free(session, &keys[i]);

    REQUIRE(ret == true);
}

// Test sorting by keyedness, key'd operations all have the same key and recno.
TEST_CASE("Keyedness sort test", "[mod_compare]")
{
    ConnectionWrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.createSession();

    WT_BTREE btrees[12];
    WT_TXN_OP ops[12];
    const int key_count = 1, key_size = 1;
    bool ret;
    WT_ITEM *keys[key_count];
    std::string key_str = "1";

    allocate_key_space(session, key_count, keys);

    init_key(session, keys[0], key_str);

    for (int i = 0; i < 6; i++)
        init_btree(&btrees[i], BTREE_ROW, i);
    for (int i = 6; i < 12; i++)
        init_btree(&btrees[i], BTREE_COL_VAR, i);

    for (int i = 0; i < 6; i++)
        init_op(&ops[i], &btrees[i], WT_TXN_OP_BASIC_ROW, WT_RECNO_OOB, keys[0]);

    for (int i = 6; i < 9; i++)
        init_op(&ops[i], &btrees[i], WT_TXN_OP_BASIC_COL, 54, NULL);

    for (int i = 9; i < 12; i++)
        init_op(&ops[i], &btrees[i], rand_non_keyd_type(), WT_RECNO_OOB, NULL);

    __wt_qsort(&ops, 12, sizeof(WT_TXN_OP), __ut_txn_mod_compare);
    ret = __mod_ops_sorted(ops, 12, key_size);

    for (int i = 0; i < key_count; i++)
        __wt_scr_free(session, &keys[i]);

    REQUIRE(ret == true);
}

// Test sorting with randomly generated keys on 2 row-store b-trees.
TEST_CASE("Many different row-store keys", "[mod_compare]")
{
    ConnectionWrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.createSession();

    WT_BTREE btrees[12];
    WT_TXN_OP ops[12];
    const int key_count = 12, key_size = 3;
    bool ret;
    WT_ITEM *keys[key_count];

    allocate_key_space(session, key_count, keys);

    for (int i = 0; i < 12; i++)
        init_key(session, keys[i], random_keys(3));

    for (int i = 0; i < 6; i++)
        init_btree(&btrees[i], BTREE_ROW, 1);
    for (int i = 6; i < 12; i++)
        init_btree(&btrees[i], BTREE_ROW, 2);

    // Operations will have randomly chosen btrees and randomly generated keys.
    for (int i = 0; i < 12; i++)
        init_op(&ops[i], &btrees[i], WT_TXN_OP_BASIC_ROW, WT_RECNO_OOB, keys[0]);

    __wt_qsort(&ops, 12, sizeof(WT_TXN_OP), __ut_txn_mod_compare);
    ret = __mod_ops_sorted(ops, 12, key_size);

    for (int i = 0; i < key_count; i++)
        __wt_scr_free(session, &keys[i]);

    REQUIRE(ret == true);
}

// Test sorting on column store keys.
TEST_CASE("Different column store keys test", "[mod_compare]")
{
    WT_BTREE btrees[6];
    WT_TXN_OP ops[8];
    bool ret;

    for (int i = 0; i < 6; i++)
        init_btree(&btrees[i], BTREE_COL_VAR, i);

    // Randomly choose btrees and assign random recnos to the ops.
    for (int i = 0; i < 8; i++)
        init_op(&ops[i], &btrees[rand() % 6], WT_TXN_OP_BASIC_COL, rand() % 200 + 1, NULL);

    __wt_qsort(&ops, 8, sizeof(WT_TXN_OP), __ut_txn_mod_compare);
    ret = __mod_ops_sorted(ops, 8, 0);

    REQUIRE(ret == true);
}

} // namespace
