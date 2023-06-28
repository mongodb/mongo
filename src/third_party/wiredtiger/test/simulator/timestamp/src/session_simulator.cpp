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

#include "session_simulator.h"

#include <cassert>
#include <iostream>
#include <map>

#include "connection_simulator.h"
#include "error_simulator.h"
#include "timestamp_manager.h"

session_simulator::session_simulator()
{
    reset_txn_level_var();
}

int
session_simulator::begin_transaction(const std::string &config)
{
    /* Make sure that the transaction from this session isn't running. */
    if (_txn_running)
        WT_SIM_RET_MSG(EINVAL, "'begin_transaction' not permitted in a running transaction");

    reset_txn_level_var();

    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();
    std::map<std::string, std::string> config_map;

    /*
     * For now, the simulator only supports roundup_timestamp and read_timestamp in the config
     * string for begin_transaction(). Hence, we ignore ignore_prepare, isolation, name,
     * no_timestamp, operation_timeout_ms, priority and sync.
     */
    const std::vector<std::string> supported_ops = {"read_timestamp", "roundup_timestamps"};
    const std::vector<std::string> unsupported_ops = {"ignore_prepare", "isolation", "name",
      "no_timestamp", "operation_timeout_ms", "priority", "sync"};

    WT_SIM_RET_MSG(ts_manager->parse_config(config, supported_ops, unsupported_ops, config_map),
      "Incorrect config (" + config + ") passed in begin_transaction");

    /* Check if the read or prepared timestamp should be rounded up. */
    auto pos = config_map.find("roundup_timestamps");
    if (pos != config_map.end()) {
        if (pos->second.find("read=true") != std::string::npos)
            _ts_round_read = true;

        if (pos->second.find("prepared=true") != std::string::npos)
            _ts_round_prepared = true;
    }

    /* Set and validate the read timestamp if it provided. */
    pos = config_map.find("read_timestamp");
    if (pos != config_map.end()) {
        WT_SIM_RET(ts_manager->validate_hex_value(pos->second, "read timestamp"));
        uint64_t read_ts = ts_manager->hex_to_decimal(pos->second);
        WT_SIM_RET(set_read_timestamp(read_ts));
    }

    /* Transaction can run successfully if we got to this point. */
    _txn_running = true;
    return (0);
}

int
session_simulator::rollback_transaction(const std::string &config)
{
    /* Make sure that the transaction from this session is running. */
    if (!_txn_running)
        WT_SIM_RET_MSG(EINVAL, "'rollback_transaction' only permitted in a running transaction");

    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();
    std::map<std::string, std::string> config_map;

    /*
     * For now, the simulator does not support operation_timeout_ms in the config string for
     * rollback_transaction().
     */
    const std::vector<std::string> supported_ops;
    const std::vector<std::string> unsupported_ops = {"operation_timeout_ms"};
    WT_SIM_RET_MSG(ts_manager->parse_config(config, supported_ops, unsupported_ops, config_map),
      "Incorrect config (" + config + ") passed in rollback_transaction");

    /* Transaction can rollback successfully if we got to this point. */
    _txn_running = false;
    return (0);
}

int
session_simulator::prepare_transaction(const std::string &config)
{
    /* 'prepare_transaction' only permitted to be called once in a running transaction. */
    if (_prepared_txn)
        WT_SIM_RET_MSG(EINVAL,
          "'prepare_transaction' only permitted to be called once in a running transaction");

    /* Make sure that the transaction from this session is running. */
    if (!_txn_running)
        WT_SIM_RET_MSG(EINVAL, "'prepare_transaction' only permitted in a running transaction");

    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();
    std::map<std::string, std::string> config_map;

    const std::vector<std::string> supported_ops = {"prepare_timestamp"};
    const std::vector<std::string> unsupported_ops;

    WT_TXN_SIM_RET_MSG(ts_manager->parse_config(config, supported_ops, unsupported_ops, config_map),
      "Incorrect config (" + config + ") passed in prepare_transaction");

    auto pos = config_map.find("prepare_timestamp");
    if (pos != config_map.end()) {
        WT_TXN_SIM_RET(ts_manager->validate_hex_value(pos->second, "prepare timestamp"));
        uint64_t prepare_ts = ts_manager->hex_to_decimal(pos->second);
        WT_TXN_SIM_RET(set_prepare_timestamp(prepare_ts));
    }

    /* A prepared timestamp should have been set at this point. */
    if (!has_prepare_timestamp())
        WT_TXN_SIM_RET_MSG(EINVAL, "'prepare_transaction' - prepare timestamp is not set");

    /* Commit timestamp must not be set before transaction is prepared. */
    if (_has_commit_ts)
        WT_TXN_SIM_RET_MSG(EINVAL,
          "'prepare_transaction' - commit timestamp must not be set before transaction is "
          "prepared");

    _prepared_txn = true;

    return (0);
}

