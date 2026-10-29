/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <filesystem>

#include "wt_internal.h"
#include "../wrappers/connection_wrapper.h"
#include "../wrappers/mock_session.h"

static constexpr auto test_home = "WT_TEST.ref_addr_discard";

struct dhandle_guard {
    WT_SESSION_IMPL *session;
    WT_DATA_HANDLE *saved;

    ~dhandle_guard()
    {
        session->dhandle = saved;
    }
};

static void
init_generations(WT_SESSION_IMPL *session, WT_SESSION_IMPL *slot)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    memset(slot, 0, sizeof(*slot));
    conn->session_array.__array = slot;
    conn->session_array.size = 1;
    conn->session_array.cnt = 0;
    __wt_gen_init(session);
}

static void
init_ref_addr(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_ADDR *addr;

    memset(ref, 0, sizeof(*ref));
    REQUIRE(__wt_calloc_one(session, &addr) == 0);
    REQUIRE(__wt_calloc(session, 1, 16, &addr->block_cookie) == 0);
    addr->block_cookie_size = 16;
    ref->addr = addr;
}

static size_t
stash_live(WT_SESSION_IMPL *session)
{
    size_t i, n;

    auto *stash = &session->stash[WT_GEN_SPLIT];
    for (i = 0, n = 0; i < stash->cnt; ++i)
        if (stash->list[i].p != NULL)
            ++n;
    return (n);
}

static void
free_stash_list(WT_SESSION_IMPL *session)
{
    auto *stash = &session->stash[WT_GEN_SPLIT];
    __wt_free(session, stash->list);
    stash->cnt = stash->alloc = 0;
}

TEST_CASE(
  "Reference address discard: immediate free does not use generations", "[ref_addr_discard]")
{
    std::filesystem::remove_all(test_home);
    connection_wrapper wrapper(test_home, "create");
    WT_SESSION_IMPL *session = wrapper.create_session();
    WT_DATA_HANDLE dhandle{};
    WT_BTREE btree{};
    WT_PAGE_INDEX *pindex;
    WT_REF root{};
    size_t stashed;
    uint64_t split_gen;

    dhandle.handle = &btree;
    WT_DATA_HANDLE *saved_dhandle = session->dhandle;
    session->dhandle = &dhandle;
    dhandle_guard guard{session, saved_dhandle};

    REQUIRE(__wt_page_alloc(session, WT_PAGE_ROW_INT, 2, true, &root.page, 0) == 0);
    WT_INTL_INDEX_GET_SAFE(root.page, pindex);
    for (uint32_t i = 0; i < pindex->entries; ++i) {
        init_ref_addr(session, pindex->index[i]);
        F_SET(pindex->index[i], WT_REF_FLAG_LEAF);
    }

    split_gen = __wt_gen(session, WT_GEN_SPLIT);
    stashed = stash_live(session);

    __wt_ref_out_exclusive(session, &root);

    REQUIRE(root.page == nullptr);
    REQUIRE(__wt_gen(session, WT_GEN_SPLIT) == split_gen);
    REQUIRE(stash_live(session) == stashed);
}

TEST_CASE(
  "Reference address discard: ordinary free retains generation protection", "[ref_addr_discard]")
{
    std::shared_ptr<mock_session> mock = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = mock->get_wt_session_impl();
    WT_SESSION_IMPL slot;
    WT_REF ref;
    uint64_t split_gen;

    init_generations(session, &slot);
    init_ref_addr(session, &ref);
    split_gen = __wt_gen(session, WT_GEN_SPLIT);

    __wt_ref_addr_free(session, &ref);

    REQUIRE(ref.addr == nullptr);
    REQUIRE(__wt_gen(session, WT_GEN_SPLIT) == split_gen + 2);
    REQUIRE(stash_live(session) == 1);

    __wt_stash_discard(session);
    REQUIRE(stash_live(session) == 0);
    free_stash_list(session);
}
