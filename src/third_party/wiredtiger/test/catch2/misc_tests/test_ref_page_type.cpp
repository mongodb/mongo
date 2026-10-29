/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"

TEST_CASE("Reference and page type consistency", "[ref_page_type]")
{
    WT_REF ref;

    memset((void *)&ref, 0, sizeof(ref));

    SECTION("Internal reference")
    {
        F_SET(&ref, WT_REF_FLAG_INTERNAL);
        REQUIRE(__wt_ref_type_matches_page(&ref, WT_PAGE_ROW_INT));
        REQUIRE(__wt_ref_type_matches_page(&ref, WT_PAGE_COL_INT));
        REQUIRE_FALSE(__wt_ref_type_matches_page(&ref, WT_PAGE_ROW_LEAF));
        REQUIRE_FALSE(__wt_ref_type_matches_page(&ref, WT_PAGE_COL_VAR));
    }

    SECTION("Leaf reference")
    {
        F_SET(&ref, WT_REF_FLAG_LEAF);
        REQUIRE(__wt_ref_type_matches_page(&ref, WT_PAGE_ROW_LEAF));
        REQUIRE(__wt_ref_type_matches_page(&ref, WT_PAGE_COL_VAR));
        REQUIRE_FALSE(__wt_ref_type_matches_page(&ref, WT_PAGE_ROW_INT));
        REQUIRE_FALSE(__wt_ref_type_matches_page(&ref, WT_PAGE_COL_INT));
    }
}

TEST_CASE("Page type matches btree format", "[ref_page_type]")
{
    WT_BTREE btree;

    memset((void *)&btree, 0, sizeof(btree));

    SECTION("Column-store handle")
    {
        btree.type = BTREE_COL_VAR;
        REQUIRE(__wt_page_type_matches_btree(&btree, WT_PAGE_COL_INT));
        REQUIRE(__wt_page_type_matches_btree(&btree, WT_PAGE_COL_VAR));
        /* A row-store page must not be reachable through a column-store handle. */
        REQUIRE_FALSE(__wt_page_type_matches_btree(&btree, WT_PAGE_ROW_INT));
        REQUIRE_FALSE(__wt_page_type_matches_btree(&btree, WT_PAGE_ROW_LEAF));
        /* Deprecated fixed-length and non-tree page types never belong. */
        REQUIRE_FALSE(__wt_page_type_matches_btree(&btree, WT_PAGE_COL_FIX_DEPRECATED));
        REQUIRE_FALSE(__wt_page_type_matches_btree(&btree, WT_PAGE_INVALID));
        REQUIRE_FALSE(__wt_page_type_matches_btree(&btree, WT_PAGE_BLOCK_MANAGER));
        REQUIRE_FALSE(__wt_page_type_matches_btree(&btree, WT_PAGE_OVFL));
    }

    SECTION("Row-store handle")
    {
        btree.type = BTREE_ROW;
        REQUIRE(__wt_page_type_matches_btree(&btree, WT_PAGE_ROW_INT));
        REQUIRE(__wt_page_type_matches_btree(&btree, WT_PAGE_ROW_LEAF));
        /* A column-store page must not be reachable through a row-store handle. */
        REQUIRE_FALSE(__wt_page_type_matches_btree(&btree, WT_PAGE_COL_INT));
        REQUIRE_FALSE(__wt_page_type_matches_btree(&btree, WT_PAGE_COL_VAR));
        /* Deprecated fixed-length and non-tree page types never belong. */
        REQUIRE_FALSE(__wt_page_type_matches_btree(&btree, WT_PAGE_COL_FIX_DEPRECATED));
        REQUIRE_FALSE(__wt_page_type_matches_btree(&btree, WT_PAGE_INVALID));
        REQUIRE_FALSE(__wt_page_type_matches_btree(&btree, WT_PAGE_BLOCK_MANAGER));
        REQUIRE_FALSE(__wt_page_type_matches_btree(&btree, WT_PAGE_OVFL));
    }
}

TEST_CASE("Reference type mismatch error policy", "[ref_page_type]")
{
    WT_BTREE btree;
    WT_DATA_HANDLE dhandle;
    WT_SESSION_IMPL session;

    memset((void *)&btree, 0, sizeof(btree));
    memset((void *)&dhandle, 0, sizeof(dhandle));
    memset((void *)&session, 0, sizeof(session));
    dhandle.handle = &btree;
    session.dhandle = &dhandle;

    REQUIRE_FALSE(__wt_ref_type_mismatch_returns_error(&session));

    F_SET(&btree, WT_BTREE_VERIFY);
    REQUIRE(__wt_ref_type_mismatch_returns_error(&session));
    F_CLR(&btree, WT_BTREE_VERIFY);

    F_SET(&session, WT_SESSION_READ_SKIP_CORRUPT);
    REQUIRE(__wt_ref_type_mismatch_returns_error(&session));
}
