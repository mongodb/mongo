/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Unit test for the leaf page-delta merge cleanup path. When initializing the per-delta merge state
 * fails early, the merge must report the failure and release whatever it allocated instead of
 * dereferencing a NULL merge-state pointer.
 */
#include <catch2/catch.hpp>
#include <cstdint>
#include "../wrappers/mock_session.h"

extern "C" {
#include "wt_internal.h"
}

namespace {

struct delta_state_cleanup_fixture {
    std::shared_ptr<mock_session> mock;
    WT_SESSION_IMPL *session;

    delta_state_cleanup_fixture() : mock(mock_session::build_test_mock_session())
    {
        mock->setup_block_manager_file_operations();
        session = mock->get_wt_session_impl();
        session->id = 0;

        WT_TXN_SHARED *txn_shared_list;
        REQUIRE(__wt_calloc(session, 1, sizeof(WT_TXN_SHARED), &txn_shared_list) == 0);
        S2C(session)->txn_global.txn_shared_list = txn_shared_list;
        REQUIRE(__wt_calloc(session, 1, sizeof(WT_TXN), &session->txn) == 0);
    }

    ~delta_state_cleanup_fixture()
    {
        __wt_free(session, session->txn);
        __wt_free(session, S2C(session)->txn_global.txn_shared_list);
    }
};

} // namespace

TEST_CASE_METHOD(delta_state_cleanup_fixture,
  "merge deltas cleans up after a delta merge-state allocation failure", "[page_delta_merge]")
{
    WT_ITEM delta, new_image;
    WT_CLEAR(delta);
    WT_CLEAR(new_image);
    REQUIRE(__wt_buf_init(session, &delta, 512) == 0);
    REQUIRE(__wt_buf_init(session, &new_image, 8192) == 0);

    /*
     * A delta_size whose product with the merge-state size overflows size_t can never be allocated.
     * The merge must report the failure and release the merge state -- here never allocated --
     * rather than dereferencing a NULL pointer during cleanup.
     */
    size_t delta_size = SIZE_MAX / sizeof(WTI_DELTA_LEAF_MERGE_STATE) + 1;

    int ret;
#ifdef HAVE_DIAGNOSTIC
    WT_TIME_AGGREGATE ta;
    ret = __wti_page_merge_deltas_with_base_image_leaf(
      session, &delta, delta_size, &new_image, nullptr, &ta);
#else
    ret = __wti_page_merge_deltas_with_base_image_leaf(
      session, &delta, delta_size, &new_image, nullptr);
#endif
    REQUIRE(ret != 0);

    __wt_buf_free(session, &delta);
    __wt_buf_free(session, &new_image);
}
