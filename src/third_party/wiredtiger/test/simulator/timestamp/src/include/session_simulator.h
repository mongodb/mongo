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

#include <string>
#include <map>

class session_simulator {
    /* Methods */
public:
    session_simulator();
    ~session_simulator() = default;

    /* API functions */
public:
    int begin_transaction(const std::string & = "");
    int rollback_transaction(const std::string & = "");
    int prepare_transaction(const std::string & = "");
    int commit_transaction(const std::string & = "");
    int timestamp_transaction(const std::string & = "");
    int timestamp_transaction_uint(const std::string &, uint64_t);
    int query_timestamp(const std::string &, std::string &);

    /* Transaction relevant functions. */
public:
    uint64_t get_commit_timestamp() const;
    uint64_t get_durable_timestamp() const;
    uint64_t get_first_commit_timestamp() const;
    uint64_t get_prepare_timestamp() const;
    uint64_t get_read_timestamp() const;
    bool has_prepare_timestamp() const;
    bool has_read_timestamp() const;
    bool is_commit_ts_set() const;
    bool is_durable_ts_set() const;
    bool is_read_ts_set() const;
    bool is_round_prepare_ts_set() const;
    bool is_round_read_ts_set() const;
    bool is_txn_prepared() const;
    bool is_txn_running() const;

private:
    int decode_timestamp_config_map(
      std::map<std::string, std::string> &, uint64_t &, uint64_t &, uint64_t &, uint64_t &);
    int set_commit_timestamp(uint64_t);
    int set_durable_timestamp(uint64_t);
    int set_prepare_timestamp(uint64_t);
    int set_read_timestamp(uint64_t);
    void reset_txn_level_var();

public:
    /* Deleted functions should generally be public as it results in better error messages. */
    session_simulator(session_simulator const &) = delete;
    session_simulator &operator=(session_simulator const &) = delete;

    /* Transaction relevant member variables */
private:
    bool _has_commit_ts;
    bool _ts_round_prepared;
    bool _ts_round_read;
    bool _txn_running;
    bool _prepared_txn;
    bool _durable_ts_set;
    bool _txn_error;
    uint64_t _commit_ts;
    uint64_t _durable_ts;
    uint64_t _first_commit_ts;
    uint64_t _prepare_ts;
    uint64_t _read_ts;
};
