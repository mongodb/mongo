/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifndef _WIN32

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch.hpp>

#include "wiredtiger.h"
#include "wt_internal.h"
#include "src/cursor/cur_layered_private.h"
#include "../utils.h"
#include "../wrappers/connection_wrapper.h"
#include "layered_disagg_utils.h"

static std::string
palite_conn_cfg(const char *palite_opts)
{
    std::string cfg = "create,statistics=(all),";
    cfg += "extensions=[./ext/page_log/palite/libwiredtiger_palite.so";
    if (palite_opts != nullptr && palite_opts[0] != '\0') {
        cfg += "=(config=\"(";
        cfg += palite_opts;
        cfg += ")\")";
    }
    cfg += "],disaggregated=(role=leader,page_log=palite,lose_all_my_data=true)";
    return cfg;
}

static void
free_results(WT_ITEM *results, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        std::free(results[i].mem);
        results[i] = {};
    }
}

static WT_ITEM
item_from_string(const char *s)
{
    WT_ITEM buf{};
    buf.data = s;
    buf.size = std::strlen(s);
    return buf;
}

TEST_CASE("Palite victim cache is off by default", "[palite_victim_cache]")
{
    connection_wrapper conn(DB_HOME, palite_conn_cfg("").c_str());
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_SESSION *session = (WT_SESSION *)conn.create_session();

    WT_PAGE_LOG *page_log = nullptr;
    REQUIRE(wt_conn->get_page_log(wt_conn, "palite", &page_log) == 0);

    WT_PAGE_LOG_HANDLE *handle = nullptr;
    REQUIRE(page_log->pl_open_handle(page_log, session, 1, &handle) == 0);
    REQUIRE(handle->plh_cache_available != nullptr);
    REQUIRE_FALSE(handle->plh_cache_available(handle, session));

    REQUIRE(handle->plh_close(handle, session) == 0);
    REQUIRE(page_log->terminate(page_log, session) == 0);
}

