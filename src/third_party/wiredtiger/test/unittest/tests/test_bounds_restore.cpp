/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "utils.h"
#include "wiredtiger.h"
#include "wrappers/connection_wrapper.h"
#include "wt_internal.h"

bool
validate_cursor_bounds_restore(WT_CURSOR *cursor, uint64_t original_cursor_flags)
{
    return cursor->flags == original_cursor_flags;
}

TEST_CASE("Bounds save and restore flag logic", "[bounds_restore]")
{
    WT_CURSOR mock_cursor;
    WT_CURSOR_BOUNDS_STATE mock_state;

    ConnectionWrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.createSession();

    mock_state.lower_bound = NULL;
    mock_state.upper_bound = NULL;

    mock_cursor.flags = 0;
    mock_cursor.lower_bound.size = 0;
    mock_cursor.lower_bound.data = NULL;
    mock_cursor.upper_bound.size = 0;
    mock_cursor.upper_bound.data = NULL;

    // Save bounds flags and ensure that the restore logic correctly restores the desired flags.
    SECTION("Save non-empty non-inclusive bounds flags and restore")
    {
        F_SET(&mock_cursor, WT_CURSTD_BOUND_UPPER);
        F_SET(&mock_cursor, WT_CURSTD_BOUND_LOWER);
        uint64_t original_cursor_flags = mock_cursor.flags;
        REQUIRE(__wt_cursor_bounds_save(session, &mock_cursor, &mock_state) == 0);
        REQUIRE(__wt_cursor_bounds_restore(session, &mock_cursor, &mock_state) == 0);
        REQUIRE(validate_cursor_bounds_restore(&mock_cursor, original_cursor_flags));
        __wt_scr_free(session, &mock_state.lower_bound);
        __wt_scr_free(session, &mock_state.upper_bound);
    }

    SECTION("Save non-empty inclusive bounds flags and restore")
    {
        F_SET(&mock_cursor, WT_CURSTD_BOUND_UPPER_INCLUSIVE);
        F_SET(&mock_cursor, WT_CURSTD_BOUND_LOWER_INCLUSIVE);
        uint64_t original_cursor_flags = mock_cursor.flags;
        REQUIRE(__wt_cursor_bounds_save(session, &mock_cursor, &mock_state) == 0);
        REQUIRE(__wt_cursor_bounds_restore(session, &mock_cursor, &mock_state) == 0);
        REQUIRE(validate_cursor_bounds_restore(&mock_cursor, original_cursor_flags));
    }
}
