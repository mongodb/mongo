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

#include "connection_simulator.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

enum class api_method {
    begin_transaction,
    close_session,
    commit_transaction,
    open_session,
    prepare_transaction,
    query_timestamp,
    rollback_transaction,
    set_timestamp,
    timestamp_transaction,
    timestamp_transaction_uint
};

class call_log_manager {
    /* Methods */
public:
    call_log_manager(const std::string &);
    void process_call_log();

private:
    void process_call_log_entry(const json &);
    void api_map_setup();
    session_simulator *get_session(const std::string &);

private:
    void call_log_begin_transaction(const json &);
    void call_log_close_session(const json &);
    void call_log_commit_transaction(const json &);
    void call_log_open_session(const json &);
    void call_log_prepare_transaction(const json &);
    void call_log_query_timestamp(const json &);
    void call_log_rollback_transaction(const json &);
    void call_log_set_timestamp(const json &);
    void call_log_timestamp_transaction(const json &);
    void call_log_timestamp_transaction_uint(const json &);

    /* Member variables */
private:
    connection_simulator *_conn;
    json _call_log;
    std::map<std::string, api_method> _api_map;
    std::map<std::string, session_simulator *> _session_map;
};