TEST_CASE(
  "Palite victim cache get is not reused and put drops the cached copy", "[palite_victim_cache]")
{
    connection_wrapper conn(DB_HOME, palite_conn_cfg("victim_cache_max_entries=10000").c_str());
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_SESSION *session = (WT_SESSION *)conn.create_session();

    WT_PAGE_LOG *page_log = nullptr;
    REQUIRE(wt_conn->get_page_log(wt_conn, "palite", &page_log) == 0);

    WT_PAGE_LOG_HANDLE *handle = nullptr;
    REQUIRE(page_log->pl_open_handle(page_log, session, 1, &handle) == 0);
    REQUIRE(handle->plh_cache_available(handle, session));

    const uint64_t page_id = 20;
    const char *store_bytes = "sqlite-page";
    const char *cache_bytes = "cached-page";
    WT_ITEM store_buf = item_from_string(store_bytes);
    WT_ITEM cache_buf = item_from_string(cache_bytes);

    WT_PAGE_LOG_PUT_ARGS put_args{};
    REQUIRE(handle->plh_put(handle, session, page_id, 0, &put_args, &store_buf) == 0);
    const uint64_t lsn = put_args.lsn;
    REQUIRE(lsn > 0);

    WT_PAGE_LOG_PUT_ARGS cache_args{};
    cache_args.lsn = lsn;
    cache_args.backlink_lsn = 7;
    cache_args.base_lsn = 3;
    cache_args.backlink_checkpoint_id = 11;
    cache_args.base_checkpoint_id = 13;
    REQUIRE(handle->plh_cache_put(handle, session, page_id, 99, &cache_args, &cache_buf) == 0);
    REQUIRE(handle->plh_cache_has(handle, session, page_id, 0, &cache_args) == 0);

    WT_ITEM results[4]{};
    uint32_t n = 4;
    WT_PAGE_LOG_GET_ARGS bypass_args{};
    bypass_args.lsn = lsn;
    bypass_args.flags = WT_PAGE_LOG_CACHE_BYPASS;
    REQUIRE(handle->plh_get(handle, session, page_id, 0, &bypass_args, results, &n) == 0);
    REQUIRE(n == 1);
    REQUIRE(results[0].size == std::strlen(store_bytes));
    REQUIRE(std::memcmp(results[0].data, store_bytes, results[0].size) == 0);
    free_results(results, n);
    REQUIRE(handle->plh_cache_has(handle, session, page_id, 0, &cache_args) == 0);

    n = 4;
    WT_PAGE_LOG_GET_ARGS get_args{};
    get_args.lsn = lsn;
    REQUIRE(handle->plh_get(handle, session, page_id, 0, &get_args, results, &n) == 0);
    REQUIRE(n == 1);
    REQUIRE(results[0].size == std::strlen(cache_bytes));
    REQUIRE(std::memcmp(results[0].data, cache_bytes, results[0].size) == 0);
    REQUIRE(get_args.backlink_lsn == 7);
    REQUIRE(get_args.base_lsn == 3);
    REQUIRE(get_args.backlink_checkpoint_id == 11);
    REQUIRE(get_args.base_checkpoint_id == 13);
    free_results(results, n);

    /* Second get reads from the store. */
    REQUIRE(handle->plh_cache_has(handle, session, page_id, 0, &cache_args) == WT_NOTFOUND);

    n = 4;
    get_args = {};
    get_args.lsn = lsn;
    REQUIRE(handle->plh_get(handle, session, page_id, 0, &get_args, results, &n) == 0);
    REQUIRE(n == 1);
    REQUIRE(results[0].size == std::strlen(store_bytes));
    REQUIRE(std::memcmp(results[0].data, store_bytes, results[0].size) == 0);
    free_results(results, n);

    /* A later put of the same page drops the cached copy. */
    REQUIRE(handle->plh_cache_put(handle, session, page_id, 0, &cache_args, &cache_buf) == 0);
    REQUIRE(handle->plh_cache_has(handle, session, page_id, 0, &cache_args) == 0);
    WT_PAGE_LOG_PUT_ARGS erase_args{};
    erase_args.backlink_lsn = lsn;
    REQUIRE(handle->plh_put(handle, session, page_id, 0, &erase_args, &store_buf) == 0);
    REQUIRE(handle->plh_cache_has(handle, session, page_id, 0, &cache_args) == WT_NOTFOUND);

    REQUIRE(handle->plh_close(handle, session) == 0);
    REQUIRE(page_log->terminate(page_log, session) == 0);
}

TEST_CASE(
  "Palite victim cache drops an entry when the handle is at capacity", "[palite_victim_cache]")
{
    connection_wrapper conn(DB_HOME, palite_conn_cfg("victim_cache_max_entries=2").c_str());
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_SESSION *session = (WT_SESSION *)conn.create_session();

    WT_PAGE_LOG *page_log = nullptr;
    REQUIRE(wt_conn->get_page_log(wt_conn, "palite", &page_log) == 0);

    WT_PAGE_LOG_HANDLE *handle = nullptr;
    REQUIRE(page_log->pl_open_handle(page_log, session, 1, &handle) == 0);

    const char *bytes = "cached-page";
    WT_ITEM buf = item_from_string(bytes);
    WT_PAGE_LOG_PUT_ARGS cache_args[3]{};
    for (uint64_t page_id = 1; page_id <= 3; ++page_id) {
        WT_PAGE_LOG_PUT_ARGS put_args{};
        REQUIRE(handle->plh_put(handle, session, page_id, 0, &put_args, &buf) == 0);
        cache_args[page_id - 1].lsn = put_args.lsn;
        REQUIRE(
          handle->plh_cache_put(handle, session, page_id, 0, &cache_args[page_id - 1], &buf) == 0);
    }

    uint32_t cached = 0;
    for (uint64_t page_id = 1; page_id <= 3; ++page_id) {
        if (handle->plh_cache_has(handle, session, page_id, 0, &cache_args[page_id - 1]) == 0)
            ++cached;
    }
    REQUIRE(cached == 2);

    REQUIRE(handle->plh_close(handle, session) == 0);
    REQUIRE(page_log->terminate(page_log, session) == 0);
}

