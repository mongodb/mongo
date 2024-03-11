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

#include <sys/types.h>
#include <sys/wait.h>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <signal.h>
#include <sstream>
#include <unistd.h>
#include "model/driver/kv_workload_runner_wt.h"
#include "model/util.h"

namespace model {

/*
 * session_context::~session_context --
 *     Destroy the context, alongside the corresponding resources.
 */
kv_workload_runner_wt::session_context::~session_context()
{
    /* Close the session, which automatically closes its cursors. */
    int r = _session->close(_session, nullptr);

    /* We cannot fail the cleanup, so just print a warning */
    if (r != 0)
        std::cerr << "Could not close a session: " << wiredtiger_strerror(r) << " (" << r << ")"
                  << std::endl;
}

/*
 * session_context::cursor --
 *     Get a cursor. Create one if it does not already exist. Use the second argument to get and/or
 *     create additional cursors for the given table.
 */
WT_CURSOR *
kv_workload_runner_wt::session_context::cursor(table_id_t table_id, unsigned table_cur_id)
{
    cursor_id_t id = cursor_id(table_id, table_cur_id);
    auto i = _cursors.find(id);
    if (i != _cursors.end())
        return i->second;

    WT_CURSOR *cursor;
    int ret = _session->open_cursor(
      _session, _workload_context.table_uri(table_id), nullptr, nullptr, &cursor);
    if (ret != 0)
        throw wiredtiger_exception("Failed to open a cursor", ret);

    _cursors[id] = cursor;
    return cursor;
}

/*
 * kv_workload_runner_wt::~kv_workload_runner_wt --
 *     Clean up the workload context.
 */
kv_workload_runner_wt::~kv_workload_runner_wt()
{
    if (_connection == nullptr)
        return;

    try {
        wiredtiger_close();
    } catch (std::runtime_error &e) {
        /*
         * We cannot throw an exception out of a destructor, so just print a warning and continue.
         */
        std::cerr << "Error while cleaning up the workload context: " << e.what() << std::endl;
    }
}

/*
 * kv_workload_runner_wt::run --
 *     Run the workload in WiredTiger. Return the return codes of the workload operations.
 */
std::vector<int>
kv_workload_runner_wt::run(const kv_workload &workload)
{
    /*
     * Initialize the shared memory state, that we will share between the controller (parent)
     * process, and the process that will actually run the workload.
     */
    shared_memory shm_state(sizeof(shared_state) + workload.size() * sizeof(int));
    _state = (shared_state *)shm_state.data();

    /* Clean up the pointer at the end, just before the actual shared memory gets cleaned up. */
    at_cleanup cleanup_state([this]() { _state = nullptr; });

    /*
     * Run the workload in a child process, so that we can properly handle crashes. If the child
     * process crashes intentionally, we'll learn about it through the shared state.
     */
    size_t p = 0; /* Position in the workload. */
    for (;;) {
        bool crashed = _state->expect_crash;
        _state->expect_crash = false;

        pid_t child = fork();
        if (child < 0)
            throw model_exception(std::string("Could not fork the process: ") + strerror(errno) +
              " (" + std::to_string(errno) + ")");

        if (child == 0) {
            int ret = 0;
            try {
                /* Subprocess. */
                wiredtiger_open();

                /* If we just crashed, we may need to recover some of the lost runtime state. */
                if (crashed) {
                    if (_state->num_tables >= sizeof(_state->tables) / sizeof(_state->tables[0]))
                        throw model_exception("Invalid number of tables");
                    for (size_t i = 0; i < _state->num_tables; i++)
                        add_table_uri(
                          _state->tables[i].id, _state->tables[i].uri, true /* recovery */);
                }

                /* Run (or resume) the workload. */
                for (; p < workload.size(); p++) {
                    const operation::any &op = workload[p];
                    if (std::holds_alternative<operation::crash>(op)) {
                        _state->expect_crash = true;
                        _state->crash_index = p;
                    }
                    _state->num_operations = p + 1;
                    _state->return_codes[p] = run_operation(op);
                }

                wiredtiger_close();
            } catch (std::exception &e) {
                _state->exception = true;
                _state->failed_operation = p;
                snprintf(
                  _state->exception_message, sizeof(_state->exception_message), "%s", e.what());
                ret = 1;
            }

            exit(ret);
            /* Not reached. */
        }

        /* Parent process. */
        int pid_status;
        int ret = waitpid(child, &pid_status, 0);
        if (ret < 0)
            throw model_exception(std::string("Waiting for a child process failed: ") +
              strerror(errno) + " (" + std::to_string(errno) + ")");

        if (WIFEXITED(pid_status) && WEXITSTATUS(pid_status) == 0)
            /* Clean exit. */
            break;

        if (_state->expect_crash) {
            /* The crash was intentional. Continue the workload. */
            p = _state->crash_index + 1;
            continue;
        }

        if (_state->exception)
            /* The child process died due to an exception. */
            throw model_exception("Workload was terminated due to an exception at operation " +
              std::to_string(_state->failed_operation + 1) + ": " + _state->exception_message);

        if (WIFEXITED(pid_status))
            /* The child process exited with an error code. */
            throw model_exception(
              "The workload process exited with code " + std::to_string(WEXITSTATUS(pid_status)));

        if (WIFSIGNALED(pid_status))
            /* The child process died due to a signal. */
            throw model_exception("The workload process was terminated with signal " +
              std::to_string(WTERMSIG(pid_status)));

        /* Otherwise the workload failed in some other way. */
        throw model_exception("The workload process terminated in an unexpected way.");
    }

    /* Extract the return codes. */
    std::vector<int> v;
    for (size_t i = 0; i < _state->num_operations; i++)
        v.push_back(_state->return_codes[i]);

    return v;
}

/*
 * kv_workload_runner_wt::add_table_uri --
 *     Add a table URI.
 */
void
kv_workload_runner_wt::add_table_uri(table_id_t id, std::string uri, bool recovery)
{
    std::unique_lock lock(_table_uris_lock);
    if (uri.empty())
        throw model_exception("Invalid table URI");

    /* Add to the runtime map. */
    auto iter = _table_uris.find(id);
    if (iter != _table_uris.end())
        throw model_exception("A table with the given ID already exists");
    _table_uris.insert_or_assign(iter, id, uri);

    /* Add to the workload recovery state. */
    if (!recovery) {
        size_t i = _state->num_tables;
        if (i >= sizeof(_state->tables) / sizeof(_state->tables[0]))
            throw model_exception("Too many tables");
        if (uri.length() + 1 /* for the terminating byte */ > sizeof(_state->tables[i].uri))
            throw model_exception("The table URI is too long");

        _state->tables[i].id = id;
        (void)snprintf(_state->tables[i].uri, sizeof(_state->tables[i].uri), "%s", uri.c_str());
        _state->num_tables++;
    }
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::begin_transaction &op)
{
    std::shared_lock lock(_connection_lock);
    session_context_ptr session = allocate_txn_session(op.txn_id);
    return session->session()->begin_transaction(session->session(), nullptr);
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::checkpoint &op)
{
    std::shared_lock lock(_connection_lock);

    WT_SESSION *session;
    int ret = _connection->open_session(_connection, nullptr, nullptr, &session);
    if (ret != 0)
        return ret;
    wiredtiger_session_guard session_guard(session);

    std::ostringstream config;
    if (!op.name.empty())
        config << "name=" << op.name;
    std::string config_str = config.str();

    return session->checkpoint(session, config_str.c_str());
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::commit_transaction &op)
{
    std::ostringstream config;
    if (op.commit_timestamp != k_timestamp_none)
        config << ",commit_timestamp=" << std::hex << op.commit_timestamp;
    if (op.durable_timestamp != k_timestamp_none)
        config << ",durable_timestamp=" << std::hex << op.durable_timestamp;
    std::string config_str = config.str();

    std::shared_lock lock(_connection_lock);
    session_context_ptr session = remove_txn_session(op.txn_id);

    return session->session()->commit_transaction(session->session(), config_str.c_str());
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::crash &op)
{
    (void)op;

    /*
     * Communicating to the parent process that this crash was intentional is done by the caller,
     * because it knows additional information such as at which point to restart the workload, which
     * is lost by the time we get here.
     */
    assert(_state->expect_crash);

    /* Terminate self with a signal that doesn't produce a core file. */
    (void)kill(getpid(), SIGKILL);

    /*
     * Well, if that somehow failed due to slow signal delivery or some weird behavior of kill,
     * abort. This should not happen, but the person writing this code is a pessimist.
     */
    abort();

    /* Not reached. */
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::create_table &op)
{
    std::shared_lock lock(_connection_lock);

    WT_SESSION *session;
    int ret = _connection->open_session(_connection, nullptr, nullptr, &session);
    if (ret != 0)
        return ret;
    wiredtiger_session_guard session_guard(session);

    std::ostringstream config;
    /*
     * Set the logging parameter explicitly, because if the connection config enables logging (e.g.,
     * because the caller wants to use debug logging), we still don't want logging for these tables.
     */
    config << "log=(enabled=false)";
    config << ",key_format=" << op.key_format << ",value_format=" << op.value_format;
    if (!_table_config.empty())
        config << "," << _table_config;
    std::string config_str = config.str();

    std::string uri = std::string("table:") + op.name;
    ret = session->create(session, uri.c_str(), config_str.c_str());
    if (ret == 0)
        add_table_uri(op.table_id, uri);
    return ret;
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::insert &op)
{
    std::shared_lock lock(_connection_lock);
    session_context_ptr session = txn_session(op.txn_id);
    WT_CURSOR *cursor = session->cursor(op.table_id);
    return wt_cursor_insert(cursor, op.key, op.value);
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::prepare_transaction &op)
{
    std::ostringstream config;
    if (op.prepare_timestamp != k_timestamp_none)
        config << ",prepare_timestamp=" << std::hex << op.prepare_timestamp;
    std::string config_str = config.str();

    std::shared_lock lock(_connection_lock);
    session_context_ptr session = txn_session(op.txn_id);

    return session->session()->prepare_transaction(session->session(), config_str.c_str());
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::remove &op)
{
    std::shared_lock lock(_connection_lock);
    session_context_ptr session = txn_session(op.txn_id);
    WT_CURSOR *cursor = session->cursor(op.table_id);
    return wt_cursor_remove(cursor, op.key);
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::restart &op)
{
    std::unique_lock lock(_connection_lock);
    (void)op;

    wiredtiger_close_nolock();
    wiredtiger_open_nolock();

    return 0;
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::rollback_to_stable &op)
{
    (void)op;

    std::unique_lock lock(_connection_lock);
    return _connection->rollback_to_stable(_connection, nullptr);
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::rollback_transaction &op)
{
    std::shared_lock lock(_connection_lock);
    session_context_ptr session = remove_txn_session(op.txn_id);
    return session->session()->rollback_transaction(session->session(), nullptr);
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::set_commit_timestamp &op)
{
    std::ostringstream config;
    if (op.commit_timestamp != k_timestamp_none)
        config << ",commit_timestamp=" << std::hex << op.commit_timestamp;
    std::string config_str = config.str();

    std::shared_lock lock(_connection_lock);
    session_context_ptr session = txn_session(op.txn_id);

    return session->session()->timestamp_transaction(session->session(), config_str.c_str());
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::set_stable_timestamp &op)
{
    std::ostringstream config;
    config << "stable_timestamp=" << std::hex << op.stable_timestamp;
    std::string config_str = config.str();

    std::shared_lock lock(_connection_lock);
    return _connection->set_timestamp(_connection, config_str.c_str());
}

/*
 * kv_workload_runner_wt::do_operation --
 *     Execute the given workload operation in WiredTiger.
 */
int
kv_workload_runner_wt::do_operation(const operation::truncate &op)
{
    std::shared_lock lock(_connection_lock);
    session_context_ptr session = txn_session(op.txn_id);
    WT_CURSOR *cursor1 = session->cursor(op.table_id);
    WT_CURSOR *cursor2 = session->cursor(op.table_id, 1); /* Open another cursor for the table. */
    return wt_cursor_truncate(
      session->session(), table_uri(op.table_id), cursor1, cursor2, op.start, op.stop);
}

/*
 * kv_workload_runner_wt::wiredtiger_open_nolock --
 *     Open WiredTiger, assume the right locks are held.
 */
void
kv_workload_runner_wt::wiredtiger_open_nolock()
{
    if (_connection != nullptr)
        throw model_exception("WiredTiger is already open");

    int ret = ::wiredtiger_open(_home.c_str(), nullptr, _connection_config.c_str(), &_connection);
    if (ret != 0)
        throw wiredtiger_exception("Cannot open WiredTiger", ret);
}

/*
 * kv_workload_runner_wt::wiredtiger_close_nolock --
 *     Close WiredTiger, assume the right locks are held.
 */
void
kv_workload_runner_wt::wiredtiger_close_nolock()
{
    if (_connection == nullptr)
        throw model_exception("WiredTiger is not open");

    /* Close all sessions. */
    _sessions.clear();

    /* Close the database. */
    int ret = _connection->close(_connection, nullptr);
    if (ret != 0)
        throw wiredtiger_exception("Cannot close WiredTiger", ret);

    _connection = nullptr;
}

/*
 * kv_workload_runner_wt::allocate_txn_session --
 *     Allocate a session context for a transaction.
 */
kv_workload_runner_wt::session_context_ptr
kv_workload_runner_wt::allocate_txn_session(txn_id_t id)
{
    std::unique_lock lock(_sessions_lock);
    if (_connection == nullptr)
        throw model_exception("The database is closed");

    auto i = _sessions.find(id);
    if (i != _sessions.end())
        throw model_exception("A session with the given ID already exists");

    WT_SESSION *session;
    int ret = _connection->open_session(_connection, nullptr, nullptr, &session);
    if (ret != 0)
        throw wiredtiger_exception("Failed to open a session", ret);

    auto context = std::make_shared<session_context>(*this, session);
    _sessions.insert_or_assign(i, id, context);
    return context;
}

} /* namespace model */
