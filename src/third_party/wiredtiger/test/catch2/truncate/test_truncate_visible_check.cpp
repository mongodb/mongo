/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Unit tests for __wt_truncate_delete_visible_check.
 *
 * The function checks whether a given key falls inside any committed fast-truncate
 * range on a layered table. These tests verify:
 *   - keys inside, outside, and at the boundaries of a single range
 *   - single-key ranges (start == stop)
 *   - multiple non-overlapping and overlapping ranges
 *   - the optional output pointer is filled on a match
 *   - the feature flag early-return path
 *   - lock discipline: read lock is always released
 */

#include <catch2/catch.hpp>
#include "wrappers/mock_session.h"

#include "wt_internal.h"

/*
 * Fixture: a minimal session and a hand-constructed WT_LAYERED_TABLE with an empty truncate list.
 * Individual tests call add_truncate_entry() to populate the list before exercising the function
 * under test.
 */
struct TruncVisibleCheckFixture {
    TruncVisibleCheckFixture() : mock(mock_session::build_test_mock_session())
    {
        session = mock->get_wt_session_impl();

        /* Allocate a zeroed transaction and shared list. */
        WT_TXN_SHARED *txn_shared_list;
        REQUIRE(__wt_calloc(session, 1, sizeof(WT_TXN_SHARED), &txn_shared_list) == 0);
        S2C(session)->txn_global.txn_shared_list = txn_shared_list;
        REQUIRE(__wt_calloc(session, 1, sizeof(WT_TXN), &session->txn) == 0);

        /* Build a minimal WT_LAYERED_TABLE with the required "layered:" name prefix. */
        REQUIRE(__wt_calloc(session, 1, sizeof(WT_LAYERED_TABLE), &layered_table) == 0);
        layered_table->iface.name = "layered:unit_test_table";
        TAILQ_INIT(&layered_table->truncateqh);
        REQUIRE(__wt_rwlock_init(session, &layered_table->truncate_lock) == 0);
        layered_table->collator = nullptr;
    }

    ~TruncVisibleCheckFixture()
    {
        /* Free every truncate entry (key data points to string literals; do not free it). */
        WT_TRUNCATE *t;
        while ((t = TAILQ_FIRST(&layered_table->truncateqh)) != nullptr) {
            TAILQ_REMOVE(&layered_table->truncateqh, t, q);
            __wt_free(session, t);
        }

        __wt_rwlock_destroy(session, &layered_table->truncate_lock);
        __wt_free(session, layered_table);

        __wt_free(session, session->txn);
        __wt_free(session, S2C(session)->txn_global.txn_shared_list);
    }

    /*
     * Add a truncate entry to the table's truncate list.
     *
     * WT_TXN_NONE makes entries globally visible without needing a transaction.
     */
    void
    add_truncate_entry(const char *start_key, const char *stop_key)
    {
        WT_TRUNCATE *t;
        REQUIRE(__wt_calloc(session, 1, sizeof(WT_TRUNCATE), &t) == 0);

        t->txn_id = WT_TXN_NONE;
        t->start_ts = WT_TS_NONE;
        t->durable_ts = WT_TS_NONE;

        t->start_key.data = start_key;
        t->start_key.size = strlen(start_key);

        t->stop_key.data = stop_key;
        t->stop_key.size = strlen(stop_key);

        TAILQ_INSERT_TAIL(&layered_table->truncateqh, t, q);
    }

    /* Build a stack-allocated WT_ITEM pointing at a string literal. */
    static WT_ITEM
    make_key(const char *s)
    {
        WT_ITEM k;
        WT_CLEAR(k);
        k.data = s;
        k.size = strlen(s);
        return k;
    }

    std::shared_ptr<mock_session> mock;
    WT_SESSION_IMPL *session;
    WT_LAYERED_TABLE *layered_table;
};

TEST_CASE_METHOD(TruncVisibleCheckFixture,
  "truncate_delete_visible_check: search key outside all truncate ranges", "[truncate]")
{
    SECTION("empty truncate list")
    {
        WT_ITEM key = make_key("key150");
        CHECK(
          __wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == WT_NOTFOUND);
    }

    SECTION("key before the truncated range")
    {
        add_truncate_entry("key100", "key200");
        WT_ITEM key = make_key("key050");
        CHECK(
          __wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == WT_NOTFOUND);
    }

    SECTION("key after the truncated range")
    {
        add_truncate_entry("key100", "key200");
        WT_ITEM key = make_key("key250");
        CHECK(
          __wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == WT_NOTFOUND);
    }

    SECTION("single-key range: key just before")
    {
        add_truncate_entry("key100", "key100");
        WT_ITEM key = make_key("key099");
        CHECK(
          __wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == WT_NOTFOUND);
    }

    SECTION("single-key range: key just after")
    {
        add_truncate_entry("key100", "key100");
        WT_ITEM key = make_key("key101");
        CHECK(
          __wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == WT_NOTFOUND);
    }

    SECTION("key between two non-overlapping ranges")
    {
        add_truncate_entry("key100", "key200");
        add_truncate_entry("key400", "key500");
        WT_ITEM key = make_key("key300");
        CHECK(
          __wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == WT_NOTFOUND);
    }
}