TEST_CASE("Palite victim cache discard drops the cached copy", "[palite_victim_cache]")
{
    connection_wrapper conn(DB_HOME, palite_conn_cfg("victim_cache_max_entries=10000").c_str());
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_SESSION *session = (WT_SESSION *)conn.create_session();

    WT_PAGE_LOG *page_log = nullptr;
    REQUIRE(wt_conn->get_page_log(wt_conn, "palite", &page_log) == 0);

    WT_PAGE_LOG_HANDLE *handle = nullptr;
    REQUIRE(page_log->pl_open_handle(page_log, session, 1, &handle) == 0);

    const uint64_t page_id = 22;
    const char *cache_bytes = "cached-page";
    WT_ITEM cache_buf = item_from_string(cache_bytes);

    WT_PAGE_LOG_PUT_ARGS put_args{};
    REQUIRE(handle->plh_put(handle, session, page_id, 0, &put_args, &cache_buf) == 0);
    const uint64_t lsn = put_args.lsn;

    WT_PAGE_LOG_PUT_ARGS cache_args{};
    cache_args.lsn = lsn;
    REQUIRE(handle->plh_cache_put(handle, session, page_id, 0, &cache_args, &cache_buf) == 0);
    REQUIRE(handle->plh_cache_has(handle, session, page_id, 0, &cache_args) == 0);

    WT_PAGE_LOG_DISCARD_ARGS discard_args{};
    discard_args.backlink_lsn = lsn;
    discard_args.base_lsn = lsn;
    REQUIRE(handle->plh_discard(handle, session, page_id, 0, &discard_args) == 0);
    REQUIRE(handle->plh_cache_has(handle, session, page_id, 0, &cache_args) == WT_NOTFOUND);

    /* The image at that LSN is still in the store. */
    WT_ITEM results[1]{};
    uint32_t n = 1;
    WT_PAGE_LOG_GET_ARGS get_args{};
    get_args.lsn = lsn;
    REQUIRE(handle->plh_get(handle, session, page_id, 0, &get_args, results, &n) == 0);
    REQUIRE(n == 1);
    REQUIRE(results[0].size == std::strlen(cache_bytes));
    REQUIRE(std::memcmp(results[0].data, cache_bytes, results[0].size) == 0);
    free_results(results, n);
    REQUIRE(handle->plh_cache_has(handle, session, page_id, 0, &cache_args) == WT_NOTFOUND);

    REQUIRE(handle->plh_close(handle, session) == 0);
    REQUIRE(page_log->terminate(page_log, session) == 0);
}

/*
 * The byte budget is shared by every handle, so two handles together must not exceed it. This is
 * what lets a caller size the cache without knowing how many handles a run will open.
 */
