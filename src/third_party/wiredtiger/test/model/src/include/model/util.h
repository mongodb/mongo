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

#pragma once

#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
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
#include "model/kv_transaction.h"
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
     * wiredtiger_connection_guard::wiredtiger_connection_guard --
     *     Delete the copy constructor.
     */
    wiredtiger_connection_guard(const wiredtiger_connection_guard &) = delete;

    /*
     * wiredtiger_connection_guard::~wiredtiger_connection_guard --
     *     Destroy the guard.
     */
    inline ~wiredtiger_connection_guard()
    {
        if (_connection != nullptr)
            (void)_connection->close(_connection, _close_config.c_str());
    }

    /*
     * wiredtiger_connection_guard::operator= --
     *     Delete the assignment operator.
     */
    wiredtiger_connection_guard &operator=(const wiredtiger_connection_guard &) = delete;

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
     * wiredtiger_cursor_guard::wiredtiger_cursor_guard --
     *     Delete the copy constructor.
     */
    wiredtiger_cursor_guard(const wiredtiger_cursor_guard &) = delete;

    /*
     * wiredtiger_cursor_guard::~wiredtiger_cursor_guard --
     *     Destroy the guard.
     */
    inline ~wiredtiger_cursor_guard()
    {
        if (_cursor != nullptr)
            (void)_cursor->close(_cursor);
    }

    /*
     * wiredtiger_cursor_guard::operator= --
     *     Delete the assignment operator.
     */
    wiredtiger_cursor_guard &operator=(const wiredtiger_cursor_guard &) = delete;

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
     * wiredtiger_session_guard::wiredtiger_session_guard --
     *     Delete the copy constructor.
     */
    wiredtiger_session_guard(const wiredtiger_session_guard &) = delete;

    /*
     * wiredtiger_session_guard::~wiredtiger_session_guard --
     *     Destroy the guard.
     */
    inline ~wiredtiger_session_guard()
    {
        if (_session == nullptr)
            return;
        (void)_session->close(_session, nullptr);
    }

    /*
     * wiredtiger_session_guard::operator= --
     *     Delete the assignment operator.
     */
    wiredtiger_session_guard &operator=(const wiredtiger_session_guard &) = delete;

private:
    WT_SESSION *_session;
};

/*
 * kv_transaction_guard --
 *     Automatically commit a model transaction at the end of the function block, or roll it back if
 *     need be.
 */
class kv_transaction_guard {

public:
    /*
     * kv_transaction_guard::kv_transaction_guard --
     *     Create a new instance of the guard.
     */
    inline kv_transaction_guard(kv_transaction_ptr txn,
      timestamp_t commit_timestamp = k_timestamp_none,
      timestamp_t durable_timestamp = k_timestamp_none) noexcept
        : _txn(std::move(txn)), _commit_timestamp(commit_timestamp),
          _durable_timestamp(durable_timestamp){};

    /*
     * kv_transaction_guard::kv_transaction_guard --
     *     Delete the copy constructor.
     */
    kv_transaction_guard(const kv_transaction_guard &) = delete;

    /*
     * kv_transaction_guard::~kv_transaction_guard --
     *     Destroy the guard.
     */
    inline ~kv_transaction_guard()
    {
        if (!_txn)
            return;
        try {
            if (_txn->failed())
                _txn->rollback();
            else
                _txn->commit(_commit_timestamp, _durable_timestamp);
        } catch (std::exception &e) {
            /*
             * We cannot propagate exceptions from a destructor, so just print a warning. Exceptions
             * at this point here are exceedingly rare.
             */
            std::cerr << "Error while finishing a transaction: " << e.what() << std::endl;
        }
    }

    /*
     * kv_transaction_guard::operator= --
     *     Delete the assignment operator.
     */
    kv_transaction_guard &operator=(const kv_transaction_guard &) = delete;

private:
    kv_transaction_ptr _txn;
    timestamp_t _commit_timestamp;
    timestamp_t _durable_timestamp;
};

/*
 * config_map --
 *     A configuration map.
 */
class config_map {
    using value_t = std::variant<std::string, std::shared_ptr<std::vector<std::string>>,
      std::shared_ptr<config_map>>;

public:
    /*
     * config_map::from_string --
     *     Parse config map from a string.
     */
    static config_map from_string(const char *str, const char **end = NULL);

    /*
     * config_map::merge --
     *     Merge two config maps.
     */
    static config_map merge(const config_map &a, const config_map &b);

