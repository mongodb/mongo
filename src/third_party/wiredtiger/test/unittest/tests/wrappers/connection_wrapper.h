/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <memory>
#include <string>

#include "wt_internal.h"

#ifdef _WIN32
#include "windows_shim.h"
#endif

/*
 * Prefer a "real" class over a mock class when you need a fully fleshed-out connection or session.
 * There's a speed cost to this, since it will write a bunch of files to disk during the test, which
 * also need to be removed.
 */
class connection_wrapper {
public:
    connection_wrapper(const std::string &db_home, const char *cfg_str = "create");
    ~connection_wrapper();

    /*
     * The memory backing the returned session is owned by the connection it was opened on, and gets
     * cleaned up when that connection is closed. Neither this class nor its users need to clean it
     * up.
     */
    WT_SESSION_IMPL *create_session(std::string cfg_str = "");

    WT_CONNECTION_IMPL *get_wt_connection_impl() const;
    WT_CONNECTION *get_wt_connection() const;

    void
    clear_do_cleanup()
    {
        _do_cleanup = false;
    };

private:
    WT_CONNECTION_IMPL *_conn_impl;
    WT_CONNECTION *_conn;
    std::string _db_home;
    const char *_cfg_str;
    bool _do_cleanup;
};
