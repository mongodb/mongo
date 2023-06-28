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

#include "call_log_manager.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <memory>

call_log_manager::call_log_manager(const std::string &call_log_file)
{
    std::ifstream file(call_log_file);
    if (file.fail())
        throw std::runtime_error(
          "File '" + call_log_file + "' either doesn't exist or is not accessible.");

    std::string contents(
      (std::istreambuf_iterator<char>(file)), (std::istreambuf_iterator<char>()));

    if (contents.empty())
        throw std::runtime_error("File '" + call_log_file + "' is empty.");

    /*
     * Get rid of the end line and comma from the call_log if it exists, and prepend, append square
     * brackets.
     */
    while (contents.back() == '\n')
        contents.pop_back();
    while (contents.back() == ',')
        contents.pop_back();
    contents = "[" + contents + "]";

    _call_log = json::parse(contents);
    _conn = &connection_simulator::get_connection();
    api_map_setup();
}

void
call_log_manager::api_map_setup()
{
    _api_map["begin_transaction"] = api_method::begin_transaction;
    _api_map["close_session"] = api_method::close_session;
    _api_map["commit_transaction"] = api_method::commit_transaction;
    _api_map["open_session"] = api_method::open_session;
    _api_map["prepare_transaction"] = api_method::prepare_transaction;
    _api_map["query_timestamp"] = api_method::query_timestamp;
    _api_map["rollback_transaction"] = api_method::rollback_transaction;
    _api_map["set_timestamp"] = api_method::set_timestamp;
    _api_map["timestamp_transaction"] = api_method::timestamp_transaction;
    _api_map["timestamp_transaction_uint"] = api_method::timestamp_transaction_uint;
}

session_simulator *
call_log_manager::get_session(const std::string &session_id)
{
    /* session_id should not be empty. */
    assert(!session_id.empty());

    /* session_id should exist in the map. */
    assert(_session_map.find(session_id) != _session_map.end());

    /* Get the session from the session map. */
    session_simulator *session = _session_map.at(session_id);
    assert(session != nullptr);
    return session;
}

void
call_log_manager::call_log_begin_transaction(const json &call_log_entry)
{
    const std::string session_id = call_log_entry["session_id"].get<std::string>();
    session_simulator *session = get_session(session_id);
    std::string config = call_log_entry["input"]["config"].get<std::string>();

    if (config == "(null)")
        config.clear();

    int ret = session->begin_transaction(config);

    int ret_expected = call_log_entry["return"]["return_val"].get<int>();
    /* The ret value should be equal to the expected ret value. */
    assert(ret == ret_expected);

    if (ret != 0)
        throw "begin_transaction for session_id (" + session_id +
          ") failed with return value: " + std::to_string(ret);
}

void
call_log_manager::call_log_close_session(const json &call_log_entry)
{
    const std::string session_id = call_log_entry["session_id"].get<std::string>();

    /* If there is a failure in closing a session, there is no work to do. */
    int ret = call_log_entry["return"]["return_val"].get<int>();
    if (ret != 0)
        throw "Cannot close the session for session_id (" + session_id +
          ") as return value in the call log is: " + std::to_string(ret);

    session_simulator *session = get_session(session_id);

    /* Remove the session from the connection and the session map. */
    _conn->close_session(session);
    _session_map.erase(session_id);
}

void
call_log_manager::call_log_commit_transaction(const json &call_log_entry)
{
    const std::string session_id = call_log_entry["session_id"].get<std::string>();
    session_simulator *session = get_session(session_id);
    std::string config = call_log_entry["input"]["config"].get<std::string>();

    if (config == "(null)")
        config.clear();

    int ret = session->commit_transaction(config);

    int ret_expected = call_log_entry["return"]["return_val"].get<int>();
    /* The ret value should be equal to the expected ret value. */
    assert(ret == ret_expected);

    if (ret != 0)
        throw "commit_transaction for session_id (" + session_id +
          ") failed with return value: " + std::to_string(ret);
}