int
session_simulator::commit_transaction(const std::string &config)
{
    /* Make sure that the transaction from this session is running. */
    if (!_txn_running)
        WT_SIM_RET_MSG(EINVAL, "'commit_transaction' only permitted in a running transaction");

    /* We need to rollback a transaction if it failed earlier. */
    if (_txn_error) {
        rollback_transaction();
        WT_SIM_RET(EINVAL);
    }

    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();
    std::map<std::string, std::string> config_map;

    /*
     * For now, the simulator only supports commit_timestamp and durable_timestamp in the config
     * string for commit_transaction(). Hence, we ignore operation_timeout_ms and sync.
     */
    const std::vector<std::string> supported_ops = {"commit_timestamp", "durable_timestamp"};
    const std::vector<std::string> unsupported_ops = {"operation_timeout_ms", "sync"};

    WT_SIM_RET_MSG(ts_manager->parse_config(config, supported_ops, unsupported_ops, config_map),
      "Incorrect config (" + config + ") passed in commit_transaction");

    uint64_t commit_ts = 0;
    uint64_t durable_ts = 0;

    if (_has_commit_ts)
        commit_ts = _commit_ts;

    if (_durable_ts_set)
        durable_ts = _durable_ts;

    int ret;
    auto pos = config_map.find("commit_timestamp");
    if (pos != config_map.end()) {
        ret = ts_manager->validate_hex_value(pos->second, "commit timestamp");
        if (ret != 0) {
            rollback_transaction();
            return (ret);
        }
        commit_ts = ts_manager->hex_to_decimal(pos->second);
    }

    pos = config_map.find("durable_timestamp");
    if (pos != config_map.end()) {
        ret = ts_manager->validate_hex_value(pos->second, "durable timestamp");
        if (ret != 0) {
            rollback_transaction();
            return (ret);
        }
        durable_ts = ts_manager->hex_to_decimal(pos->second);
    }

    if (commit_ts != 0) {
        ret = set_commit_timestamp(commit_ts);
        if (ret != 0) {
            rollback_transaction();
            return (ret);
        }
    }

    if (durable_ts != 0) {
        ret = set_durable_timestamp(durable_ts);
        if (ret != 0) {
            rollback_transaction();
            return (ret);
        }
    }

    if (_prepared_txn) {
        if (!_has_commit_ts) {
            rollback_transaction();
            WT_SIM_RET_MSG(EINVAL, "commit timestamp is required for a prepared transaction");
        }

        if (!is_durable_ts_set()) {
            rollback_transaction();
            WT_SIM_RET_MSG(EINVAL, "durable timestamp is required for a prepared transaction");
        }
    } else {
        if (has_prepare_timestamp()) {
            rollback_transaction();
            WT_SIM_RET_MSG(EINVAL, "prepare timestamp is set for non-prepared transaction");
        }

        if (is_durable_ts_set()) {
            rollback_transaction();
            WT_SIM_RET_MSG(
              EINVAL, "durable timestamp should not be specified for non-prepared transaction");
        }
    }

    if (_has_commit_ts || _durable_ts_set) {
        connection_simulator *conn = &connection_simulator::get_connection();
        if (_durable_ts > conn->get_global_durable_ts())
            conn->set_global_durable_ts(_durable_ts);
    }

    /* Transaction can commit successfully if we got to this point. */
    _txn_running = false;
    return (0);
}

