/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wiredtiger.h"
#include "../wrappers/mock_session.h"

int
mock_plh_open_handle(
  WT_PAGE_LOG *page_log, WT_SESSION *session, uint64_t table_id, WT_PAGE_LOG_HANDLE **plh)
{
    WT_PAGE_LOG_HANDLE *mock_log_handle;
    WT_UNUSED(page_log);
    WT_UNUSED(table_id);

    WT_RET(__wt_calloc_one((WT_SESSION_IMPL *)session, &mock_log_handle));
    *plh = mock_log_handle;
    return (0);
}

int
mock_terminate(WT_PAGE_LOG *page_log, WT_SESSION *session)
{
    __wt_free((WT_SESSION_IMPL *)session, page_log);
    return (0);
}

int
mock_plh_close(WT_PAGE_LOG_HANDLE *plh, WT_SESSION *session)
{
    WT_UNUSED(plh);
    WT_UNUSED(session);
    return (0);
}

void
setup_page_log_queue(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn_impl = S2C(session);
    WT_NAMED_PAGE_LOG *npl;

    /* Create a mock page log. */
    WT_PAGE_LOG *page_log;
    REQUIRE(__wt_calloc_one(session, &page_log) == 0);
    page_log->pl_open_handle = mock_plh_open_handle;
    page_log->terminate = mock_terminate;

    /* Create a named page log. */
    REQUIRE(__wt_calloc_one(session, &npl) == 0);
    npl->page_log = page_log;
    REQUIRE(__wt_strdup(session, "mock", &npl->name) == 0);

    /* Insert mock page log into mock connection. */
    TAILQ_INIT(&conn_impl->pagelogqh);
    TAILQ_INSERT_TAIL(&conn_impl->pagelogqh, npl, q);
}

TEST_CASE("Test disaggregated configuration logic", "[disagg_config]")
{
    std::shared_ptr<mock_session> session_mock = mock_session::build_test_mock_session();

    WT_CONNECTION_IMPL *conn_impl = session_mock->get_mock_connection()->get_wt_connection_impl();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();

    const char *disagg_cfg[] = {"disaggregated=(role=follower,page_log=mock)", NULL};

    REQUIRE(__wt_spin_init(session, &conn_impl->disaggregated_storage.copy_metadata_lock,
              "copy shared metadata") == 0);
    REQUIRE(__wt_spin_init(session, &conn_impl->api_lock, "api") == 0);

    /* Setup the page log queue. */
    setup_page_log_queue(session);

    SECTION("Test page log handle is constructed")
    {
        REQUIRE(__wti_disagg_conn_config(session, disagg_cfg, false) == WT_ERROR);
        REQUIRE(conn_impl->disaggregated_storage.page_log != nullptr);
        REQUIRE(conn_impl->disaggregated_storage.npage_log != nullptr);
        REQUIRE(conn_impl->disaggregated_storage.page_log_meta != nullptr);

        /*
         * The key provider log handle should not be constructed if no custom key provider is set.
         */
        REQUIRE(conn_impl->disaggregated_storage.page_log_key_provider == nullptr);

        free(conn_impl->disaggregated_storage.page_log);
        free(conn_impl->disaggregated_storage.page_log_meta);
        free(conn_impl->disaggregated_storage.page_log_key_provider);
    }

    SECTION("Test key provider handle is constructed")
    {
        WT_KEY_PROVIDER kp;
        conn_impl->key_provider = &kp; /* Dummy non-nullptr value. */

        REQUIRE(__wti_disagg_conn_config(session, disagg_cfg, false) == WT_ERROR);
        REQUIRE(conn_impl->disaggregated_storage.page_log != nullptr);
        REQUIRE(conn_impl->disaggregated_storage.npage_log != nullptr);
        REQUIRE(conn_impl->disaggregated_storage.page_log_meta != nullptr);
        REQUIRE(conn_impl->disaggregated_storage.page_log_key_provider != nullptr);

        free(conn_impl->disaggregated_storage.page_log);
        free(conn_impl->disaggregated_storage.page_log_meta);
        free(conn_impl->disaggregated_storage.page_log_key_provider);
    }

    SECTION("Test key provider and page log handle is destroyed")
    {
        WT_PAGE_LOG_HANDLE mock_page_log_handle;
        mock_page_log_handle.plh_close = mock_plh_close;
        conn_impl->disaggregated_storage.page_log_meta = &mock_page_log_handle;
        conn_impl->disaggregated_storage.page_log_key_provider = &mock_page_log_handle;
        REQUIRE(__wti_disagg_destroy(session) == 0);

        REQUIRE(conn_impl->disaggregated_storage.page_log_meta == nullptr);
        REQUIRE(conn_impl->disaggregated_storage.page_log_key_provider == nullptr);
    }
    REQUIRE(__wti_conn_remove_page_log(session) == 0);
    REQUIRE(__wti_layered_table_manager_destroy(session) == 0);

    __wt_spin_destroy(session, &conn_impl->disaggregated_storage.copy_metadata_lock);
    __wt_spin_destroy(session, &conn_impl->api_lock);
}