void
call_log_manager::call_log_open_session(const json &call_log_entry)
{
    const std::string session_id = call_log_entry["session_id"].get<std::string>();

    /* session_id should not be empty. */
    assert(!session_id.empty());
    /* session_id should not already exist in session map. */
    assert(_session_map.find(session_id) == _session_map.end());

    /* If there is a failure in opening a session, there is no work to do. */
    int ret = call_log_entry["return"]["return_val"].get<int>();
    if (ret != 0)
        throw "Cannot open the session for session_id (" + session_id +
          ") as return value in the call log is: " + std::to_string(ret);

    session_simulator *session = _conn->open_session();
    /*
     * Insert this session into the mapping between the simulator session object and the wiredtiger
     * session object.
     */
    _session_map.insert(std::pair<std::string, session_simulator *>(session_id, session));
}

void
call_log_manager::call_log_prepare_transaction(const json &call_log_entry)
{
    const std::string session_id = call_log_entry["session_id"].get<std::string>();
    session_simulator *session = get_session(session_id);
    std::string config = call_log_entry["input"]["config"].get<std::string>();

    if (config == "(null)")
        config.clear();

    int ret = session->prepare_transaction(config);

    int ret_expected = call_log_entry["return"]["return_val"].get<int>();
    /* The ret value should be equal to the expected ret value. */
    assert(ret == ret_expected);

    if (ret != 0)
        throw "prepare_transaction for session_id (" + session_id +
          ") failed with return value: " + std::to_string(ret);
}

void
call_log_manager::call_log_query_timestamp(const json &call_log_entry)
{
    /* Convert the config char * to a string object. */
    std::string config = call_log_entry["input"]["config"].get<std::string>();

    const std::string class_name = call_log_entry["class_name"].get<std::string>();
    std::string hex_ts;
    bool ts_supported = true;
    int ret;
    /*
     * Check whether we're querying a global connection timestamp or a session transaction
     * timestamp.
     */
    if (class_name == "connection") {
        /*
         * A generated call log without a configuration string in the set timestamp entry will have
         * the string "(null)", default to all_durable.
         */
        if (config == "(null)")
            config = "get=all_durable";

        ret = _conn->query_timestamp(config, hex_ts, ts_supported);
    } else if (class_name == "session") {
        /*
         * A generated call log without a configuration string in the set timestamp entry will have
         * the string "(null)", default to read.
         */
        if (config == "(null)")
            config = "get=read";

        const std::string session_id = call_log_entry["session_id"].get<std::string>();
        session_simulator *session = get_session(session_id);

        ret = session->query_timestamp(config, hex_ts);
    } else
        throw std::invalid_argument(
          "'query_timestamp' failed as class name '" + class_name + "' does not exist!");

    int ret_expected = call_log_entry["return"]["return_val"].get<int>();
    /* The ret value should be equal to the expected ret value. */
    assert(ret == ret_expected);

    if (ret != 0)
        throw "'query_timestamp' failed on " + class_name + "with ret value: '" +
          std::to_string(ret) + "', and config: '" + config + "'";

    /*
     * Ensure that the timestamp returned from query timestamp is equal to the expected timestamp.
     */
    if (ts_supported) {
        std::string hex_ts_expected =
          call_log_entry["output"]["timestamp_queried"].get<std::string>();
        assert(hex_ts == hex_ts_expected);
    }
}

void
call_log_manager::call_log_rollback_transaction(const json &call_log_entry)
{
    const std::string session_id = call_log_entry["session_id"].get<std::string>();
    session_simulator *session = get_session(session_id);
    std::string config = call_log_entry["input"]["config"].get<std::string>();

    if (config == "(null)")
        config.clear();

    int ret = session->rollback_transaction(config);

    int ret_expected = call_log_entry["return"]["return_val"].get<int>();
    /* The ret value should be equal to the expected ret value. */
    assert(ret == ret_expected);

    if (ret != 0)
        throw "rollback_transaction for session_id (" + session_id +
          ") failed with return value: " + std::to_string(ret);
}

