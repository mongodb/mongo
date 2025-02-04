/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include "wt_internal.h"
#include "../../wrappers/connection_wrapper.h"

/*
 * [sub_level_error_is_valid_sub_level_error]: test_sub_level_error_is_valid_sub_level_error.c
 * This unit test file tests that the helper function __wt_is_valid_sub_level_error correctly
 * validates sub level error codes, which range between -32000 and -32199 inclusive.
 */

TEST_CASE(
  "Test that helper function __wt_is_valid_sub_level_error validates sub level error codes "
  "correctly",
  "[sub_level_error_is_valid_sub_level_error],[sub_level_error]")
{

    /* Ensure no normal error codes are mistaken for sub level error codes. */
    CHECK_FALSE(__wt_is_valid_sub_level_error(WT_ROLLBACK));
    CHECK_FALSE(__wt_is_valid_sub_level_error(WT_DUPLICATE_KEY));
    CHECK_FALSE(__wt_is_valid_sub_level_error(WT_ERROR));
    CHECK_FALSE(__wt_is_valid_sub_level_error(WT_NOTFOUND));
    CHECK_FALSE(__wt_is_valid_sub_level_error(WT_PANIC));
    CHECK_FALSE(__wt_is_valid_sub_level_error(WT_RESTART));
    CHECK_FALSE(__wt_is_valid_sub_level_error(WT_RUN_RECOVERY));
    CHECK_FALSE(__wt_is_valid_sub_level_error(WT_CACHE_FULL));
    CHECK_FALSE(__wt_is_valid_sub_level_error(WT_PREPARE_CONFLICT));
    CHECK_FALSE(__wt_is_valid_sub_level_error(WT_TRY_SALVAGE));

    /* Ensure that all valid sub level error codes are validated. */
    CHECK(__wt_is_valid_sub_level_error(WT_NONE));

    /* Boundary checks (valid between -32000 and -32199 inclusive). */
    CHECK_FALSE(__wt_is_valid_sub_level_error(-31999));
    CHECK(__wt_is_valid_sub_level_error(-32000));
    CHECK(__wt_is_valid_sub_level_error(-32001));
    CHECK(__wt_is_valid_sub_level_error(-32199));
    CHECK_FALSE(__wt_is_valid_sub_level_error(-32200));
    CHECK_FALSE(__wt_is_valid_sub_level_error(-32201));
}
