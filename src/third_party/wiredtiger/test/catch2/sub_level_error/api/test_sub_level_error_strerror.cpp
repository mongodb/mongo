/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include "wt_internal.h"
#include <string>

static void
check_error_code(int error, std::string expected)
{
    std::string result = wiredtiger_strerror(error);
    CHECK(result == expected);
}

TEST_CASE("Test generation of sub-level error codes when strerror is called",
  "[sub_level_error_strerror],[sub_level_error]")
{
    SECTION("Unique sub-level error codes")
    {
        std::vector<std::pair<int, std::string>> errors = {
          {WT_NONE, "WT_NONE: No additional context"},
          {WT_BACKGROUND_COMPACT_ALREADY_RUNNING,
            "WT_BACKGROUND_COMPACT_ALREADY_RUNNING: Background compaction is already running"},
          {WT_CACHE_OVERFLOW, "WT_CACHE_OVERFLOW: Cache capacity has overflown"},
          {WT_WRITE_CONFLICT, "WT_WRITE_CONFLICT: Write conflict between concurrent operations"},
          {WT_OLDEST_FOR_EVICTION,
            "WT_OLDEST_FOR_EVICTION: Transaction has the oldest pinned transaction ID"},
          {WT_CONFLICT_BACKUP,
            "WT_CONFLICT_BACKUP: Conflict performing operation due to running backup"},
          {WT_CONFLICT_DHANDLE,
            "WT_CONFLICT_DHANDLE: Another thread currently holds the data handle of the table"},
          {WT_CONFLICT_SCHEMA_LOCK,
            "WT_CONFLICT_SCHEMA_LOCK: Conflict performing schema operation"},
          {WT_UNCOMMITTED_DATA, "WT_UNCOMMITTED_DATA: Table has uncommitted data"},
          {WT_DIRTY_DATA, "WT_DIRTY_DATA: Table has dirty data"},
          {WT_CONFLICT_TABLE_LOCK,
            "WT_CONFLICT_TABLE_LOCK: Another thread currently holds the table lock"},
          {WT_CONFLICT_CHECKPOINT_LOCK,
            "WT_CONFLICT_CHECKPOINT_LOCK: Another thread currently holds the checkpoint lock"},
          {WT_CONFLICT_LIVE_RESTORE,
            "WT_CONFLICT_LIVE_RESTORE: Conflict performing operation due to an in-progress live "
            "restore"},
          {WT_CONFLICT_DISAGG, "WT_CONFLICT_DISAGG: Conflict with disaggregated storage"},
        };

        for (auto const [code, expected] : errors)
            check_error_code(code, expected);
    }
}
