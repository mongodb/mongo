/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef MODEL_UTIL_H
#define MODEL_UTIL_H

#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>
#include "model/core.h"
#include "model/data_value.h"
#include "wiredtiger.h"

namespace model {

/*
 * wiredtiger_connection_guard --
 *     Automatically close the connection on delete.
 */
class wiredtiger_connection_guard {

public:
    /*
     * wiredtiger_connection_guard::wiredtiger_connection_guard --
     *     Create a new instance of the guard.
     */
    inline wiredtiger_connection_guard(
      WT_CONNECTION *connection, const char *close_config = nullptr) noexcept
        : _connection(connection), _close_config(close_config == nullptr ? "" : close_config){};

    /*
     * wiredtiger_connection_guard::~wiredtiger_connection_guard --
     *     Destroy the guard.
     */
    inline ~wiredtiger_connection_guard()
    {
        if (_connection != nullptr)
            (void)_connection->close(_connection, _close_config.c_str());
    }

private:
    WT_CONNECTION *_connection;
    std::string _close_config;
};

/*
 * wiredtiger_cursor_guard --
 *     Automatically close the cursor on delete.
 */
class wiredtiger_cursor_guard {

public:
    /*
     * wiredtiger_cursor_guard::wiredtiger_cursor_guard --
     *     Create a new instance of the guard.
     */
    inline wiredtiger_cursor_guard(WT_CURSOR *cursor) noexcept : _cursor(cursor){};

    /*
     * wiredtiger_cursor_guard::~wiredtiger_cursor_guard --
     *     Destroy the guard.
     */
    inline ~wiredtiger_cursor_guard()
    {
        if (_cursor != nullptr)
            (void)_cursor->close(_cursor);
    }

private:
    WT_CURSOR *_cursor;
};

/*
 * wiredtiger_session_guard --
 *     Automatically close the session on delete.
 */
class wiredtiger_session_guard {

public:
    /*
     * wiredtiger_session_guard::wiredtiger_session_guard --
     *     Create a new instance of the guard.
     */
    inline wiredtiger_session_guard(WT_SESSION *session) noexcept : _session(session){};

    /*
     * wiredtiger_session_guard::~wiredtiger_session_guard --
     *     Destroy the guard.
     */
    inline ~wiredtiger_session_guard()
    {
        if (_session != nullptr)
            (void)_session->close(_session, nullptr);
    }

private:
    WT_SESSION *_session;
};

/*
 * config_map --
 *     A configuration map.
 */
class config_map {
    using value_t = std::variant<std::string, std::shared_ptr<config_map>>;

public:
    /*
     * config_map::from_string --
     *     Parse config map from a string.
     */
    static config_map from_string(const char *str, const char **end = NULL);

    /*
     * config_map::from_string --
     *     Parse config map from a string.
     */
    static inline config_map
    from_string(const std::string &str)
    {
        return from_string(str.c_str());
    }

    /*
     * config_map::contains --
     *     Check whether the config map contains the given key.
     */
    inline bool
    contains(const char *key) const noexcept
    {
        return _map.find(key) != _map.end();
    }

    /*
     * config_map::get_map --
     *     Get the corresponding config map value. Throw an exception on error.
     */
    inline std::shared_ptr<config_map> const
    get_map(const char *key)
    {
        return std::get<std::shared_ptr<config_map>>(_map.find(key)->second);
    }

    /*
     * config_map::get_string --
     *     Get the corresponding string value. Throw an exception on error.
     */
    inline std::string
    get_string(const char *key) const
    {
        return std::get<std::string>(_map.find(key)->second);
    }

    /*
     * config_map::get_uint64 --
     *     Get the corresponding integer value. Throw an exception on error.
     */
    inline uint64_t
    get_uint64(const char *key) const
    {
        std::istringstream stream(std::get<std::string>(_map.find(key)->second));
        uint64_t v;
        stream >> v;
        return v;
    }

private:
    /*
     * config_map::config_map --
     *     Create a new instance of the config map.
     */
    inline config_map() noexcept {};

private:
    std::unordered_map<std::string, value_t> _map;
};

/*
 * starts_with --
 *     Check whether the string has the given prefix. (C++ does not have this until C++20.)
 */
inline bool
starts_with(std::string_view str, const char *prefix)
{
    return str.compare(0, std::strlen(prefix), prefix) == 0;
}

/*
 * wt_cursor_insert --
 *     Insert into WiredTiger using the provided cursor.
 */
inline int
wt_cursor_insert(WT_CURSOR *cursor, const data_value &key, const data_value &value)
{
    set_wt_cursor_key(cursor, key);
    set_wt_cursor_value(cursor, value);
    return cursor->insert(cursor);
}

/*
 * wt_cursor_remove --
 *     Remove from WiredTiger using the provided cursor.
 */
inline int
wt_cursor_remove(WT_CURSOR *cursor, const data_value &key)
{
    set_wt_cursor_key(cursor, key);
    return cursor->remove(cursor);
}

/*
 * wt_cursor_search --
 *     Search in WiredTiger using the provided cursor.
 */
inline int
wt_cursor_search(WT_CURSOR *cursor, const data_value &key)
{
    set_wt_cursor_key(cursor, key);
    return cursor->search(cursor);
}

/*
 * wt_cursor_update --
 *     Update in WiredTiger using the provided cursor.
 */
inline int
wt_cursor_update(WT_CURSOR *cursor, const data_value &key, const data_value &value)
{
    set_wt_cursor_key(cursor, key);
    set_wt_cursor_value(cursor, value);
    return cursor->update(cursor);
}

/*
 * wt_list_tables --
 *     Get the list of WiredTiger tables.
 */
std::vector<std::string> wt_list_tables(WT_CONNECTION *conn);

} /* namespace model */
#endif