TEST_CASE("Palite victim cache byte budget is shared across handles", "[palite_victim_cache]")
{
    /* One megabyte for the whole page log, no entry limit. */
    connection_wrapper conn(DB_HOME, palite_conn_cfg("victim_cache_size_mb=1").c_str());
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_SESSION *session = (WT_SESSION *)conn.create_session();

    WT_PAGE_LOG *page_log = nullptr;
    REQUIRE(wt_conn->get_page_log(wt_conn, "palite", &page_log) == 0);

    WT_PAGE_LOG_HANDLE *first = nullptr, *second = nullptr;
    REQUIRE(page_log->pl_open_handle(page_log, session, 1, &first) == 0);
    REQUIRE(page_log->pl_open_handle(page_log, session, 2, &second) == 0);

    /* A byte budget alone makes the cache available. */
    REQUIRE(first->plh_cache_available(first, session));
    REQUIRE(second->plh_cache_available(second, session));

    std::vector<uint8_t> page(64 * 1024, 0xab);
    WT_ITEM buf;
    std::memset(&buf, 0, sizeof(buf));
    buf.data = page.data();
    buf.size = page.size();

    /* Offer each handle a megabyte of pages; between them they may only hold a megabyte. */
    const int per_handle = 16;
    WT_PAGE_LOG_HANDLE *handles[2] = {first, second};
    for (auto &handle : handles)
        for (int i = 0; i < per_handle; ++i) {
            WT_PAGE_LOG_PUT_ARGS args;
            std::memset(&args, 0, sizeof(args));
            args.lsn = (uint64_t)i + 1;
            REQUIRE(handle->plh_cache_put(handle, session, (uint64_t)i, 0, &args, &buf) == 0);
        }

    int cached = 0;
    for (auto &handle : handles)
        for (int i = 0; i < per_handle; ++i) {
            WT_PAGE_LOG_PUT_ARGS args;
            std::memset(&args, 0, sizeof(args));
            args.lsn = (uint64_t)i + 1;
            if (handle->plh_cache_has(handle, session, (uint64_t)i, 0, &args) == 0)
                ++cached;
        }

    /* 1MB of 64KB pages, so at most 16 between the two handles rather than 16 each. */
    REQUIRE(cached > 0);
    REQUIRE(cached <= per_handle);

    REQUIRE(first->plh_close(first, session) == 0);
    REQUIRE(second->plh_close(second, session) == 0);
    REQUIRE(page_log->terminate(page_log, session) == 0);
}

/*
 * A handle that cannot win the room gives up nothing. Discarding what it already holds would cost a
 * connection its warm pages for a put that was never going to succeed.
 */
TEST_CASE("Palite victim cache keeps its entries when it cannot make room", "[palite_victim_cache]")
{
    connection_wrapper conn(DB_HOME, palite_conn_cfg("victim_cache_size_mb=1").c_str());
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_SESSION *session = (WT_SESSION *)conn.create_session();

    WT_PAGE_LOG *page_log = nullptr;
    REQUIRE(wt_conn->get_page_log(wt_conn, "palite", &page_log) == 0);

    WT_PAGE_LOG_HANDLE *hog = nullptr, *small = nullptr;
    REQUIRE(page_log->pl_open_handle(page_log, session, 1, &hog) == 0);
    REQUIRE(page_log->pl_open_handle(page_log, session, 2, &small) == 0);

    std::vector<uint8_t> big(64 * 1024, 0xab), huge(96 * 1024, 0xef), tiny(1024, 0xcd);
    WT_ITEM big_buf, huge_buf, tiny_buf;
    std::memset(&big_buf, 0, sizeof(big_buf));
    std::memset(&huge_buf, 0, sizeof(huge_buf));
    std::memset(&tiny_buf, 0, sizeof(tiny_buf));
    big_buf.data = big.data();
    big_buf.size = big.size();
    huge_buf.data = huge.data();
    huge_buf.size = huge.size();
    tiny_buf.data = tiny.data();
    tiny_buf.size = tiny.size();

    /* The small handle caches one page first, so it has something to lose. */
    WT_PAGE_LOG_PUT_ARGS tiny_args;
    std::memset(&tiny_args, 0, sizeof(tiny_args));
    tiny_args.lsn = 1;
    REQUIRE(small->plh_cache_put(small, session, 100, 0, &tiny_args, &tiny_buf) == 0);
    REQUIRE(small->plh_cache_has(small, session, 100, 0, &tiny_args) == 0);

    /* The other handle then takes the rest of the budget. */
    for (int i = 0; i < 15; ++i) {
        WT_PAGE_LOG_PUT_ARGS args;
        std::memset(&args, 0, sizeof(args));
        args.lsn = (uint64_t)i + 1;
        REQUIRE(hog->plh_cache_put(hog, session, (uint64_t)i, 0, &args, &big_buf) == 0);
    }

    /* A page the small handle could not fit even by discarding everything it holds. */
    WT_PAGE_LOG_PUT_ARGS refused;
    std::memset(&refused, 0, sizeof(refused));
    refused.lsn = 2;
    REQUIRE(small->plh_cache_put(small, session, 101, 0, &refused, &huge_buf) == 0);
    REQUIRE(small->plh_cache_has(small, session, 101, 0, &refused) == WT_NOTFOUND);

    REQUIRE(small->plh_cache_has(small, session, 100, 0, &tiny_args) == 0);

    REQUIRE(hog->plh_close(hog, session) == 0);
    REQUIRE(small->plh_close(small, session) == 0);
    REQUIRE(page_log->terminate(page_log, session) == 0);
}