int
session_simulator::timestamp_transaction(const std::string &config)
{
    /* Make sure that the transaction from this session is running. */
    if (!_txn_running)
        WT_SIM_RET_MSG(EINVAL, "'timestamp_transaction' only permitted in a running transaction");

    /* If no timestamp was supplied, there's nothing to do. */
    if (config.empty())
        return (0);

    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();
    std::map<std::string, std::string> config_map;

    const std::vector<std::string> supported_ops = {
      "commit_timestamp", "durable_timestamp", "prepare_timestamp", "read_timestamp"};
    const std::vector<std::string> unsupported_ops;

    WT_TXN_SIM_RET_MSG(ts_manager->parse_config(config, supported_ops, unsupported_ops, config_map),
      "Incorrect config (" + config + ") passed in timestamp_transaction");

    uint64_t commit_ts = 0, durable_ts = 0, prepare_ts = 0, read_ts = 0;

    /* Decode a configuration string that may contain multiple timestamps and store them here. */
    WT_TXN_SIM_RET(
      decode_timestamp_config_map(config_map, commit_ts, durable_ts, prepare_ts, read_ts));

    /* Check if the timestamps were included in the configuration string and set them. */
    if (commit_ts != 0)
        WT_TXN_SIM_RET(set_commit_timestamp(commit_ts));

    if (durable_ts != 0)
        WT_TXN_SIM_RET(set_durable_timestamp(durable_ts));

    if (prepare_ts != 0)
        WT_TXN_SIM_RET(set_prepare_timestamp(prepare_ts));

    if (read_ts != 0)
        WT_TXN_SIM_RET(set_read_timestamp(read_ts));

    return (0);
}

int
session_simulator::timestamp_transaction_uint(const std::string &ts_type, uint64_t ts)
{
    /* Make sure that the transaction from this session is running. */
    if (!_txn_running)
        WT_SIM_RET_MSG(
          EINVAL, "'timestamp_transaction_uint' only permitted in a running transaction");

    /* Zero timestamp is not permitted. */
    if (ts == 0)
        WT_TXN_SIM_RET_MSG(
          EINVAL, "Illegal " + std::to_string(ts) + " timestamp: zero not permitted.");

    if (ts_type == "commit")
        WT_TXN_SIM_RET(set_commit_timestamp(ts));
    else if (ts_type == "durable")
        WT_TXN_SIM_RET(set_durable_timestamp(ts));
    else if (ts_type == "prepare")
        WT_TXN_SIM_RET(set_prepare_timestamp(ts));
    else if (ts_type == "read")
        WT_TXN_SIM_RET(set_read_timestamp(ts));
    else {
        WT_TXN_SIM_RET_MSG(
          EINVAL, "Invalid timestamp type (" + ts_type + ") passed to timestamp transaction uint.");
    }

    return (0);
}

int
session_simulator::query_timestamp(const std::string &config, std::string &hex_ts)
{
    std::string query_timestamp;
    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();
    std::map<std::string, std::string> config_map;

    /* For an empty config default to all_durable. */
    if (config.empty())
        query_timestamp = "read";
    else {
        std::map<std::string, std::string> config_map;
        const std::vector<std::string> supported_ops = {"get"};
        const std::vector<std::string> unsupported_ops;

        WT_SIM_RET_MSG(ts_manager->parse_config(config, supported_ops, unsupported_ops, config_map),
          "Incorrect config (" + config + ") passed in query_timestamp");

        auto pos = config_map.find("get");
        query_timestamp = pos->second;
    }

    uint64_t ts;
    if (query_timestamp == "commit")
        ts = _commit_ts;
    else if (query_timestamp == "first_commit")
        ts = _first_commit_ts;
    else if (query_timestamp == "prepare")
        ts = _prepare_ts;
    else if (query_timestamp == "read")
        ts = _read_ts;
    else
        WT_SIM_RET_MSG(EINVAL, "Incorrect config (" + config + ") passed in query timestamp");

    /* Convert the timestamp from decimal to hex-decimal. */
    hex_ts = ts_manager->decimal_to_hex(ts);

    return (0);
}

void
session_simulator::reset_txn_level_var()
{
    _commit_ts = 0;
    _durable_ts = 0;
    _first_commit_ts = 0;
    _prepare_ts = 0;
    _read_ts = 0;
    _durable_ts_set = false;
    _has_commit_ts = false;
    _prepared_txn = false;
    _ts_round_prepared = false;
    _ts_round_read = false;
    _txn_running = false;
    _txn_error = false;
}

uint64_t
session_simulator::get_commit_timestamp() const
{
    return (_commit_ts);
}

uint64_t
session_simulator::get_durable_timestamp() const
{
    return (_durable_ts);
}

uint64_t
session_simulator::get_first_commit_timestamp() const
{
    return (_first_commit_ts);
}

uint64_t
session_simulator::get_prepare_timestamp() const
{
    return (_prepare_ts);
}

uint64_t
session_simulator::get_read_timestamp() const
{
    return (_read_ts);
}