    /*
     * config_map::merge --
     *     Merge two config maps.
     */
    inline static std::shared_ptr<config_map>
    merge(std::shared_ptr<config_map> a, std::shared_ptr<config_map> b)
    {
        return std::make_shared<config_map>(merge(*a, *b));
    }

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
     * config_map::get_array --
     *     Get the corresponding array value. Throw an exception on error.
     */
    inline std::shared_ptr<std::vector<std::string>>
    get_array(const char *key) const
    {
        return std::get<std::shared_ptr<std::vector<std::string>>>(get_raw(key));
    }

    /*
     * config_map::get_array_uint64 --
     *     Get the corresponding array value, as integers. Throw an exception on error.
     */
    inline std::shared_ptr<std::vector<uint64_t>>
    get_array_uint64(const char *key) const
    {
        std::shared_ptr<std::vector<uint64_t>> r = std::make_shared<std::vector<uint64_t>>();
        std::shared_ptr<std::vector<std::string>> a = get_array(key);
        for (const std::string &s : *a) {
            std::istringstream stream(s);
            uint64_t v;
            stream >> v;
            r->push_back(v);
        }
        return r;
    }

    /*
     * config_map::get_map --
     *     Get the corresponding config map value. Throw an exception on error.
     */
    inline std::shared_ptr<config_map> const
    get_map(const char *key)
    {
        return std::get<std::shared_ptr<config_map>>(get_raw(key));
    }

    /*
     * config_map::get_string --
     *     Get the corresponding string value. Throw an exception on error.
     */
    inline std::string
    get_string(const char *key) const
    {
        return std::get<std::string>(get_raw(key));
    }

    /*
     * config_map::get_bool --
     *     Get the corresponding bool value. Throw an exception on error.
     */
    inline uint64_t
    get_bool(const char *key) const
    {
        std::string v = std::get<std::string>(get_raw(key));
        return v == "true" || v == "1";
    }

    /*
     * config_map::get_float --
     *     Get the corresponding float value. Throw an exception on error.
     */
    inline float
    get_float(const char *key) const
    {
        return std::stof(std::get<std::string>(get_raw(key)));
    }

    /*
     * config_map::get_uint64 --
     *     Get the corresponding integer value. Throw an exception on error.
     */
    inline uint64_t
    get_uint64(const char *key) const
    {
        std::istringstream stream(std::get<std::string>(get_raw(key)));
        uint64_t v;
        stream >> v;
        return v;
    }

    /*
     * config_map::get_uint64_hex --
     *     Get the corresponding hexadecimal integer value. Throw an exception on error.
     */
    inline uint64_t
    get_uint64_hex(const char *key) const
    {
        std::istringstream stream(std::get<std::string>(get_raw(key)));
        uint64_t v;
        stream >> std::hex >> v;
        return v;
    }

    /*
     * config_map::keys --
     *     Get the collection of keys.
     */
    inline std::vector<std::string>
    keys() const noexcept
    {
        std::vector<std::string> r;
        for (std::pair<std::string, value_t> p : _map)
            r.push_back(p.first);
        return r;
    }

private:
    /*
     * config_map::config_map --
     *     Create a new instance of the config map.
     */
    inline config_map() noexcept {};

    /*
     * config_map::get_raw --
     *     Get the raw value of a key from the config map; throw an exception if not found.
     */
    inline const value_t
    get_raw(const std::string &key) const
    {
        auto iter = _map.find(key);
        if (iter == _map.end())
            throw std::runtime_error("No such key in the config map: " + key);
        return iter->second;
    }

    /*
     * config_map::parse_array --
     *     Parse an array.
     */
    static std::shared_ptr<std::vector<std::string>> parse_array(const char *str, const char **end);

private:
    std::unordered_map<std::string, value_t> _map;
};

/*
 * shared_memory --
 *     Shared memory with a child process. After creating this object, the shared memory would be
 *     available in both the parent and the child process. The memory object will be automatically
 *     cleaned up when it falls out of scope.
 */
class shared_memory {

public:
    /*
     * shared_memory::shared_memory --
     *     Create a shared memory object of the given size.
     */
    shared_memory(size_t size);

    /* Delete the copy constructor. */
    shared_memory(const shared_memory &) = delete;

    /* Delete the copy operator. */
    shared_memory &operator=(const shared_memory &) = delete;

    /*
     * shared_memory::~shared_memory --
     *     Free the memory object.
     */
    ~shared_memory();

