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

#include <map>
#include <string>
#include <vector>

#include "session_simulator.h"

/* The connection simulator is a Singleton class. */
class connection_simulator {
    /* Methods */
public:
    static connection_simulator &get_connection();
    session_simulator *open_session();
    void close_session(session_simulator *);
    int set_timestamp(const std::string & = "");
    void set_global_durable_ts(uint64_t);
    uint64_t get_oldest_ts() const;
    uint64_t get_stable_ts() const;
    uint64_t get_global_durable_ts() const;
    bool has_oldest_ts() const;
    bool has_stable_ts() const;
    uint64_t get_latest_active_read() const;
    int query_timestamp(const std::string &, std::string &, bool &);
    ~connection_simulator();

private:
    int decode_timestamp_config_map(std::map<std::string, std::string> &, uint64_t &, uint64_t &,
      uint64_t &, bool &, bool &, bool &, bool &);

    /* No copies of the singleton allowed. */
private:
    connection_simulator();

public:
    /* Deleted functions should generally be public as it results in better error messages. */
    connection_simulator(connection_simulator const &) = delete;
    connection_simulator &operator=(connection_simulator const &) = delete;

    /* Member variables */
private:
    std::vector<session_simulator *> _session_list;
    uint64_t _oldest_ts;
    uint64_t _stable_ts;
    uint64_t _durable_ts;
};
