/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * These tests exercise the layered-cursor helpers that read their per-operation state from a
 * WTI_CLAYERED_OP. They drive those helpers over plain file: cursors, without any leader/follower
 * role machinery or a disaggregated checkpoint.
 *
 * The helpers under test are file-static, so we #include cur_layered.c and call them by name
 * instead of going through exported wrappers. A real layered: cursor cannot be opened in this
 * configuration (it would require the disaggregated storage / page-log path), but the helpers
 * themselves only need a session, a couple of positioned cursors, an op->collator, and an op back
 * pointer to a WTI_CURSOR_LAYERED for CUR2S(). All of that we can stand up by hand.
 */

#include <catch2/catch.hpp>

#include "wiredtiger.h"
#include "wt_internal.h"
#include "../../utils.h"
#include "../../wrappers/connection_wrapper.h"

/*
 * Pull in the file-static helpers under test. The exported functions in cur_layered.c are declared
 * extern "C" through wt_internal.h, so the definitions compiled in here resolve against the same
 * symbols as the linked library.
 */
#include "src/cursor/cur_layered.c"

namespace {

/*
 * make_op --
 *     Build a WTI_CLAYERED_OP (and its backing WTI_CURSOR_LAYERED) on the stack. The helpers under
 *     test reach the session via CUR2S(op->clayered), so the only field the layered cursor needs is
 *     iface.session. Everything else is zeroed; collator NULL means default lexicographic ordering.
 */
void
make_op(WTI_CLAYERED_OP &op, WTI_CURSOR_LAYERED &clayered, WT_SESSION_IMPL *session)
{
    memset(&clayered, 0, sizeof(clayered));
    memset(&op, 0, sizeof(op));

    clayered.iface.session = &session->iface;

    op.clayered = &clayered;
    op.collator = nullptr;
}

/*
 * open_file_cursor --
 *     Create a row-store file: table and return an open cursor on it.
 */
WT_CURSOR *
open_file_cursor(WT_SESSION *session, const char *uri)
{
    REQUIRE(session->create(session, uri, "key_format=S,value_format=S") == 0);
    WT_CURSOR *cursor = nullptr;
    REQUIRE(session->open_cursor(session, uri, nullptr, nullptr, &cursor) == 0);
    return cursor;
}

/*
 * close_and_drop --
 *     Close a cursor and force-drop its backing table. connection_wrapper's cleanup only removes a
 *     fixed set of file names, so the tables created here must be dropped explicitly; otherwise the
 *     leftover files trip "file exists" warnings when Catch2 re-runs the test body per SECTION.
 */
void
close_and_drop(WT_SESSION *session, WT_CURSOR *cursor, const char *uri)
{
    REQUIRE(cursor->close(cursor) == 0);
    REQUIRE(session->drop(session, uri, "force=true") == 0);
}

} // namespace

