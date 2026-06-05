/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <cstdint>

#include "wt_internal.h"

/*
 * This build can read a disagg block header if its own version is at least the header's
 * compatible_version. The read path used to compare against WT_BLOCK_DISAGG_COMPATIBLE_VERSION
 * (this build's own oldest-compatible bound) instead of WT_BLOCK_DISAGG_VERSION (this build's
 * version), incorrectly rejecting readable blocks once the writer version diverges from the
 * compatible version. The check is currently equivalent because the two macros are equal; this test
 * fails the moment WT_BLOCK_DISAGG_VERSION is bumped past WT_BLOCK_DISAGG_COMPATIBLE_VERSION and
 * the wrong macro is used, which is exactly when the bug can be observed.
 */
TEST_CASE("disagg block header version compatibility", "[block_disagg]")
{
    /* This build can read a header that only requires an older or equal version. */
    REQUIRE(__ut_block_disagg_header_version_compatible(1));
    REQUIRE(__ut_block_disagg_header_version_compatible(WT_BLOCK_DISAGG_COMPATIBLE_VERSION));

    /*
     * This build must read a header whose compatible version equals its own version. This is the
     * case the buggy check got wrong once the writer version diverges from the compatible version.
     */
    REQUIRE(__ut_block_disagg_header_version_compatible(WT_BLOCK_DISAGG_VERSION));

    /* This build cannot read a header that requires a newer reader than its own version. */
    REQUIRE_FALSE(__ut_block_disagg_header_version_compatible(WT_BLOCK_DISAGG_VERSION + 1));
}
