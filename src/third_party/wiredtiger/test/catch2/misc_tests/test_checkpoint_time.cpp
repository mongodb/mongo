/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include "wt_internal.h"

#include "../utils.h"
#include "../wrappers/connection_wrapper.h"

TEST_CASE("meta_ckpt: optional checkpoint_time missing in metadata", "[meta][checkpoint]")
{
    const std::string home = "WT_TEST.checkpoint_time_missing";

    connection_wrapper conn(home, "create");
    WT_SESSION_IMPL *session = conn.create_session();
    REQUIRE(session != nullptr);

    SECTION("snapshot metadata without checkpoint_time")
    {
        uint64_t ckpt_time = 999;
        uint64_t snap_max = 0;
        uint64_t snap_min = 0;
        uint64_t snap_write_gen = 0;

        const char *snap_val = "snapshot_min=10,snapshot_max=20,snapshot_count=0";
        REQUIRE(__wt_metadata_update(session, WT_SYSTEM_CKPT_SNAPSHOT_URI, snap_val) == 0);

        int ret = __wt_meta_read_checkpoint_snapshot(
          session, nullptr, &snap_write_gen, &snap_min, &snap_max, nullptr, nullptr, &ckpt_time);

        REQUIRE(ret == 0);
        REQUIRE(snap_min == 10);
        REQUIRE(snap_max == 20);
        REQUIRE(ckpt_time == 0);
    }

    SECTION("checkpoint timestamp metadata without checkpoint_time")
    {
        uint64_t ckpt_time = 999;
        wt_timestamp_t ts = 0;

        const char *ckpt_val = "checkpoint_timestamp=\"100\"";
        REQUIRE(__wt_metadata_update(session, WT_SYSTEM_CKPT_URI, ckpt_val) == 0);

        int ret = __wt_meta_read_checkpoint_timestamp(session, nullptr, &ts, &ckpt_time);

        REQUIRE(ret == 0);
        REQUIRE(ts == 0x100);
        REQUIRE(ckpt_time == 0);
    }

    SECTION("oldest timestamp metadata without checkpoint_time")
    {
        uint64_t ckpt_time = 999;
        wt_timestamp_t ts = 0;

        const char *oldest_val = "oldest_timestamp=\"200\"";
        REQUIRE(__wt_metadata_update(session, WT_SYSTEM_OLDEST_URI, oldest_val) == 0);

        int ret = __wt_meta_read_checkpoint_oldest(session, nullptr, &ts, &ckpt_time);

        REQUIRE(ret == 0);
        REQUIRE(ts == 0x200);
        REQUIRE(ckpt_time == 0);
    }

    SECTION("snapshot and timestamp metadata with checkpoint_time present")
    {
        uint64_t ckpt_time = 0;
        wt_timestamp_t ts = 0;

        const char *ckpt_val = "checkpoint_timestamp=\"100\",checkpoint_time=12345";
        REQUIRE(__wt_metadata_update(session, WT_SYSTEM_CKPT_URI, ckpt_val) == 0);

        int ret = __wt_meta_read_checkpoint_timestamp(session, nullptr, &ts, &ckpt_time);

        REQUIRE(ret == 0);
        REQUIRE(ts == 0x100);
        REQUIRE(ckpt_time == 12345);
    }
}