TEST_CASE("Layered op: cursor compare orders constituent keys", "[layered][layered_op]")
{
    connection_wrapper conn(DB_HOME);
    WT_SESSION_IMPL *session_impl = conn.create_session();
    WT_SESSION *session = &session_impl->iface;

    const char *uri1 = "file:layered_op_c1.wt";
    const char *uri2 = "file:layered_op_c2.wt";
    WT_CURSOR *c1 = open_file_cursor(session, uri1);
    WT_CURSOR *c2 = open_file_cursor(session, uri2);

    WTI_CLAYERED_OP op;
    WTI_CURSOR_LAYERED clayered;
    make_op(op, clayered, session_impl);

    int cmp = 0;

    SECTION("first key sorts before second key")
    {
        c1->set_key(c1, "a");
        c2->set_key(c2, "b");
        REQUIRE(__clayered_cursor_compare(&op, c1, c2, &cmp) == 0);
        REQUIRE(cmp < 0);
    }

    SECTION("first key sorts after second key")
    {
        c1->set_key(c1, "b");
        c2->set_key(c2, "a");
        REQUIRE(__clayered_cursor_compare(&op, c1, c2, &cmp) == 0);
        REQUIRE(cmp > 0);
    }

    SECTION("equal keys compare equal")
    {
        c1->set_key(c1, "same-key");
        c2->set_key(c2, "same-key");
        REQUIRE(__clayered_cursor_compare(&op, c1, c2, &cmp) == 0);
        REQUIRE(cmp == 0);
    }

    SECTION("comparison uses byte order, not length")
    {
        /* "ab" sorts after "aa" even though "aaa" is longer, proving a lexicographic compare. */
        c1->set_key(c1, "ab");
        c2->set_key(c2, "aaa");
        REQUIRE(__clayered_cursor_compare(&op, c1, c2, &cmp) == 0);
        REQUIRE(cmp > 0);
    }

    SECTION("comparison leaves its inputs unchanged")
    {
        c1->set_key(c1, "a");
        c2->set_key(c2, "b");

        const WTI_CLAYERED_OP op_before = op;
        const WT_ITEM key1_before = c1->key;
        const WT_ITEM key2_before = c2->key;

        REQUIRE(__clayered_cursor_compare(&op, c1, c2, &cmp) == 0);

        /* The compare only reads its inputs; the op and both cursor keys must be untouched. */
        REQUIRE(memcmp(&op, &op_before, sizeof(op)) == 0);
        REQUIRE(memcmp(&c1->key, &key1_before, sizeof(c1->key)) == 0);
        REQUIRE(memcmp(&c2->key, &key2_before, sizeof(c2->key)) == 0);
    }

    close_and_drop(session, c1, uri1);
    close_and_drop(session, c2, uri2);
}

TEST_CASE(
  "Layered op: get_current selects the smaller/larger positioned cursor", "[layered][layered_op]")
{
    connection_wrapper conn(DB_HOME);
    WT_SESSION_IMPL *session_impl = conn.create_session();
    WT_SESSION *session = &session_impl->iface;

    /*
     * __clayered_get_current looks at op->ingest / op->stable, requiring them to be truly
     * positioned (WT_CURSTD_KEY_INT). We stand in two file: cursors for the ingest and stable
     * constituents, insert one key into each, and search to position them.
     */
    const char *ingest_uri = "file:layered_op_ingest.wt";
    const char *stable_uri = "file:layered_op_stable.wt";
    WT_CURSOR *ingest = open_file_cursor(session, ingest_uri);
    WT_CURSOR *stable = open_file_cursor(session, stable_uri);

    auto insert_one = [&](WT_CURSOR *cursor, const char *key, const char *value) {
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        REQUIRE(cursor->insert(cursor) == 0);
    };

    auto position_at = [&](WT_CURSOR *cursor, const char *key) {
        cursor->set_key(cursor, key);
        REQUIRE(cursor->search(cursor) == 0);
        REQUIRE(F_ISSET(cursor, WT_CURSTD_KEY_INT));
    };

    insert_one(ingest, "i", "ingest-value");
    insert_one(stable, "s", "stable-value");

    WTI_CLAYERED_OP op;
    WTI_CURSOR_LAYERED clayered;
    make_op(op, clayered, session_impl);
    op.ingest = ingest;
    op.stable = stable;

    SECTION("smallest picks the lower key (ingest 'i' < stable 's')")
    {
        position_at(ingest, "i");
        position_at(stable, "s");
        REQUIRE(__clayered_get_current(&op, true) == 0);
        REQUIRE(clayered.current_cursor == ingest);
    }

    SECTION("largest picks the higher key (stable 's' > ingest 'i')")
    {
        position_at(ingest, "i");
        position_at(stable, "s");
        REQUIRE(__clayered_get_current(&op, false) == 0);
        REQUIRE(clayered.current_cursor == stable);
    }

    SECTION("only stable positioned -> stable is current")
    {
        position_at(stable, "s");
        REQUIRE(__clayered_get_current(&op, true) == 0);
        REQUIRE(clayered.current_cursor == stable);
    }

    SECTION("no ingest cursor -> stable only")
    {
        position_at(ingest, "i");
        position_at(stable, "s");
        op.ingest = nullptr;
        REQUIRE(__clayered_get_current(&op, true) == 0);
        /* With no ingest cursor, stable is the only candidate even though 'i' < 's'. */
        REQUIRE(clayered.current_cursor == stable);
    }

    close_and_drop(session, ingest, ingest_uri);
    close_and_drop(session, stable, stable_uri);
}
