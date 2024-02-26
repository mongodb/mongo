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

#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include "model/driver/kv_workload.h"
#include "model/core.h"
#include "wiredtiger.h"

namespace model {

/*
 * kv_workload_runner_wt --
 *     The workload context for WiredTiger.
 */
class kv_workload_runner_wt {

protected:
    /*
     * session_context --
     *     The WiredTiger session
     */
    class session_context {

        using cursor_id_t = unsigned;
        static constexpr cursor_id_t k_cursors_per_table = 16;

    public:
        /*
         * session_context::session_context --
         *     Create the
         */
        inline session_context(kv_workload_runner_wt &workload_context, WT_SESSION *session)
            : _session(session), _workload_context(workload_context)
        {
        }

        /*
         * session_context::~session_context --
         *     Destroy the context, alongside the corresponding resources.
         */
        ~session_context();

        /*
         * session_context::session --
         *     Get the session.
         */
        inline WT_SESSION *
        session() const noexcept
        {
            return _session;
        }

        /*
         * session_context::cursor --
         *     Get a cursor. Create one if it does not already exist. Use the second argument to get
         *     and/or create additional cursors for the given table.
         */
        WT_CURSOR *cursor(table_id_t table_id, unsigned table_cur_id = 0);

    private:
        /*
         * session_context::cursor_id --
         *     Get a cursor ID.
         */
        static inline cursor_id_t
        cursor_id(table_id_t table_id, unsigned table_cur_id)
        {
            if (table_cur_id < 0 || table_cur_id >= k_cursors_per_table)
                throw model_exception("Cursor ID out of range");
            return (cursor_id_t)(table_id * k_cursors_per_table + table_cur_id);
        }

    private:
        WT_SESSION *_session;
        kv_workload_runner_wt &_workload_context;

        /* Cached cursors. There can be up to k_cursors_per_table cursors for each table. */
        std::unordered_map<cursor_id_t, WT_CURSOR *> _cursors;
    };

    /*
     * session_context_ptr --
     *     The shared pointer for the session
     */
    using session_context_ptr = std::shared_ptr<session_context>;

    /*
     * shared_state --
     *     The shared state of the child executor process, which is shared with the parent. Because
     *     of the way this struct is used, only C types are allowed.
     */
    struct shared_state {

        /*
         * shared_state::table_state --
         *     Shared table state.
         */
        struct table_state {
            table_id_t id;
            char uri[256];
        };

        /* Crash handling. */
        size_t crash_index; /* The crash operation that resulted, well, in the crash. */
        bool expect_crash;  /* True when the child process is expected to crash. */

        /* Execution failure handling. */
        bool exception;              /* If there was an exception. */
        size_t failed_operation;     /* The operation that caused an exception. */
        char exception_message[256]; /* The exception message. */

        /* The map of table IDs to URIs, needed to resume the workload from a crash. */
        size_t num_tables;
        table_state tables[256]; /* The table states; protected by the same lock as the URI map. */
    };

public:
    /*
     * kv_workload_runner_wt::kv_workload_runner_wt --
     *     Create a new workload
     */
    inline kv_workload_runner_wt(
      const char *home, const char *connection_config, const char *table_config)
        : _connection(nullptr),
          _connection_config(connection_config == nullptr ? "" : connection_config), _home(home),
          _state(nullptr), _table_config(table_config == nullptr ? "" : table_config)
    {
    }

    /*
     * kv_workload_runner_wt::~kv_workload_runner_wt --
     *     Clean up the workload
     */
    ~kv_workload_runner_wt();

    /*
     * kv_workload::run --
     *     Run the workload in WiredTiger.
     */
    void run(const kv_workload &workload);

protected:
    /*
     * kv_workload_runner::run_operation --
     *     Run the given operation.
     */
    inline void
    run_operation(const operation::any &op)
    {
        std::visit(
          [this](auto &&x) {
              int ret = do_operation(x);
              /*
               * In the future, we would like to be able to test operations that can fail, at which
               * point we would record and compare return codes. But we're not there yet, so just
               * fail on error.
               */
              if (ret != 0 && ret != WT_NOTFOUND)
                  throw wiredtiger_exception(ret);
          },
          op);
    }

