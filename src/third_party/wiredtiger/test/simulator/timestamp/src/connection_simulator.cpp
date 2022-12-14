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

#include "connection_simulator.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "error_simulator.h"
#include "timestamp_manager.h"

/* Get an instance of connection_simulator class. */
connection_simulator &
connection_simulator::get_connection()
{
    static connection_simulator _connection_instance;
    return (_connection_instance);
}

bool
connection_simulator::has_oldest_ts() const
{
    return (_oldest_ts != 0);
}

bool
connection_simulator::has_stable_ts() const
{
    return (_stable_ts != 0);
}

uint64_t
connection_simulator::get_oldest_ts() const
{
    return (_oldest_ts);
}

uint64_t
connection_simulator::get_stable_ts() const
{
    return (_stable_ts);
}

uint64_t
connection_simulator::get_global_durable_ts() const
{
    return (_durable_ts);
}

uint64_t
connection_simulator::get_latest_active_read() const
{
    uint64_t max_read_ts = 0;
    for (auto &session : _session_list)
        if (session->is_txn_running())
            if (session->has_read_timestamp())
                if (session->get_read_timestamp() > max_read_ts)
                    max_read_ts = session->get_read_timestamp();
    return (max_read_ts);
}

session_simulator *
connection_simulator::open_session()
{
    session_simulator *session = new session_simulator();

    _session_list.push_back(session);

    return (session);
}

void
connection_simulator::close_session(session_simulator *session)
{
    auto position = std::find(_session_list.begin(), _session_list.end(), session);

    /* The session to be closed should be present in the session list. */
    assert(position != _session_list.end());

    _session_list.erase(position);
    delete session;
    session = nullptr;
}

int
connection_simulator::query_timestamp(
  const std::string &config, std::string &hex_ts, bool &ts_supported)
{
    std::string query_timestamp;
    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();

    /* For an empty config default to all_durable. */
    if (config.empty())
        query_timestamp = "all_durable";
    else {
        std::map<std::string, std::string> config_map;
        const std::vector<std::string> supported_ops = {"get"};
        const std::vector<std::string> unsupported_ops;

        WT_SIM_RET_MSG(ts_manager->parse_config(config, supported_ops, unsupported_ops, config_map),
          "Incorrect config (" + config + ") passed in query_timestamp");

        auto pos = config_map.find("get");
        query_timestamp = pos->second;
    }

    /*
     * For now, the simulator only supports all_durable, oldest_timestamp, and stable_timestamp.
     * Hence, we ignore last_checkpoint, oldest_reader, pinned and recovery.
     */
    ts_supported = false;
    uint64_t ts;
    if (query_timestamp == "all_durable") {
        ts = _durable_ts;

        for (auto &session : _session_list) {
            if (!session->is_txn_running())
                continue;

            uint64_t durable_ts;
            if (session->is_durable_ts_set())
                durable_ts = session->get_durable_timestamp();
            else if (session->is_commit_ts_set()) {
                if (session->is_txn_prepared())
                    continue;
                durable_ts = session->get_first_commit_timestamp();
            } else
                continue;

            if (ts == 0 || --durable_ts < ts)
                ts = durable_ts;
        }
        ts_supported = true;
    } else if (query_timestamp == "oldest_timestamp" || query_timestamp == "oldest") {
        ts = _oldest_ts;
        ts_supported = true;
    } else if (query_timestamp == "stable_timestamp" || query_timestamp == "stable") {
        ts = _stable_ts;
        ts_supported = true;
    } else if (query_timestamp == "last_checkpoint")
        return (0);
    else if (query_timestamp == "oldest_reader")
        return (0);
    else if (query_timestamp == "pinned")
        return (0);
    else if (query_timestamp == "recovery")
        return (0);
    else
        WT_SIM_RET_MSG(EINVAL, "Incorrect config (" + config + ") passed in query timestamp");

    /* Convert the timestamp from decimal to hex-decimal. */
    hex_ts = ts_manager->decimal_to_hex(ts);

    return (0);
}

/* Get the timestamps and decode config map. */
int
connection_simulator::decode_timestamp_config_map(std::map<std::string, std::string> &config_map,
  uint64_t &new_oldest_ts, uint64_t &new_stable_ts, uint64_t &new_durable_ts, bool &has_oldest,
  bool &has_stable, bool &has_durable, bool &force)
{
    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();
    auto pos = config_map.find("oldest_timestamp");
    if (pos != config_map.end()) {
        WT_SIM_RET(ts_manager->validate_hex_value(pos->second, "oldest timestamp"));
        new_oldest_ts = ts_manager->hex_to_decimal(pos->second);
        has_oldest = true;
    }

    pos = config_map.find("stable_timestamp");
    if (pos != config_map.end()) {
        WT_SIM_RET(ts_manager->validate_hex_value(pos->second, "stable timestamp"));
        new_stable_ts = ts_manager->hex_to_decimal(pos->second);
        has_stable = true;
    }

    pos = config_map.find("durable_timestamp");
    if (pos != config_map.end()) {
        WT_SIM_RET(ts_manager->validate_hex_value(pos->second, "durable timestamp"));
        new_durable_ts = ts_manager->hex_to_decimal(pos->second);
        has_durable = true;
    }
    pos = config_map.find("force");
    if (pos != config_map.end()) {
        force = true;
    }

    return (0);
}

int
connection_simulator::set_timestamp(const std::string &config)
{
    /* If no timestamp was supplied, there's nothing to do. */
    if (config.empty())
        return (0);

    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();
    std::map<std::string, std::string> config_map;
    const std::vector<std::string> supported_ops = {
      "durable_timestamp", "force", "oldest_timestamp", "stable_timestamp"};
    const std::vector<std::string> unsupported_ops;

    WT_SIM_RET_MSG(ts_manager->parse_config(config, supported_ops, unsupported_ops, config_map),
      "Incorrect config (" + config + ") passed in set_timestamp");

    uint64_t new_stable_ts = 0, new_oldest_ts = 0, new_durable_ts = 0;
    bool has_stable = false, has_oldest = false, has_durable = false, force = false;

    WT_SIM_RET(decode_timestamp_config_map(config_map, new_oldest_ts, new_stable_ts, new_durable_ts,
      has_oldest, has_stable, has_durable, force));

    if (!force) {
        /* Validate the new durable timestamp. */
        WT_SIM_RET(ts_manager->validate_conn_durable_timestamp(new_durable_ts, has_durable));

        /* Validate the new oldest and stable timestamp. */
        WT_SIM_RET(ts_manager->validate_oldest_and_stable_timestamp(
          new_stable_ts, new_oldest_ts, has_oldest, has_stable));
    }

    if (has_stable)
        _stable_ts = new_stable_ts;
    if (has_oldest)
        _oldest_ts = new_oldest_ts;
    if (has_durable)
        _durable_ts = new_durable_ts;

    return (0);
}

void
connection_simulator::set_global_durable_ts(uint64_t ts)
{
    _durable_ts = ts;
}

connection_simulator::connection_simulator() : _oldest_ts(0), _stable_ts(0), _durable_ts(0) {}

connection_simulator::~connection_simulator()
{
    for (auto session : _session_list)
        delete session;
}
