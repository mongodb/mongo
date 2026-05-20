/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include "wt_internal.h"

#include <cstring>

/* Initialize a WT_CKPT entry. Pass NULL as name for the sentinel. */
static void
make_ckpt(WT_CKPT *ckpt, const char *name, uint32_t flags)
{
    memset(ckpt, 0, sizeof(*ckpt));
    ckpt->name = (name != NULL) ? const_cast<char *>(name) : NULL;
    ckpt->flags = flags;
}

/* Empty list: only the NULL sentinel, no real entries. */
TEST_CASE("__checkpoint_skip_ckptlist: empty list returns false", "[checkpoint_mark_skip]")
{
    WT_CKPT list[1];
    make_ckpt(&list[0], NULL, 0);
    REQUIRE(__ut_checkpoint_skip_ckptlist(list) == false);
}

/* Single entry: cannot compare two names. */
TEST_CASE("__checkpoint_skip_ckptlist: single entry returns false", "[checkpoint_mark_skip]")
{
    WT_CKPT list[2];
    make_ckpt(&list[0], "WiredTigerCheckpoint.1", 0);
    make_ckpt(&list[1], NULL, 0);
    REQUIRE(__ut_checkpoint_skip_ckptlist(list) == false);
}

/* Two entries with the same name: checkpoint can be skipped. */
TEST_CASE(
  "__checkpoint_skip_ckptlist: two entries same name returns true", "[checkpoint_mark_skip]")
{
    WT_CKPT list[3];
    make_ckpt(&list[0], "my_checkpoint", 0);
    make_ckpt(&list[1], "my_checkpoint", 0);
    make_ckpt(&list[2], NULL, 0);
    REQUIRE(__ut_checkpoint_skip_ckptlist(list) == true);
}

/* Two entries with different names: checkpoint must not be skipped. */
TEST_CASE(
  "__checkpoint_skip_ckptlist: two entries different names returns false", "[checkpoint_mark_skip]")
{
    WT_CKPT list[3];
    make_ckpt(&list[0], "checkpoint_A", 0);
    make_ckpt(&list[1], "checkpoint_B", 0);
    make_ckpt(&list[2], NULL, 0);
    REQUIRE(__ut_checkpoint_skip_ckptlist(list) == false);
}

/* Two internal-prefix entries (e.g. WiredTigerCheckpoint.N): treated as equivalent. */
TEST_CASE(
  "__checkpoint_skip_ckptlist: two internal-prefix entries returns true", "[checkpoint_mark_skip]")
{
    WT_CKPT list[3];
    make_ckpt(&list[0], WT_CHECKPOINT ".1", 0);
    make_ckpt(&list[1], WT_CHECKPOINT ".2", 0);
    make_ckpt(&list[2], NULL, 0);
    REQUIRE(__ut_checkpoint_skip_ckptlist(list) == true);
}

/* Two or more deletions: may reclaim space, do not skip. */
TEST_CASE("__checkpoint_skip_ckptlist: two deletions returns false", "[checkpoint_mark_skip]")
{
    WT_CKPT list[4];
    make_ckpt(&list[0], "my_checkpoint", WT_CKPT_DELETE);
    make_ckpt(&list[1], "my_checkpoint", WT_CKPT_DELETE);
    make_ckpt(&list[2], "my_checkpoint", 0);
    make_ckpt(&list[3], NULL, 0);
    REQUIRE(__ut_checkpoint_skip_ckptlist(list) == false);
}

/* Three entries where the last two share an internal prefix: skip. */
TEST_CASE(
  "__checkpoint_skip_ckptlist: three entries last two match returns true", "[checkpoint_mark_skip]")
{
    WT_CKPT list[4];
    make_ckpt(&list[0], WT_CHECKPOINT ".1", 0);
    make_ckpt(&list[1], WT_CHECKPOINT ".2", 0);
    make_ckpt(&list[2], WT_CHECKPOINT ".3", 0);
    make_ckpt(&list[3], NULL, 0);
    REQUIRE(__ut_checkpoint_skip_ckptlist(list) == true);
}

/* Three entries where the last two differ: do not skip. */
TEST_CASE("__checkpoint_skip_ckptlist: three entries last two differ returns false",
  "[checkpoint_mark_skip]")
{
    WT_CKPT list[4];
    make_ckpt(&list[0], "snap_A", 0);
    make_ckpt(&list[1], "snap_B", 0);
    make_ckpt(&list[2], "snap_C", 0);
    make_ckpt(&list[3], NULL, 0);
    REQUIRE(__ut_checkpoint_skip_ckptlist(list) == false);
}

/* One deletion with matching last-two names: single deletion still allows skip. */
TEST_CASE("__checkpoint_skip_ckptlist: one deletion same last-two names returns true",
  "[checkpoint_mark_skip]")
{
    WT_CKPT list[4];
    make_ckpt(&list[0], "my_checkpoint", WT_CKPT_DELETE);
    make_ckpt(&list[1], "my_checkpoint", 0);
    make_ckpt(&list[2], "my_checkpoint", 0);
    make_ckpt(&list[3], NULL, 0);
    REQUIRE(__ut_checkpoint_skip_ckptlist(list) == true);
}