    /*
     * shared_memory::data --
     *     Get the data pointer.
     */
    inline void *
    data() noexcept
    {
        return _data;
    }

    /*
     * shared_memory::size --
     *     Get the data size.
     */
    inline size_t
    size() noexcept
    {
        return _size;
    }

private:
    void *_data;
    size_t _size;
    std::string _name;
};

/*
 * at_cleanup --
 *     Run an action at the time this object falls out of scope.
 */
class at_cleanup {

public:
    /*
     * at_cleanup::at_cleanup --
     *     Create the cleanup object.
     */
    inline at_cleanup(std::function<void()> fn) : _fn(std::move(fn)){};

    /* Delete the copy constructor. */
    at_cleanup(const at_cleanup &) = delete;

    /* Delete the copy operator. */
    at_cleanup &operator=(const at_cleanup &) = delete;

    /*
     * at_cleanup::~at_cleanup --
     *     Free the object and run the clean up function.
     */
    inline ~at_cleanup()
    {
        _fn();
    }

private:
    std::function<void()> _fn;
};

/*
 * decode_utf8 --
 *     Decode a UTF-8 string into one-byte code points. Throw an exception on error.
 */
std::string decode_utf8(const std::string &str);

/*
 * directory_path --
 *     Get path to the parent directory. Throw an exception on error.
 */
std::string directory_path(const std::string &path);

/*
 * executable_path --
 *     Path to the current executable. Throw an exception on error.
 */
std::string executable_path();

/*
 * model_library_path --
 *     Path to this library. Throw an exception on error.
 */
std::string model_library_path();

/*
 * ends_with --
 *     Check whether the string ends with the given suffix.
 */
inline bool
ends_with(std::string_view str, std::string_view suffix)
{
    return str.size() >= suffix.size() &&
      str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/*
 * parse_uint64 --
 *     Parse the string into a number. Throw an exception on error.
 */
uint64_t parse_uint64(const char *str, const char **end = nullptr);

/*
 * parse_uint64 --
 *     Parse the string into a number. Throw an exception on error.
 */
inline uint64_t
parse_uint64(const std::string &str)
{
    return parse_uint64(str.c_str());
}

/*
 * join --
 *     Join two strings with a comma (or the specified separator) if need be.
 */
inline std::string
join(const std::string &a, const std::string &b, const std::string &sep = ",")
{
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    return a + sep + b;
}

/*
 * quote --
 *     Return a string in quotes, with appropriate escaping.
 */
std::string quote(const std::string &str);

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
 * wt_cursor_truncate --
 *     Truncate in WiredTiger using the provided cursors.
 */
inline int
wt_cursor_truncate(WT_SESSION *session, const char *uri, WT_CURSOR *cursor_start,
  WT_CURSOR *cursor_stop, const data_value &start, const data_value &stop)
{
    if (start == NONE)
        cursor_start = nullptr;
    else
        set_wt_cursor_key(cursor_start, start);
    if (stop == NONE)
        cursor_stop = nullptr;
    else
        set_wt_cursor_key(cursor_stop, stop);

    return session->truncate(session,
      cursor_start == nullptr && cursor_stop == nullptr ? uri : nullptr, cursor_start, cursor_stop,
      nullptr);
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
 * wt_build_dir_path --
 *     Path to the WiredTiger build directory, assuming that this library is used from the build
 *     directory. Throw an exception on error.
 */
std::string wt_build_dir_path();

/*
 * wt_disagg_config_string --
 *     Get the config string for disaggregated storage.
 */
std::string wt_disagg_config_string();

/*
 * wt_disagg_pick_up_latest_checkpoint --
 *     Pick up the latest disaggregated storage checkpoint.
 */
bool wt_disagg_pick_up_latest_checkpoint(
  WT_CONNECTION *conn, model::timestamp_t &checkpoint_timestamp);

/*
 * wt_evict --
 *     Evict a WiredTiger page with the given key.
 */
void wt_evict(WT_CONNECTION *conn, const char *uri, const data_value &key);

/*
 * wt_extension_path --
 *     Path to the given WiredTiger extension given its relative path. Throw an exception on error.
 */
std::string wt_extension_path(const std::string &path);

/*
 * wt_list_tables --
 *     Get the list of WiredTiger tables.
 */
std::vector<std::string> wt_list_tables(WT_CONNECTION *conn);

} /* namespace model */