/*
 * Closing a handle must give its share of the budget back, or a connection that opens and closes
 * handles over time would charge itself until nothing could be cached again.
 */
TEST_CASE("Palite victim cache budget is released when a handle closes", "[palite_victim_cache]")
{
    connection_wrapper conn(DB_HOME, palite_conn_cfg("victim_cache_size_mb=1").c_str());
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_SESSION *session = (WT_SESSION *)conn.create_session();

    WT_PAGE_LOG *page_log = nullptr;
    REQUIRE(wt_conn->get_page_log(wt_conn, "palite", &page_log) == 0);

    std::vector<uint8_t> page(64 * 1024, 0xab);
    WT_ITEM buf;
    std::memset(&buf, 0, sizeof(buf));
    buf.data = page.data();
    buf.size = page.size();

    /* Fill the budget from a handle, close it, and do the same again. */
    int cached[3] = {0, 0, 0};
    for (int round = 0; round < 3; ++round) {
        WT_PAGE_LOG_HANDLE *handle = nullptr;
        REQUIRE(page_log->pl_open_handle(page_log, session, 1, &handle) == 0);
        for (int i = 0; i < 16; ++i) {
            WT_PAGE_LOG_PUT_ARGS args;
            std::memset(&args, 0, sizeof(args));
            args.lsn = (uint64_t)i + 1;
            REQUIRE(handle->plh_cache_put(handle, session, (uint64_t)i, 0, &args, &buf) == 0);
        }
        for (int i = 0; i < 16; ++i) {
            WT_PAGE_LOG_PUT_ARGS args;
            std::memset(&args, 0, sizeof(args));
            args.lsn = (uint64_t)i + 1;
            if (handle->plh_cache_has(handle, session, (uint64_t)i, 0, &args) == 0)
                ++cached[round];
        }
        REQUIRE(handle->plh_close(handle, session) == 0);
    }

    /* A leaked budget would starve the later rounds. */
    REQUIRE(cached[0] > 0);
    REQUIRE(cached[1] == cached[0]);
    REQUIRE(cached[2] == cached[0]);

    REQUIRE(page_log->terminate(page_log, session) == 0);
}

/*
 * The two bounds are independent, so the entry count has to hold even when the byte budget is far
 * too large to ever bind.
 */
