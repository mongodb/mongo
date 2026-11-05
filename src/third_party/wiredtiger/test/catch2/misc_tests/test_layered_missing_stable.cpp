/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifndef _WIN32

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <string>

#include <catch2/catch.hpp>

#include "wiredtiger.h"
#include "wt_internal.h"
#include "../utils.h"
#include "../wrappers/connection_wrapper.h"
#include "../../utility/test_util.h"

/*
 * test_layered_missing_stable.cpp
 *
 * Leader recovery succeeds with stable file metadata and aborts without it.
 */

static const std::string TABLE_NAME = "test_layered_missing_stable";
static const std::string TABLE_URI = "table:test_layered_missing_stable";

/*
 * build_cfg --
 *     Construct a wiredtiger_open config string for the given role. Pass create=true for the first
 *     open; omit it on reopens.
 */
static std::string
build_cfg(const std::string &role, bool create)
{
    std::string cfg;
    if (create)
        cfg += "create,";
    cfg += "statistics=(all),";
    cfg += "extensions=[./ext/page_log/palite/libwiredtiger_palite.so],";
    cfg += std::string("disaggregated=(role=") + role + ",page_log=palite)";
    return cfg;
}

/*
 * prepare_db --
 *     Create a layered table and optionally remove its stable file metadata.
 */
static void
prepare_db(const std::string &home, bool remove_stable)
{
    /* Start from a clean slate. */
    testutil_system("rm -rf %s && mkdir -p %s", home.c_str(), home.c_str());

    /* Phase 1: Open as leader, create a complete layered table, and close. */
    {
        connection_wrapper conn(home, build_cfg("leader", true).c_str());
        conn.clear_do_cleanup();
        WT_SESSION *session = (WT_SESSION *)conn.create_session();
        REQUIRE(session->create(session, TABLE_URI.c_str(),
                  "key_format=S,value_format=S,block_manager=disagg,type=layered") == 0);
        /* conn destructor closes the connection, checkpointing both metadata entries. */
    }

    if (!remove_stable)
        return;

    /*
     * Remove metadata through a follower with no open layered handle to avoid a panic on close.
     */
    {
        connection_wrapper conn(home, build_cfg("follower", false).c_str());
        conn.clear_do_cleanup();
        WT_SESSION_IMPL *session = conn.create_session();

        const std::string key = "file:" + TABLE_NAME + ".wt_stable";
        REQUIRE(__wt_metadata_remove(session, key.c_str()) == 0);
        /* conn destructor closes the connection, persisting the metadata removals. */
    }
}

/*
 * try_reopen --
 *     Attempt to reopen an existing database as a leader. Returns the wiredtiger_open return value;
 *     closes the connection on success.
 */
static int
try_reopen(const std::string &home)
{
    WT_CONNECTION *conn = nullptr;
    int ret = wiredtiger_open(home.c_str(), nullptr, build_cfg("leader", false).c_str(), &conn);
    if (ret == 0) {
        REQUIRE(conn != nullptr);
        conn->close(conn, nullptr);
    }
    return ret;
}

/*
 * reopen_aborts --
 *     Fork a child process that tries to reopen the database as a leader. Returns true if the child
 *     exited abnormally (killed by a signal such as SIGABRT, or with a non-zero exit status),
 *     indicating that WT_ASSERT_ALWAYS fired as expected.
 */
static bool
reopen_aborts(const std::string &home)
{
    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        /*
         * Child process: reset SIGABRT to the default disposition so that Catch2's signal handler
         * (which throws a C++ exception) cannot interfere with the abort() call inside
         * WT_ASSERT_ALWAYS. Without this, the Catch2 handler catches the signal, throws an
         * exception that escapes the if-block, and the child falls through to execute the parent's
         * waitpid path corrupting the test result.
         */
        signal(SIGABRT, SIG_DFL);
        int ret = try_reopen(home);
        _exit(ret == 0 ? 0 : 1);
    }

    /* Parent: wait for the child and inspect its exit status. */
    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFSIGNALED(status))
        return true; /* Killed by a signal (e.g. SIGABRT from WT_ASSERT_ALWAYS). */
    return !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}

TEST_CASE("Layered table leader recovery requires stable metadata", "[layered_missing_stable]")
{
    SECTION("stable metadata present: should succeed")
    {
        const std::string home = "WT_TEST.layered_missing_stable_present";
        prepare_db(home, /*remove_stable=*/false);
        REQUIRE(try_reopen(home) == 0);
        utils::wiredtiger_cleanup(home);
    }

    SECTION("stable metadata missing: should abort")
    {
        const std::string home = "WT_TEST.layered_missing_stable_missing";
        prepare_db(home, /*remove_stable=*/true);
        CHECK(reopen_aborts(home));
        utils::wiredtiger_cleanup(home);
    }
}

#endif /* !_WIN32 */
