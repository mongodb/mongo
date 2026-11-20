/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wiredtiger.h"
#include "wt_internal.h"
#include "../../utils.h"
#include "../../wrappers/connection_wrapper.h"
#include "../../wrappers/item_wrapper.h"

static int
insert_key_value(WT_CURSOR *cursor, const char *key, const char *value)
{
    item_wrapper item_key(key);
    item_wrapper item_value(value);
    __wt_cursor_set_raw_key(cursor, item_key.get_item());
    __wt_cursor_set_raw_value(cursor, item_value.get_item());
    return cursor->insert(cursor);
}

static bool
require_get_key_value(WT_CURSOR *cursor, const char *expected_key, const char *expected_value)
{
    const char *key = nullptr;
    const char *value = nullptr;
    REQUIRE(cursor->get_key(cursor, &key) == 0);
    REQUIRE(cursor->get_value(cursor, &value) == 0);

    bool keys_match = strcmp(key, expected_key) == 0;
    bool values_match = strcmp(value, expected_value) == 0;
    REQUIRE(keys_match);
    REQUIRE(values_match);

    return keys_match && values_match;
}

static void
insert_sample_values(WT_CURSOR *cursor)
{
    REQUIRE(insert_key_value(cursor, "k1", "v1") == 0);
    REQUIRE(insert_key_value(cursor, "k2", "v2") == 0);
    REQUIRE(insert_key_value(cursor, "k3", "v3") == 0);
    REQUIRE(insert_key_value(cursor, "k4", "v4") == 0);
    REQUIRE(insert_key_value(cursor, "k5", "v5") == 0);
}

static void
check_next_raw_key_value_batch(WT_SESSION *session, const char *uri)
{
    WT_CURSOR *cursor = nullptr;
    REQUIRE(session->open_cursor(session, uri, nullptr, nullptr, &cursor) == 0);
    REQUIRE(session->begin_transaction(session, nullptr) == 0);
    insert_sample_values(cursor);
    REQUIRE(session->commit_transaction(session, "commit_timestamp=10") == 0);
    REQUIRE(cursor->reset(cursor) == 0);

    WT_ITEM key{}, value{};
    size_t n = 0;

    /* Reject invalid arguments. */
    REQUIRE(cursor->next_raw_key_value_batch(cursor, &key, &value, 0, &n) == EINVAL);
    REQUIRE(cursor->next_raw_key_value_batch(cursor, nullptr, nullptr, 1, &n) == EINVAL);

    const char *keys[] = {"k1", "k2", "k3", "k4", "k5"};
    const char *values[] = {"v1", "v2", "v3", "v4", "v5"};
    for (int i = 0; i < 5; i++) {
        n = 0;
        REQUIRE(cursor->next_raw_key_value_batch(cursor, &key, &value, 1, &n) == 0);
        REQUIRE(n >= 1);
        REQUIRE(strcmp(static_cast<const char *>(key.data), keys[i]) == 0);
        REQUIRE(strcmp(static_cast<const char *>(value.data), values[i]) == 0);
        REQUIRE(require_get_key_value(cursor, keys[i], values[i]));
    }
    REQUIRE(cursor->next_raw_key_value_batch(cursor, &key, &value, 1, &n) == WT_NOTFOUND);

    /* Only keys, and only values, are both acceptable. */
    REQUIRE(cursor->reset(cursor) == 0);
    REQUIRE(cursor->next_raw_key_value_batch(cursor, &key, nullptr, 1, &n) == 0);
    REQUIRE(strcmp(static_cast<const char *>(key.data), "k1") == 0);
    REQUIRE(cursor->next_raw_key_value_batch(cursor, nullptr, &value, 1, &n) == 0);
    REQUIRE(strcmp(static_cast<const char *>(value.data), "v2") == 0);

    REQUIRE(cursor->close(cursor) == 0);
}

TEST_CASE("next_raw_key_value_batch_table", "[cursor]")
{
    connection_wrapper conn(DB_HOME);
    WT_SESSION *session = &conn.create_session()->iface;
    REQUIRE(session->create(session, "table:t", "key_format=S,value_format=S") == 0);
    check_next_raw_key_value_batch(session, "table:t");
    REQUIRE(session->close(session, nullptr) == 0);
}

TEST_CASE("next_raw_key_value_batch_table_multiple_colgroups", "[cursor]")
{
    connection_wrapper conn(DB_HOME);
    WT_SESSION *session = &conn.create_session()->iface;
    REQUIRE(session->create(session, "table:cg",
              "key_format=S,value_format=SS,columns=(k,a,b),colgroups=(g1,g2)") == 0);
    REQUIRE(session->create(session, "colgroup:cg:g1", "columns=(a)") == 0);
    REQUIRE(session->create(session, "colgroup:cg:g2", "columns=(b)") == 0);

    WT_CURSOR *cursor = nullptr;
    REQUIRE(session->open_cursor(session, "table:cg", nullptr, nullptr, &cursor) == 0);
    cursor->set_key(cursor, "k1");
    cursor->set_value(cursor, "a1", "b1");
    REQUIRE(cursor->insert(cursor) == 0);
    REQUIRE(cursor->reset(cursor) == 0);

    /* The API currently only supports one colgroup. */
    WT_ITEM key{}, value{};
    size_t n = 0;
    REQUIRE(cursor->next_raw_key_value_batch(cursor, &key, &value, 1, &n) == ENOTSUP);

    REQUIRE(cursor->close(cursor) == 0);
    REQUIRE(session->close(session, nullptr) == 0);
}

TEST_CASE("next_raw_key_value_batch_file", "[cursor]")
{
    connection_wrapper conn(DB_HOME);
    WT_SESSION *session = &conn.create_session()->iface;
    REQUIRE(session->create(session, "file:f.wt", "key_format=S,value_format=S") == 0);
    check_next_raw_key_value_batch(session, "file:f.wt");
    REQUIRE(session->close(session, nullptr) == 0);
}

#ifndef _WIN32
#include "../../misc_tests/layered_disagg_utils.h"

TEST_CASE("next_raw_key_value_batch_layered", "[cursor]")
{
    connection_wrapper conn(DB_HOME, layered_disagg_build_cfg("leader").c_str());
    WT_SESSION *session = &conn.create_session()->iface;
    REQUIRE(session->create(session, "layered:l",
              "key_format=S,value_format=S, block_manager=disagg,type=layered") == 0);
    check_next_raw_key_value_batch(session, "layered:l");
    REQUIRE(session->close(session, nullptr) == 0);
}
#endif
