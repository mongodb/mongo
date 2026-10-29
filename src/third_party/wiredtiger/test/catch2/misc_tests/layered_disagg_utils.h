/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <string>

#include "wt_internal.h"
#include "../../utility/test_util.h"
#include <catch2/catch.hpp>

/*
 * layered_disagg_build_cfg --
 *     Construct a wiredtiger_open config string for the given disaggregated role.
 */
static inline std::string
layered_disagg_build_cfg(const std::string &role)
{
    std::string cfg;
    cfg += "create,statistics=(all),";
    cfg += "extensions=[./ext/page_log/palite/libwiredtiger_palite.so],";
    cfg += std::string("disaggregated=(role=") + role + ",page_log=palite)";
    return cfg;
}

/*
 * layered_disagg_leader_checkpoint --
 *     Advance the stable timestamp and take a checkpoint on the leader.
 */
static inline void
layered_disagg_leader_checkpoint(WT_CONNECTION *conn, WT_SESSION *session, uint32_t stable_ts)
{
    char cfg[64];

    testutil_snprintf(cfg, sizeof(cfg), "stable_timestamp=%x", stable_ts);
    REQUIRE(conn->set_timestamp(conn, cfg) == 0);
    REQUIRE(session->checkpoint(session, nullptr) == 0);
}

/*
 * layered_disagg_pickup_latest_checkpoint --
 *     Make the follower pick up the latest complete checkpoint from the page log.
 */
static inline void
layered_disagg_pickup_latest_checkpoint(WT_CONNECTION *conn, WT_SESSION *session)
{
    WT_PAGE_LOG *page_log;
    WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS args;
    char cfg[1024];

    REQUIRE(conn->get_page_log(conn, "palite", &page_log) == 0);
    memset(&args, 0, sizeof(args));
    REQUIRE(page_log->pl_get_complete_checkpoint(page_log, session, &args) == 0);
    testutil_snprintf(cfg, sizeof(cfg), "disaggregated=(checkpoint_meta=\"%.*s\")",
      (int)args.checkpoint_metadata.size, (const char *)args.checkpoint_metadata.data);
    REQUIRE(conn->reconfigure(conn, cfg) == 0);
    free(args.checkpoint_metadata.mem);
    REQUIRE(page_log->terminate(page_log, session) == 0);
}