bool
session_simulator::is_round_prepare_ts_set() const
{
    return (_ts_round_prepared);
}

bool
session_simulator::is_round_read_ts_set() const
{
    return (_ts_round_read);
}

bool
session_simulator::is_durable_ts_set() const
{
    return (_durable_ts_set);
}

bool
session_simulator::has_prepare_timestamp() const
{
    return (_prepare_ts != 0);
}

bool
session_simulator::has_read_timestamp() const
{
    return (_read_ts != 0);
}

bool
session_simulator::is_commit_ts_set() const
{
    return (_has_commit_ts);
}

bool
session_simulator::is_txn_prepared() const
{
    return (_prepared_txn);
}

bool
session_simulator::is_txn_running() const
{
    return (_txn_running);
}

bool
session_simulator::is_read_ts_set() const
{
    return (_read_ts != 0);
}

int
session_simulator::set_commit_timestamp(uint64_t commit_ts)
{
    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();
    WT_SIM_RET(ts_manager->validate_commit_timestamp(this, commit_ts));

    if (!_has_commit_ts) {
        _first_commit_ts = commit_ts;
        _has_commit_ts = true;
    }

    if (has_prepare_timestamp())
        if (_ts_round_prepared && commit_ts < _prepare_ts)
            commit_ts = _prepare_ts;

    _commit_ts = commit_ts;
    _durable_ts = commit_ts;

    return (0);
}

int
session_simulator::set_durable_timestamp(uint64_t durable_ts)
{
    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();
    WT_SIM_RET(ts_manager->validate_session_durable_timestamp(this, durable_ts));

    _durable_ts = durable_ts;
    _durable_ts_set = true;

    return (0);
}

int
session_simulator::set_prepare_timestamp(uint64_t prepare_ts)
{
    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();
    WT_SIM_RET(ts_manager->validate_prepare_timestamp(this, prepare_ts));

    /*
     * If the given timestamp is earlier than the oldest timestamp then round the prepare timestamp
     * to oldest timestamp if round prepared is true.
     */
    if (_ts_round_prepared) {
        connection_simulator *conn = &connection_simulator::get_connection();
        uint64_t oldest_ts = conn->get_oldest_ts();
        if (prepare_ts < oldest_ts)
            prepare_ts = oldest_ts;
    }

    _prepare_ts = prepare_ts;

    return (0);
}

int
session_simulator::set_read_timestamp(uint64_t read_ts)
{
    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();
    WT_SIM_RET(ts_manager->validate_read_timestamp(this, read_ts));

    /*
     * If the given timestamp is earlier than the oldest timestamp then round the read timestamp to
     * oldest timestamp.
     */
    connection_simulator *conn = &connection_simulator::get_connection();
    uint64_t oldest_ts = conn->get_oldest_ts();
    if (_ts_round_read && read_ts < oldest_ts)
        _read_ts = oldest_ts;
    else if (read_ts >= oldest_ts)
        _read_ts = read_ts;

    return (0);
}

int
session_simulator::decode_timestamp_config_map(std::map<std::string, std::string> &config_map,
  uint64_t &commit_ts, uint64_t &durable_ts, uint64_t &prepare_ts, uint64_t &read_ts)
{
    timestamp_manager *ts_manager = &timestamp_manager::get_timestamp_manager();
    auto pos = config_map.find("commit_timestamp");
    if (pos != config_map.end()) {
        WT_SIM_RET(ts_manager->validate_hex_value(pos->second, "commit timestamp"));
        commit_ts = ts_manager->hex_to_decimal(pos->second);
    }

    pos = config_map.find("durable_timestamp");
    if (pos != config_map.end()) {
        WT_SIM_RET(ts_manager->validate_hex_value(pos->second, "durable timestamp"));
        durable_ts = ts_manager->hex_to_decimal(pos->second);
    }

    pos = config_map.find("prepare_timestamp");
    if (pos != config_map.end()) {
        WT_SIM_RET(ts_manager->validate_hex_value(pos->second, "prepare timestamp"));
        prepare_ts = ts_manager->hex_to_decimal(pos->second);
    }

    pos = config_map.find("read_timestamp");
    if (pos != config_map.end()) {
        WT_SIM_RET(ts_manager->validate_hex_value(pos->second, "read timestamp"));
        read_ts = ts_manager->hex_to_decimal(pos->second);
    }

    return (0);
}