    /*
     * kv_workload_runner_wt::wiredtiger_open --
     *     Open WiredTiger.
     */
    inline void
    wiredtiger_open()
    {
        std::unique_lock lock(_connection_lock);
        wiredtiger_open_nolock();
    }

    /*
     * kv_workload_runner_wt::wiredtiger_close --
     *     Close WiredTiger.
     */
    inline void
    wiredtiger_close()
    {
        std::unique_lock lock(_connection_lock);
        wiredtiger_close_nolock();
    }

    /*
     * kv_workload_runner_wt::do_operation --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::begin_transaction &op);

    /*
     * kv_workload_runner_wt::do_operation --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::checkpoint &op);

    /*
     * kv_workload_runner_wt::do_operation --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::commit_transaction &op);

    /*
     * kv_workload_runner_wt::crash --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::crash &op);

    /*
     * kv_workload_runner_wt::do_operation --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::create_table &op);

    /*
     * kv_workload_runner_wt::do_operation --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::insert &op);

    /*
     * kv_workload_runner_wt::do_operation --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::prepare_transaction &op);

    /*
     * kv_workload_runner_wt::do_operation --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::remove &op);

    /*
     * kv_workload_runner_wt::do_operation --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::restart &op);

    /*
     * kv_workload_runner_wt::do_operation --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::rollback_to_stable &op);

    /*
     * kv_workload_runner_wt::do_operation --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::rollback_transaction &op);

    /*
     * kv_workload_runner_wt::do_operation --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::set_commit_timestamp &op);

    /*
     * kv_workload_runner_wt::do_operation --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::set_stable_timestamp &op);

    /*
     * kv_workload_runner_wt::do_operation --
     *     Execute the given workload operation in WiredTiger.
     */
    int do_operation(const operation::truncate &op);

    /*
     * kv_workload_runner_wt::wiredtiger_open_nolock --
     *     Open WiredTiger, assume the right locks are held.
     */
    void wiredtiger_open_nolock();

    /*
     * kv_workload_runner_wt::wiredtiger_close_nolock --
     *     Close WiredTiger, assume the right locks are held.
     */
    void wiredtiger_close_nolock();

    /*
     * kv_workload_runner_wt::add_table_uri --
     *     Add a table URI.
     */
    void add_table_uri(table_id_t id, std::string uri, bool recovery = false);

    /*
     * kv_workload_runner_wt::table_uri --
     *     Get the table URI. The returned C string pointer is valid for the rest of the duration of
     *     this object.
     */
    inline const char *
    table_uri(table_id_t id) const
    {
        std::shared_lock lock(_table_uris_lock);
        auto i = _table_uris.find(id);
        if (i == _table_uris.end())
            throw model_exception("A table with the given ID does not exist");
        return i->second.c_str();
    }

    /*
     * kv_workload_runner_wt::allocate_txn_session --
     *     Allocate a session context for a transaction.
     */
    session_context_ptr allocate_txn_session(txn_id_t id);

    /*
     * kv_workload_runner_wt::remove_txn_session --
     *     Remove a session context from the transaction.
     */
    inline session_context_ptr
    remove_txn_session(txn_id_t id)
    {
        std::unique_lock lock(_sessions_lock);
        auto i = _sessions.find(id);
        if (i == _sessions.end())
            throw model_exception("A session with the given ID does not already exist");
        session_context_ptr session = i->second;
        _sessions.erase(i);
        return session;
    }

    /*
     * kv_workload_runner_wt::session_context --
     *     Get the session context associated with the given transaction.
     */
    inline session_context_ptr
    txn_session(txn_id_t id) const
    {
        std::shared_lock lock(_sessions_lock);
        auto i = _sessions.find(id);
        if (i == _sessions.end())
            throw model_exception("A session with the given ID does not exist");
        return i->second;
    }

private:
    std::string _connection_config;
    std::string _home;
    std::string _table_config;

    shared_state *_state; /* The shared state between the executor and the parent process. */

    mutable std::shared_mutex _connection_lock; /* Should be held for all operations. */
    WT_CONNECTION *_connection;

    mutable std::shared_mutex _table_uris_lock;
    std::unordered_map<table_id_t, std::string> _table_uris;

    mutable std::shared_mutex _sessions_lock;
    std::unordered_map<txn_id_t, session_context_ptr> _sessions;
};

} /* namespace model */