TEST_CASE_METHOD(TruncVisibleCheckFixture,
  "truncate_delete_visible_check: search key inside a committed truncate range", "[truncate]")
{
    SECTION("key strictly inside the range")
    {
        add_truncate_entry("key100", "key200");
        WT_ITEM key = make_key("key150");
        CHECK(__wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == 0);
    }

    SECTION("key at the start boundary (inclusive)")
    {
        add_truncate_entry("key100", "key200");
        WT_ITEM key = make_key("key100");
        CHECK(__wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == 0);
    }

    SECTION("key at the stop boundary (inclusive)")
    {
        add_truncate_entry("key100", "key200");
        WT_ITEM key = make_key("key200");
        CHECK(__wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == 0);
    }

    SECTION("single-key range, exact match")
    {
        add_truncate_entry("key100", "key100");
        WT_ITEM key = make_key("key100");
        CHECK(__wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == 0);
    }

    SECTION("key matched by the first of two non-overlapping ranges")
    {
        add_truncate_entry("key100", "key200");
        add_truncate_entry("key400", "key500");
        WT_ITEM key = make_key("key150");
        CHECK(__wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == 0);
    }

    SECTION("key matched by the second of two non-overlapping ranges")
    {
        add_truncate_entry("key100", "key200");
        add_truncate_entry("key400", "key500");
        WT_ITEM key = make_key("key450");
        CHECK(__wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == 0);
    }
}

TEST_CASE_METHOD(TruncVisibleCheckFixture,
  "truncate_delete_visible_check: search key inside overlapping truncate ranges", "[truncate]")
{
    SECTION("key in the overlap region of both ranges")
    {
        add_truncate_entry("key100", "key300");
        add_truncate_entry("key200", "key400");
        WT_ITEM key = make_key("key250");
        CHECK(__wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == 0);
    }

    SECTION("key in the first range only (outside the overlap)")
    {
        add_truncate_entry("key100", "key300");
        add_truncate_entry("key200", "key400");
        WT_ITEM key = make_key("key150");
        CHECK(__wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == 0);
    }

    SECTION("key in the second range only (outside the overlap)")
    {
        add_truncate_entry("key100", "key300");
        add_truncate_entry("key200", "key400");
        WT_ITEM key = make_key("key350");
        CHECK(__wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == 0);
    }
}

TEST_CASE_METHOD(
  TruncVisibleCheckFixture, "truncate_delete_visible_check: lock discipline", "[truncate]")
{
    /*
     * Verify that the read lock on the truncate list is always released before the function
     * returns. If the function leaked the read lock, a subsequent __wt_try_writelock would return
     * EBUSY; a successful acquisition proves no reader holds the lock.
     */
    SECTION("read lock is released after a match")
    {
        add_truncate_entry("key100", "key200");
        WT_ITEM key = make_key("key150");

        CHECK(__wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == 0);

        CHECK(__wt_try_writelock(session, &layered_table->truncate_lock) == 0);
        __wt_writeunlock(session, &layered_table->truncate_lock);
    }

    SECTION("read lock is released after a miss")
    {
        add_truncate_entry("key100", "key200");
        WT_ITEM key = make_key("key050");

        CHECK(
          __wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == WT_NOTFOUND);

        CHECK(__wt_try_writelock(session, &layered_table->truncate_lock) == 0);
        __wt_writeunlock(session, &layered_table->truncate_lock);
    }

    SECTION("read lock is released when the truncate list is empty")
    {
        WT_ITEM key = make_key("key150");

        CHECK(
          __wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == WT_NOTFOUND);

        CHECK(__wt_try_writelock(session, &layered_table->truncate_lock) == 0);
        __wt_writeunlock(session, &layered_table->truncate_lock);
    }
}

TEST_CASE_METHOD(
  TruncVisibleCheckFixture, "truncate_delete_visible_check: output parameter tp", "[truncate]")
{
    SECTION("When tp is null ensure function does not crash on a match")
    {
        add_truncate_entry("key100", "key200");
        WT_ITEM key = make_key("key150");
        CHECK(__wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == 0);
    }

    SECTION("tp is set to the matching entry when key is deleted")
    {
        add_truncate_entry("key100", "key200");
        WT_ITEM key = make_key("key150");
        WT_TRUNCATE *tp = nullptr;
        REQUIRE(__wt_truncate_delete_visible_check(session, layered_table, &key, &tp) == 0);
        REQUIRE(tp != nullptr);
        CHECK(strncmp((const char *)tp->start_key.data, "key100", tp->start_key.size) == 0);
        CHECK(strncmp((const char *)tp->stop_key.data, "key200", tp->stop_key.size) == 0);
    }

    SECTION("tp is null, when there is a miss")
    {
        add_truncate_entry("key100", "key200");
        WT_ITEM key = make_key("key050");
        WT_TRUNCATE *tp = nullptr;
        CHECK(__wt_truncate_delete_visible_check(session, layered_table, &key, &tp) == WT_NOTFOUND);
        CHECK(tp == nullptr);
    }
}

TEST_CASE_METHOD(
  TruncVisibleCheckFixture, "truncate_delete_visible_check: slow truncate mode", "[truncate]")
{
    /*
     * When disagg_slow_truncate_2026 is true the function must return WT_NOTFOUND immediately
     * without consulting the truncate list
     */
    SECTION("returns WT_NOTFOUND even when a matching entry exists")
    {
        add_truncate_entry("key100", "key200");
        __wt_process.disagg_slow_truncate_2026 = true;
        WT_ITEM key = make_key("key150");
        CHECK(
          __wt_truncate_delete_visible_check(session, layered_table, &key, nullptr) == WT_NOTFOUND);
        __wt_process.disagg_slow_truncate_2026 = false;
    }
}