TEST_CASE(
  "Palite victim cache respects the entry count under a large byte budget", "[palite_victim_cache]")
{
    connection_wrapper conn(DB_HOME,
      palite_conn_cfg("victim_cache_max_entries=1000,victim_cache_size_mb=1048576").c_str());
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_SESSION *session = (WT_SESSION *)conn.create_session();

    WT_PAGE_LOG *page_log = nullptr;
    REQUIRE(wt_conn->get_page_log(wt_conn, "palite", &page_log) == 0);

    WT_PAGE_LOG_HANDLE *handle = nullptr;
    REQUIRE(page_log->pl_open_handle(page_log, session, 1, &handle) == 0);

    /* Tiny pages, so a terabyte of budget can never be what stops us. */
    std::vector<uint8_t> page(64, 0xab);
    WT_ITEM buf;
    std::memset(&buf, 0, sizeof(buf));
    buf.data = page.data();
    buf.size = page.size();

    const int limit = 1000;
    for (int i = 0; i < limit + 1; ++i) {
        WT_PAGE_LOG_PUT_ARGS args;
        std::memset(&args, 0, sizeof(args));
        args.lsn = (uint64_t)i + 1;
        REQUIRE(handle->plh_cache_put(handle, session, (uint64_t)i, 0, &args, &buf) == 0);
    }

    int cached = 0;
    for (int i = 0; i < limit + 1; ++i) {
        WT_PAGE_LOG_PUT_ARGS args;
        std::memset(&args, 0, sizeof(args));
        args.lsn = (uint64_t)i + 1;
        if (handle->plh_cache_has(handle, session, (uint64_t)i, 0, &args) == 0)
            ++cached;
    }
    REQUIRE(cached == limit);

    REQUIRE(handle->plh_close(handle, session) == 0);
    REQUIRE(page_log->terminate(page_log, session) == 0);
}

/*
 * The last put under a key wins, even when the new image is too large to cache. Declining the
 * replacement must not leave the previous one behind.
 */
TEST_CASE("Palite victim cache drops the old copy when the replacement is too large",
  "[palite_victim_cache]")
{
    connection_wrapper conn(DB_HOME, palite_conn_cfg("victim_cache_size_mb=1").c_str());
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_SESSION *session = (WT_SESSION *)conn.create_session();

    WT_PAGE_LOG *page_log = nullptr;
    REQUIRE(wt_conn->get_page_log(wt_conn, "palite", &page_log) == 0);

    WT_PAGE_LOG_HANDLE *handle = nullptr;
    REQUIRE(page_log->pl_open_handle(page_log, session, 1, &handle) == 0);

    WT_PAGE_LOG_PUT_ARGS args;
    std::memset(&args, 0, sizeof(args));
    args.lsn = 1;

    std::vector<uint8_t> small(1024, 0x11);
    WT_ITEM buf;
    std::memset(&buf, 0, sizeof(buf));
    buf.data = small.data();
    buf.size = small.size();
    REQUIRE(handle->plh_cache_put(handle, session, 1, 0, &args, &buf) == 0);
    REQUIRE(handle->plh_cache_has(handle, session, 1, 0, &args) == 0);

    /* Replace it under the same key with an image that cannot fit in the budget at all. */
    std::vector<uint8_t> huge(2 * 1024 * 1024, 0x22);
    buf.data = huge.data();
    buf.size = huge.size();
    REQUIRE(handle->plh_cache_put(handle, session, 1, 0, &args, &buf) == 0);
    REQUIRE(handle->plh_cache_has(handle, session, 1, 0, &args) != 0);

    REQUIRE(handle->plh_close(handle, session) == 0);
    REQUIRE(page_log->terminate(page_log, session) == 0);
}

static int64_t
victim_cache_stat(WT_SESSION *session, int stat)
{
    WT_CURSOR *cursor;
    const char *description, *printable;
    int64_t value;

    REQUIRE(session->open_cursor(session, "statistics:", nullptr, nullptr, &cursor) == 0);
    cursor->set_key(cursor, stat);
    REQUIRE(cursor->search(cursor) == 0);
    REQUIRE(cursor->get_value(cursor, &description, &printable, &value) == 0);
    REQUIRE(cursor->close(cursor) == 0);
    return value;
}

