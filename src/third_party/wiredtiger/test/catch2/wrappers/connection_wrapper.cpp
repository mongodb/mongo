/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#include <cstdio>
#include <stdexcept>

#include "wiredtiger.h"
#include "wt_internal.h"

#include "connection_wrapper.h"
#include "../utils.h"

connection_wrapper::connection_wrapper(const std::string &db_home, const char *cfg_str)
    : _conn_impl(nullptr), _conn(nullptr), _db_home(db_home), _cfg_str(cfg_str), _do_cleanup(true)
{
    struct stat sb;
    /*
     * Check if the DB Home exists and is a directory, without this the mkdir can fail on some
     * platforms (win).
     */
    if (stat(_db_home.c_str(), &sb) == 0) {
        if (!S_ISDIR(sb.st_mode)) {
            std::string errorMessage("Path exists and is not a directory: " + db_home);
            throw std::runtime_error(errorMessage);
        }
        /* We are happy that it is an existing directory. */
    } else {
        utils::throw_if_non_zero(mkdir(_db_home.c_str(), 0700));
    }
    utils::throw_if_non_zero(wiredtiger_open(_db_home.c_str(), nullptr, cfg_str, &_conn));
}

connection_wrapper::~connection_wrapper()
{
    utils::throw_if_non_zero(_conn->close(_conn, ""));

    if (_do_cleanup)
        utils::wiredtiger_cleanup(_db_home);
}

WT_SESSION_IMPL *
connection_wrapper::create_session(std::string cfg)
{
    WT_SESSION *sess;
    _conn->open_session(_conn, nullptr, cfg.c_str(), &sess);

    auto sess_impl = (WT_SESSION_IMPL *)sess;
    _conn_impl = S2C(sess_impl);

    return sess_impl;
}

WT_CONNECTION_IMPL *
connection_wrapper::get_wt_connection_impl() const
{
    return _conn_impl;
}

WT_CONNECTION *
connection_wrapper::get_wt_connection() const
{
    return _conn;
}