void
call_log_manager::call_log_set_timestamp(const json &call_log_entry)
{
    /* Convert the config char * to a string object. */
    const std::string config = call_log_entry["input"]["config"].get<std::string>();

    /*
     * A generated call log without a configuration string in the set timestamp entry will have the
     * string "(null)". No work to do if there is no configuration.
     */
    if (config == "(null)")
        return;

    int ret = _conn->set_timestamp(config);
    int ret_expected = call_log_entry["return"]["return_val"].get<int>();
    /* The ret value should be equal to the expected ret value. */
    assert(ret == ret_expected);

    if (ret != 0)
        throw "'set_timestamp' failed with ret value: '" + std::to_string(ret) +
          "', and config: '" + config + "'";
}

void
call_log_manager::call_log_timestamp_transaction(const json &call_log_entry)
{
    const std::string session_id = call_log_entry["session_id"].get<std::string>();
    session_simulator *session = get_session(session_id);
    std::string config = call_log_entry["input"]["config"].get<std::string>();

    if (config == "(null)")
        config.clear();

    int ret = session->timestamp_transaction(config);

    int ret_expected = call_log_entry["return"]["return_val"].get<int>();
    /* The ret value should be equal to the expected ret value. */
    assert(ret == ret_expected);

    if (ret != 0)
        throw "'timestamp_transaction' failed with ret value: '" + std::to_string(ret) +
          "', and config: '" + config + "'";
}

void
call_log_manager::call_log_timestamp_transaction_uint(const json &call_log_entry)
{
    /* Convert the transaction type char * to a string object. */
    const std::string wt_ts_txn_type = call_log_entry["input"]["wt_ts_txp_type"].get<std::string>();
    const std::string session_id = call_log_entry["session_id"].get<std::string>();

    /* There are no timestamps to be set if the timestamp type is not specified. */
    if (wt_ts_txn_type == "unknown")
        throw "Cannot set a transaction timestamp for session_id (" + session_id +
          ") without a valid timestamp type.";

    const uint64_t ts = call_log_entry["input"]["timestamp"].get<uint64_t>();
    session_simulator *session = get_session(session_id);

    int ret = session->timestamp_transaction_uint(wt_ts_txn_type, ts);
    int ret_expected = call_log_entry["return"]["return_val"].get<int>();
    /* The ret value should be equal to the expected ret value. */
    assert(ret == ret_expected);

    if (ret != 0)
        throw "'timestamp_transaction_uint' failed with ret value: '" + std::to_string(ret) +
          "', and timestamp type: '" + wt_ts_txn_type + "'";
}

void
call_log_manager::process_call_log()
{
    for (const auto &call_log_entry : _call_log)
        process_call_log_entry(call_log_entry);
}

void
call_log_manager::process_call_log_entry(const json &call_log_entry)
{
    try {
        const std::string method_name = call_log_entry["method_name"].get<std::string>();

        switch (_api_map.at(method_name)) {
        case api_method::begin_transaction:
            call_log_begin_transaction(call_log_entry);
            break;
        case api_method::close_session:
            call_log_close_session(call_log_entry);
            break;
        case api_method::commit_transaction:
            call_log_commit_transaction(call_log_entry);
            break;
        case api_method::open_session:
            call_log_open_session(call_log_entry);
            break;
        case api_method::prepare_transaction:
            call_log_prepare_transaction(call_log_entry);
            break;
        case api_method::query_timestamp:
            call_log_query_timestamp(call_log_entry);
            break;
        case api_method::rollback_transaction:
            call_log_rollback_transaction(call_log_entry);
            break;
        case api_method::set_timestamp:
            call_log_set_timestamp(call_log_entry);
            break;
        case api_method::timestamp_transaction:
            call_log_timestamp_transaction(call_log_entry);
            break;
        case api_method::timestamp_transaction_uint:
            call_log_timestamp_transaction_uint(call_log_entry);
            break;
        }
    } catch (std::string &exception_str) {
        std::cerr << "exception: " << exception_str << std::endl << std::endl;
    }
}

int
main(int argc, char *argv[])
{
    /* Exit if call log file was not passed. */
    if (argc != 2)
        throw std::runtime_error("call_log_interface: missing call log file path");

    const std::string call_log_file = argv[1];

    auto cl_manager = std::make_unique<call_log_manager>(call_log_file);
    cl_manager->process_call_log();

    return (0);
}