static uint64_t
victim_cache_durable_write_gen(
  WT_SESSION *session, WT_PAGE_LOG_HANDLE *handle, const WT_PAGE_BLOCK_META &meta)
{
    WT_ITEM results[WT_DELTA_LIMIT + 1]{};
    WT_PAGE_LOG_GET_ARGS args{};
    args.lsn = meta.disagg_lsn;
    F_SET(&args, WT_PAGE_LOG_CACHE_BYPASS);
    uint32_t count = WT_DELTA_LIMIT + 1;
    int ret = handle->plh_get(handle, session, meta.page_id, 0, &args, results, &count);
    bool valid = ret == 0 && count > 0 && count <= WT_DELTA_LIMIT + 1 &&
      results[count - 1].data != nullptr && results[count - 1].size >= WT_PAGE_HEADER_SIZE;
    WT_PAGE_HEADER header{};
    /* The newest delta carries the backing block's generation. */
    if (valid) {
        std::memcpy(&header, results[count - 1].data, WT_PAGE_HEADER_SIZE);
        __wt_page_header_byteswap(&header);
    }
    free_results(results, WT_DELTA_LIMIT + 1);
    REQUIRE(ret == 0);
    REQUIRE(valid);
    REQUIRE(args.lsn == meta.disagg_lsn);
    return header.write_gen;
}

