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

#include <catch2/catch.hpp>

#include "wiredtiger.h"
#include "../utils.h"
#include "../wrappers/connection_wrapper.h"

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

#endif