TEST_CASE("Layered skip-write retains the backing generation in the victim cache",
  "[palite_victim_cache][layered_victim_cache_skip_write_gen]")
{
    std::string cfg = palite_conn_cfg("victim_cache_max_entries=10000");
    cfg += ",cache_size=2GB,precise_checkpoint=true,checkpoint_threads=1,";
    cfg += "page_delta=(delta_pct=1000,leaf_page_delta=true)";
    const char *home = DB_HOME "_victim_cache_skip_write_gen";
    testutil_recreate_dir(home);
    connection_wrapper conn(home, cfg.c_str());
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_SESSION *session = &conn.create_session("cache_cursors=false")->iface;
    WT_SESSION *read_session = &conn.create_session("cache_cursors=false")->iface;
    const char *uri = "layered:victim_cache_skip_write_gen";
    const std::string initial_value(1024, 'A'), final_value(1024, 'B');

    auto write_rows = [&](const char *timestamp, const std::string &value) {
        WT_CURSOR *cursor;
        REQUIRE(session->begin_transaction(session, nullptr) == 0);
        REQUIRE(session->open_cursor(session, uri, nullptr, nullptr, &cursor) == 0);
        for (unsigned i = 0; i < 10; ++i) {
            std::string row_key = std::to_string(i);
            cursor->set_key(cursor, row_key.c_str());
            cursor->set_value(cursor, value.c_str());
            REQUIRE(cursor->insert(cursor) == 0);
        }
        REQUIRE(cursor->close(cursor) == 0);
        REQUIRE(session->commit_transaction(session, timestamp) == 0);
    };
    auto stable_cursor = [](WT_CURSOR *cursor) {
        auto *layered = reinterpret_cast<WTI_CURSOR_LAYERED *>(cursor);
        REQUIRE(layered->stable_cursor != nullptr);
        REQUIRE(layered->current_cursor == layered->stable_cursor);
        auto *cbt = reinterpret_cast<WT_CURSOR_BTREE *>(layered->stable_cursor);
        REQUIRE(cbt->ref != nullptr);
        REQUIRE(F_ISSET(cbt->ref, WT_REF_FLAG_LEAF));
        REQUIRE(cbt->ref->page != nullptr);
        REQUIRE(cbt->ref->page->disagg_info != nullptr);
        return cbt;
    };

    REQUIRE(wt_conn->set_timestamp(wt_conn, "oldest_timestamp=1,stable_timestamp=1") == 0);
    REQUIRE(session->create(session, uri, "key_format=S,value_format=S,leaf_page_max=16KB") == 0);
    write_rows("commit_timestamp=a", initial_value);
    layered_disagg_leader_checkpoint(wt_conn, session, 10);

    WT_CURSOR *cursor;
    REQUIRE(
      read_session->open_cursor(read_session, uri, nullptr, "debug=(release_evict)", &cursor) == 0);
    cursor->set_key(cursor, "0");
    REQUIRE(cursor->search(cursor) == 0);
    REQUIRE(cursor->reset(cursor) == 0);
    cursor->set_key(cursor, "0");
    REQUIRE(cursor->search(cursor) == 0);
    WT_CURSOR_BTREE *cbt = stable_cursor(cursor);
    WT_PAGE *page = cbt->ref->page;
    REQUIRE(page->dsk != nullptr);
    REQUIRE(page->disagg_info->block_meta.delta_count == 0);
    uint64_t original_gen = page->dsk->write_gen;

    /* Keep the original image resident while checkpoint writes the first delta. */
    write_rows("commit_timestamp=14", final_value);
    layered_disagg_leader_checkpoint(wt_conn, session, 20);
    REQUIRE(cbt->ref->page == page);
    const WT_PAGE_BLOCK_META previous = page->disagg_info->block_meta;
    REQUIRE(previous.delta_count == 1);
    auto *block_disagg = reinterpret_cast<WT_BLOCK_DISAGG *>(CUR2BT(cbt)->bm->block);
    uint64_t durable_gen =
      victim_cache_durable_write_gen(read_session, block_disagg->plhandle, previous);
    REQUIRE(page->dsk->write_gen == original_gen);
    REQUIRE(original_gen < durable_gen);
    REQUIRE(page->modify != nullptr);
    REQUIRE(page->modify->rec_write_gen == durable_gen);
    REQUIRE_FALSE(__wt_page_is_modified(page));

    /* Dirty the stable leaf without introducing any new content to persist. */
    WT_SESSION_IMPL *cursor_session = CUR2S(cbt);
    int ret;
    WT_WITH_BTREE(cursor_session, CUR2BT(cbt), {
        ret = __wt_page_modify_init(cursor_session, page);
        if (ret == 0)
            __wt_page_modify_set(cursor_session, page);
    });
    REQUIRE(ret == 0);
    REQUIRE(__wt_page_is_modified(page));
    int64_t skips_before = victim_cache_stat(session, WT_STAT_CONN_REC_SKIP_WRITE);
    int64_t puts_before = victim_cache_stat(session, WT_STAT_CONN_BLOCK_CACHE_PUTS);
    REQUIRE(cursor->reset(cursor) == 0);
    REQUIRE(victim_cache_stat(session, WT_STAT_CONN_REC_SKIP_WRITE) > skips_before);

    /* Check the retained image before the clean eviction publishes it to the victim cache. */
    for (bool cached : {false, true}) {
        cursor->set_key(cursor, "0");
        REQUIRE(cursor->search(cursor) == 0);
        const char *value;
        REQUIRE(cursor->get_value(cursor, &value) == 0);
        REQUIRE(std::string(value) == final_value);
        cbt = stable_cursor(cursor);
        page = cbt->ref->page;
        REQUIRE(page->dsk != nullptr);
        const WT_PAGE_BLOCK_META &meta = page->disagg_info->block_meta;
        REQUIRE(meta.page_id == previous.page_id);
        REQUIRE(meta.disagg_lsn == previous.disagg_lsn);
        CAPTURE(cached, meta.page_id, meta.disagg_lsn, durable_gen, page->dsk->write_gen);
        REQUIRE(page->dsk->write_gen ==
          victim_cache_durable_write_gen(read_session, block_disagg->plhandle, meta));
        if (cached) {
            const auto *blk =
              static_cast<const WT_BLOCK_DISAGG_HEADER *>(WT_BLOCK_HEADER_REF(page->dsk));
            REQUIRE(F_ISSET(blk, WT_BLOCK_DISAGG_MODIFIED));
        } else
            REQUIRE(cursor->reset(cursor) == 0);
    }
    REQUIRE(victim_cache_stat(session, WT_STAT_CONN_BLOCK_CACHE_PUTS) > puts_before);
    REQUIRE(cursor->close(cursor) == 0);
}

#endif
