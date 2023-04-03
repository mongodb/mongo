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

/* Needed to get UINT64_MAX in C++. */
#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

/* Needed to get PRIuXX macros in C++. */
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <algorithm>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "wiredtiger.h"
#include "workgen.h"
#include "workgen_int.h"
extern "C" {
// Include some specific WT files, as some files included by wt_internal.h
// have some C-ism's that don't work in C++.
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include "test_util.h"
}
#define BUF_SIZE 100

#define LATENCY_US_BUCKETS 1000
#define LATENCY_MS_BUCKETS 1000
#define LATENCY_SEC_BUCKETS 100

#define TABLE_MAX_RETRIES 10 // times we will retry an operation on a table
#define THROTTLE_PER_SEC 20  // times per sec we will throttle

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#define TIMESPEC_DOUBLE(ts) (static_cast<double>((ts).tv_sec) + ts.tv_nsec * 0.000000001)
#define PCT(n, total) ((total) == 0 ? 0 : ((n)*100) / (total))
#define OPS_PER_SEC(ops, ts) static_cast<int>((ts) == 0 ? 0.0 : (ops) / TIMESPEC_DOUBLE(ts))

// Get the value of a STL container, even if it is not present
#define CONTAINER_VALUE(container, idx, dfault) \
    (((container).count(idx) > 0) ? (container)[idx] : (dfault))

#define CROSS_USAGE(a, b)                                 \
    (((a & USAGE_READ) != 0 && (b & USAGE_WRITE) != 0) || \
      ((a & USAGE_WRITE) != 0 && (b & USAGE_READ) != 0))

#define ASSERT(cond)                                                                  \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "%s:%d: ASSERT failed: %s\n", __FILE__, __LINE__, #cond); \
            abort();                                                                  \
        }                                                                             \
    } while (0)

#define THROW_ERRNO(e, args)                     \
    do {                                         \
        std::stringstream __sstm;                \
        __sstm << args;                          \
        WorkgenException __wge(e, __sstm.str()); \
        throw(__wge);                            \
    } while (0)

#define THROW(args) THROW_ERRNO(0, args)

#define VERBOSE(runner, args)               \
    do {                                    \
        if ((runner)._context->_verbose)    \
            std::cout << args << std::endl; \
    } while (0)

#define OP_HAS_VALUE(op) \
    ((op)->_optype == Operation::OP_INSERT || (op)->_optype == Operation::OP_UPDATE)

const std::string DYN_TABLE_APP_METADATA = "workgen_dynamic_table=true";
const std::string MIRROR_TABLE_APP_METADATA = "workgen_table_mirror=";
const std::string BASE_TABLE_APP_METADATA = "workgen_base_table=";

namespace workgen {

struct WorkloadRunnerConnection {
    WorkloadRunner *runner;
    WT_CONNECTION *connection;
};

/*
 * The number of contexts. Normally there is one context created, but it will be possible to use
 * several eventually. More than one is not yet implemented, but we must at least guard against the
 * caller creating more than one.
 */
static uint32_t context_count = 0;

static void *
thread_runner_main(void *arg)
{
    ThreadRunner *runner = static_cast<ThreadRunner *>(arg);
    try {
        runner->_errno = runner->run();
    } catch (WorkgenException &wge) {
        runner->_exception = wge;
    }
    return (nullptr);
}

static void *
thread_workload(void *arg)
{

    WorkloadRunnerConnection *runnerConnection = static_cast<WorkloadRunnerConnection *>(arg);
    WorkloadRunner *runner = runnerConnection->runner;
    WT_CONNECTION *connection = runnerConnection->connection;

    try {
        runner->increment_timestamp(connection);
    } catch (WorkgenException &wge) {
        std::cerr << "Exception while incrementing timestamp: " << wge._str << std::endl;
    }

    return (nullptr);
}

static void *
thread_idle_table_cycle_workload(void *arg)
{
    WorkloadRunnerConnection *runnerConnection = static_cast<WorkloadRunnerConnection *>(arg);
    WT_CONNECTION *connection = runnerConnection->connection;
    WorkloadRunner *runner = runnerConnection->runner;

    try {
        runner->start_table_idle_cycle(connection);
    } catch (WorkgenException &wge) {
        std::cerr << "Exception while create/drop tables: " << wge._str << std::endl;
    }

    return (nullptr);
}

static void *
thread_tables_create_workload(void *arg)
{
    WorkloadRunnerConnection *runnerConnection = static_cast<WorkloadRunnerConnection *>(arg);
    WT_CONNECTION *connection = runnerConnection->connection;
    WorkloadRunner *runner = runnerConnection->runner;

    try {
        runner->start_tables_create(connection);
    } catch (WorkgenException &wge) {
        std::cerr << "Exception while creating tables: " << wge._str << std::endl;
    }

    return (nullptr);
}

static void *
thread_tables_drop_workload(void *arg)
{
    WorkloadRunnerConnection *runnerConnection = static_cast<WorkloadRunnerConnection *>(arg);
    WT_CONNECTION *connection = runnerConnection->connection;
    WorkloadRunner *runner = runnerConnection->runner;

    try {
        runner->start_tables_drop(connection);
    } catch (WorkgenException &wge) {
        std::cerr << "Exception while dropping tables: " << wge._str << std::endl;
    }

    return (nullptr);
}

/*
 * The signal handler allows us to gracefully terminate workgen and the user to close the connection
 * in the runner script, thus ensuring a clean shutdown of wiredtiger. The exact signals handled are
 * registered in the WorkloadRunner::run_all function.
 */
volatile std::sig_atomic_t signal_raised = 0;

void
signal_handler(int signum)
{
    signal_raised = signum;
}

int
WorkloadRunner::check_timing(const std::string &name, uint64_t last_interval)
{
    WorkloadOptions *options = &_workload->options;
    int msg_err = 0;

    if (last_interval > options->max_idle_table_cycle) {
        std::string str;
        if (options->max_idle_table_cycle_fatal) {
            msg_err = ETIMEDOUT;
            str = "ERROR";
        } else {
            str = "WARNING";
        }
        std::cerr << str << ": Cycling idle table failed because " << name << " took "
                  << last_interval << " s which is longer than configured acceptable maximum of "
                  << options->max_idle_table_cycle << " s. Diff is "
                  << (last_interval - options->max_idle_table_cycle) << " s." << std::endl;
    }
    return (msg_err);
}

int
WorkloadRunner::start_table_idle_cycle(WT_CONNECTION *conn)
{
    WT_SESSION *session;
    if (conn->open_session(conn, nullptr, nullptr, &session) != 0) {
        THROW("Error opening a session.");
    }

    for (int cycle_count = 0; !stopping; ++cycle_count) {
        char uri[BUF_SIZE];
        snprintf(uri, BUF_SIZE, "table:test_cycle%04d", cycle_count);

        uint64_t start;
        workgen_clock(&start);

        /* Create a table. */
        int ret;
        if ((ret = session->create(session, uri, "key_format=S,value_format=S")) != 0) {
            if (ret == EBUSY)
                continue;
            THROW("Table create failed in start_table_idle_cycle.");
        }

        uint64_t stop;
        workgen_clock(&stop);

        uint64_t last_interval = ns_to_sec(stop - start);
        if ((ret = check_timing("CREATE", last_interval)) != 0)
            THROW_ERRNO(ret, "WT_SESSION->create timeout.");

        start = stop;

        /* Open and close cursor. */
        WT_CURSOR *cursor;
        if ((ret = session->open_cursor(session, uri, nullptr, nullptr, &cursor)) != 0) {
            THROW("Cursor open failed.");
        }
        if ((ret = cursor->close(cursor)) != 0) {
            THROW("Cursor close failed.");
        }

        workgen_clock(&stop);
        last_interval = ns_to_sec(stop - start);
        if ((ret = check_timing("CURSOR", last_interval)) != 0)
            THROW_ERRNO(ret, "WT_SESSION->open_cursor timeout.");
        start = stop;

        /*
         * Drop the table. Keep retrying on EBUSY failure - it is an expected return when
         * checkpoints are happening.
         */
        while ((ret = session->drop(session, uri, "checkpoint_wait=false")) == EBUSY)
            sleep(1);

        if (ret != 0) {
            THROW("Table drop failed in cycle_idle_tables.");
        }
        workgen_clock(&stop);
        last_interval = ns_to_sec(stop - start);
        if ((ret = check_timing("DROP", last_interval)) != 0)
            THROW_ERRNO(ret, "WT_SESSION->drop timeout.");
    }
    return 0;
}

/*
 * Get the cumulative size of all the files under the given directory.
 */
static uint32_t
get_dir_size_mb(const std::string &dir)
{
    uint64_t result = 0;
    for (const auto &path : std::filesystem::recursive_directory_iterator(dir)) {
        try {
            result += std::filesystem::file_size(path);
        } catch (std::filesystem::filesystem_error &e) {
            /*
             * A file might be dropped between listing the directory contents and getting the file
             * sizes. Ignore such errors.
             */
            ASSERT(std::string(e.what()).find("No such file or directory") != std::string::npos);
            continue;
        }
    }
    return result / WT_MEGABYTE;
}

// 5 random characters + Null terminator
#define DYNAMIC_TABLE_LEN 6
/*
 * This function generates a random table name which is alphanumeric and 5 chars long.
 */
static void
gen_random_table_name(char *name, workgen_random_state volatile *rand_state)
{
    ASSERT(name != NULL);

    const char charset[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";

    for (int i = 0; i < DYNAMIC_TABLE_LEN - 1; i++) {
        name[i] = charset[workgen_random(rand_state) % (sizeof(charset) - 1)];
    }
    name[DYNAMIC_TABLE_LEN - 1] = '\0';
}

/*
 * Create a dynamic table with the specified name and config. If mirroring tables, add the mirror
 * table uri to the table runtime. Return an error status if the uri already exists or the create
 * fails.
 */
int
WorkloadRunner::create_table(WT_SESSION *session, const std::string &config, const std::string &uri,
  const std::string &mirror_uri, const bool is_base)
{
    // Check if a table with this name already exists. Return if it does. Use a shared lock to
    // read the dynamic table structure.
    ContextInternal *icontext = _workload->_context->_internal;
    {
        const std::shared_lock lock(*icontext->_dyn_mutex);
        if (icontext->_tint.count(uri) > 0 || icontext->_dyn_tint.count(uri) > 0)
            return EEXIST;
    }

    // Create the table.
    WT_DECL_RET;
    if ((ret = session->create(session, uri.c_str(), config.c_str())) != 0) {
        if (ret != EBUSY)
            THROW("Failed to create table '" << uri << "'.");
        return ret;
    }

    // The data structures for the dynamic table set are protected by a mutex.
    {
        const std::lock_guard<std::shared_mutex> lock(*icontext->_dyn_mutex);

        // Add the table into the list of dynamic set.
        tint_t tint = icontext->_dyn_tint_last;
        icontext->_dyn_tint[uri] = tint;
        icontext->_dyn_table_names[tint] = uri;
        icontext->_dyn_table_runtime[tint] = TableRuntime(is_base, mirror_uri);
        ++icontext->_dyn_tint_last;
        VERBOSE(*_workload, "Created table and added to the dynamic set: " << uri);
    }

    return 0;
}

/*
 * This function creates one or more tables at regular intervals, where the interval length and
 * number of tables are specified in the workload options. It also monitors the database size and
 * stops creating tables once the size crosses a target value. It restarts table creation if the
 * database size drops below a trigger value.
 */
int
WorkloadRunner::start_tables_create(WT_CONNECTION *conn)
{
    WT_SESSION *session;
    if (conn->open_session(conn, nullptr, nullptr, &session) != 0) {
        THROW("Error opening a session.");
    }

    ContextInternal *icontext = _workload->_context->_internal;
    bool manage_db_size =
      _workload->options.create_target > 0 && _workload->options.create_trigger > 0;
    uint32_t db_size = 0;
    bool creating = true;

    if (manage_db_size) {
        db_size = get_dir_size_mb(_wt_home);
        // Initially we start creating tables if the database size is less than the create target.
        creating = db_size < _workload->options.create_target;
    }

    std::string uri;
    std::string mirror_uri = std::string();
    int creates, retries, status;
    char rand_chars[DYNAMIC_TABLE_LEN];

    /*
     * Add app_metadata to the config to indicate the table was created dynamically (can be selected
     * for random deletion), the name of the table's mirror if mirroring is enabled, and if this
     * table is a base table or a mirror. We want these settings to persist over restarts.
     */
    std::string base_config =
      "key_format=S,value_format=S,app_metadata=\"" + DYN_TABLE_APP_METADATA;
    if (_workload->options.mirror_tables) {
        base_config += "," + MIRROR_TABLE_APP_METADATA;
    } else {
        base_config += "," + BASE_TABLE_APP_METADATA + "true\"";
    }
    std::string config = base_config;

    while (!stopping) {
        /*
         * When managing the database size: If we are creating tables, continue until we reach the
         * create target size. If we are not creating tables, begin to do so if the database size
         * falls below the create trigger.
         */
        if (manage_db_size) {
            db_size = get_dir_size_mb(_wt_home);
            if (creating) {
                creating = db_size < _workload->options.create_target;
                if (!creating) {
                    VERBOSE(*_workload,
                      "Stopped creating new tables. db_size now "
                        << db_size << " MB has reached the create target of "
                        << _workload->options.create_target << " MB.");
                }
            } else {
                creating = db_size < _workload->options.create_trigger;
                if (creating) {
                    VERBOSE(*_workload,
                      "Started creating new tables. db_size now "
                        << db_size << " MB has reached the create trigger of "
                        << _workload->options.create_trigger << " MB.");
                }
            }
        }
        if (!creating) {
            sleep(_workload->options.create_interval);
            continue;
        }

        retries = 0;
        creates = 0;
        while (creates < _workload->options.create_count) {

            // Generate a table name from the user specified prefix and a random alphanumeric
            // sequence.
            gen_random_table_name(rand_chars, _rand_state);
            uri = "table:";
            uri += _workload->options.create_prefix;
            uri += rand_chars;

            if (_workload->options.mirror_tables) {
                // The mirror table name is the table name with the user specified suffix.
                mirror_uri = uri + _workload->options.mirror_suffix;
                config = base_config + mirror_uri + "," + BASE_TABLE_APP_METADATA + "true\"";
            }

            // Create the table. Simply continue on failure.
            if (create_table(session, config, uri, mirror_uri, true) == 0) {
                VERBOSE(*_workload, "Created base table '" << uri << "'");
                if (_workload->options.mirror_tables) {
                    // Create the mirror. Retry on failure and throw an exception after
                    // making too many retry attempts.
                    config = base_config + uri + "," + BASE_TABLE_APP_METADATA + "false\"";
                    do
                        status = create_table(session, config, mirror_uri, uri, false);
                    while (status == EBUSY && ++retries < TABLE_MAX_RETRIES);
                    if (status != 0)
                        THROW_ERRNO(
                          status, "Failed to create mirror table '" << mirror_uri << "'.");
                    VERBOSE(*_workload, "Created mirror table '" << mirror_uri << "'");
                }
                ++creates;
            }
        }
        sleep(_workload->options.create_interval);
    }

    return 0;
}

// Randomly select a dynamic table to drop and schedule it for deletion. The function
// returns an EEXIST error status if the selected table is already scheduled for deletion.
int
WorkloadRunner::select_table_for_drop(std::vector<std::string> &pending_delete)
{
    // Select a random table to drop. Skip tables already flagged for deletion.
    ContextInternal *icontext = _workload->_context->_internal;
    auto table_itr = icontext->_dyn_tint.begin();
    std::advance(table_itr, workgen_random(_rand_state) % icontext->_dyn_tint.size());
    if (icontext->_dyn_table_runtime[table_itr->second]._pending_delete) {
        return EEXIST;
    }
    schedule_table_for_drop(table_itr, pending_delete);

    // If the table has a mirror, schedule the mirror table for deletion too.
    if (icontext->_dyn_table_runtime[table_itr->second].has_mirror()) {
        auto mirror_itr =
          icontext->_dyn_tint.find(icontext->_dyn_table_runtime[table_itr->second]._mirror);
        schedule_table_for_drop(mirror_itr, pending_delete);
    }
    return 0;
}

/*
 * The table specified by the dynamic table iterator is flagged for deletion and added to the
 * pending delete list. If the table has a mirror, the mirror is also flagged for deletion and added
 * to the pending delete list.
 */
void
WorkloadRunner::schedule_table_for_drop(
  const std::map<std::string, tint_t>::iterator &itr, std::vector<std::string> &pending_delete)
{
    ContextInternal *icontext = _workload->_context->_internal;
    ASSERT(itr != icontext->_dyn_tint.end());
    VERBOSE(*_workload, "Flagging pending delete for: " << itr->first);
    icontext->_dyn_table_runtime[itr->second]._pending_delete = true;

    ASSERT(
      std::find(pending_delete.begin(), pending_delete.end(), itr->first) == pending_delete.end());
    pending_delete.push_back(itr->first);
}

/*
 * This function drops one or more tables at regular intervals, where the interval length and number
 * of tables are specified in the workload options. It also monitors the database size and stops
 * dropping tables once the size reduces to a target value. It restarts table drops if the database
 * size exceeds a trigger value.
 */
int
WorkloadRunner::start_tables_drop(WT_CONNECTION *conn)
{
    WT_SESSION *session;
    if (conn->open_session(conn, nullptr, nullptr, &session) != 0) {
        THROW("Error opening a session.");
    }

    ContextInternal *icontext = _workload->_context->_internal;
    std::vector<std::string> pending_delete; // Track tables that are pending deletion.
    bool manage_db_size = _workload->options.drop_target > 0 && _workload->options.drop_trigger > 0;
    uint32_t db_size = 0;
    bool dropping = true;

    if (manage_db_size) {
        db_size = get_dir_size_mb(_wt_home);
        // Initially we start dropping tables if the database size is greater than the drop trigger.
        dropping = db_size > _workload->options.drop_trigger;
    }
    while (!stopping) {
        /*
         * When managing the database size: If we are dropping tables, continue until we reach the
         * drop target size. If we are not dropping tables, begin to do so if the database size
         * crosses the drop trigger.
         */
        if (manage_db_size) {
            db_size = get_dir_size_mb(_wt_home);
            if (dropping) {
                dropping = db_size > _workload->options.drop_target;
                if (!dropping) {
                    VERBOSE(*_workload,
                      "Stopped dropping new tables. db_size now "
                        << db_size << " MB has reached the drop target of "
                        << _workload->options.drop_target << " MB.");
                }
            } else {
                dropping = db_size > _workload->options.drop_trigger;
                if (dropping) {
                    VERBOSE(*_workload,
                      "Started dropping new tables. db_size now "
                        << db_size << " MB has reached the drop trigger of "
                        << _workload->options.drop_trigger << " MB.");
                }
            }
        }
        if (!dropping) {
            sleep(_workload->options.drop_interval);
            continue;
        }

        std::vector<std::string> drop_files; // Files ready to be dropped.
        // The data structures for the dynamic table set are protected by a mutex.
        {
            const std::lock_guard<std::shared_mutex> lock(*icontext->_dyn_mutex);

            /*
             * When dropping, consider how many dynamic tables we have left and how many are already
             * marked for deletion.
             */
            int tables_remaining = icontext->_dyn_tint.size() - pending_delete.size();
            ASSERT(tables_remaining >= 0);
            int drop_count = std::min(tables_remaining, _workload->options.drop_count);
            int drops = 0;
            while (drops < drop_count) {
                if (select_table_for_drop(pending_delete) != 0) {
                    continue;
                }
                ++drops;
            }

            /*
             * Now process any pending deletes. The actual table drop will be done later without
             * holding the lock.
             */
            for (size_t i = 0; i < pending_delete.size();) {
                // The table might still be in use.
                const std::string uri(pending_delete.at(i));
                tint_t tint = icontext->_dyn_tint.at(uri);
                ASSERT(icontext->_dyn_table_runtime[tint]._pending_delete == true);
                if (icontext->_dyn_table_runtime[tint]._in_use != 0) {
                    ++i;
                    continue;
                }

                // Delete all local data related to the table.
                ASSERT(icontext->_dyn_table_runtime[tint]._in_use == 0);
                pending_delete.erase(pending_delete.begin() + i);
                icontext->_dyn_tint.erase(uri);
                icontext->_dyn_table_names.erase(tint);
                icontext->_dyn_table_runtime.erase(tint);
                drop_files.push_back(uri);
            }
        }

        /*
         * We will drop the WiredTiger tables without holding the lock. These tables have been
         * removed from the shared data structures, and we know no thread is operating on them.
         */
        for (auto uri : drop_files) {
            WT_DECL_RET;
            // Spin on EBUSY. We do not expect to get stuck.
            while ((ret = session->drop(session, uri.c_str(), "checkpoint_wait=false")) == EBUSY) {
                VERBOSE(*_workload, "Drop returned EBUSY for table: " << uri);
                sleep(1);
            }
            if (ret != 0)
                THROW("Table drop failed for '" << uri << "' in start_tables_drop.");

            VERBOSE(*_workload, "Dropped table: " << uri);
        }

        sleep(_workload->options.drop_interval);
    }

    return 0;
}

/*
 * This function will sleep for "timestamp_advance" seconds, increment and set oldest_timestamp,
 * stable_timestamp with the specified lag until stopping is set to true
 */
int
WorkloadRunner::increment_timestamp(WT_CONNECTION *conn)
{
    uint64_t time_us;
    char buf[BUF_SIZE];

    while (!stopping) {
        if (_workload->options.oldest_timestamp_lag > 0) {
            time_us = WorkgenTimeStamp::get_timestamp_lag(_workload->options.oldest_timestamp_lag);
            snprintf(buf, BUF_SIZE, "oldest_timestamp=%" PRIu64, time_us);
            conn->set_timestamp(conn, buf);
        }

        if (_workload->options.stable_timestamp_lag > 0) {
            time_us = WorkgenTimeStamp::get_timestamp_lag(_workload->options.stable_timestamp_lag);
            snprintf(buf, BUF_SIZE, "stable_timestamp=%" PRIu64, time_us);
            conn->set_timestamp(conn, buf);
        }

        WorkgenTimeStamp::sleep(_workload->options.timestamp_advance);
    }
    return 0;
}

static void *
monitor_main(void *arg)
{
    Monitor *monitor = static_cast<Monitor *>(arg);
    try {
        monitor->_errno = monitor->run();
    } catch (WorkgenException &wge) {
        monitor->_exception = wge;
    }
    return (nullptr);
}

/*
 * Exponentiate (like the pow function), except that it returns an exact integral 64 bit value, and
 * if it overflows, returns the maximum possible value for the return type.
 */
static uint64_t
power64(int base, int exp)
{
    uint64_t last, result;

    result = 1;
    for (int i = 0; i < exp; i++) {
        last = result;
        result *= base;
        if (result < last)
            return UINT64_MAX;
    }
    return result;
}

OptionsList::OptionsList(const OptionsList &other) : _option_map(other._option_map) {}

void
OptionsList::add_option(const std::string &name, const std::string typestr, const std::string &desc)
{
    TypeDescPair pair(typestr, desc);
    _option_map[name] = pair;
}

void
OptionsList::add_int(const std::string &name, int default_value, const std::string &desc)
{
    std::stringstream sstm;
    sstm << "int, default=" << default_value;
    add_option(name, sstm.str(), desc);
}

void
OptionsList::add_bool(const std::string &name, bool default_value, const std::string &desc)
{
    std::stringstream sstm;
    sstm << "boolean, default=" << (default_value ? "true" : "false");
    add_option(name, sstm.str(), desc);
}

void
OptionsList::add_double(const std::string &name, double default_value, const std::string &desc)
{
    std::stringstream sstm;
    sstm << "double, default=" << default_value;
    add_option(name, sstm.str(), desc);
}

void
OptionsList::add_string(
  const std::string &name, const std::string &default_value, const std::string &desc)
{
    std::stringstream sstm;
    sstm << "string, default=\"" << default_value << "\"";
    add_option(name, sstm.str(), desc);
}

static void
pretty_print(const char *p, const char *indent, std::stringstream &sstm)
{
    const char *t;

    for (;; p = t + 1) {
        if (strlen(p) <= 70)
            break;
        for (t = p + 70; t > p && *t != ' '; --t)
            ;
        if (t == p) /* No spaces? */
            break;
        if (indent != nullptr)
            sstm << indent;
        std::string line(p, (size_t)(t - p));
        sstm << line << std::endl;
    }
    if (*p != '\0') {
        if (indent != nullptr)
            sstm << indent;
        sstm << p << std::endl;
    }
}

std::string
OptionsList::help() const
{
    std::stringstream sstm;
    for (std::map<std::string, TypeDescPair>::const_iterator i = _option_map.begin();
         i != _option_map.end(); i++) {
        sstm << i->first << " (" << i->second.first << ")" << std::endl;
        pretty_print(i->second.second.c_str(), "\t", sstm);
    }
    return sstm.str();
}

std::string
OptionsList::help_description(const std::string &option_name) const
{
    if (_option_map.count(option_name) == 0)
        return ("");
    else
        return (_option_map.find(option_name)->second.second);
}

std::string
OptionsList::help_type(const std::string &option_name) const
{
    if (_option_map.count(option_name) == 0)
        return ("");
    else
        return (_option_map.find(option_name)->second.first);
}

Context::Context() : _verbose(false), _internal(new ContextInternal()) {}
Context::~Context()
{
    delete _internal;
}
Context &
Context::operator=(const Context &other)
{
    _verbose = other._verbose;
    *_internal = *other._internal;
    return (*this);
}

ContextInternal::ContextInternal()
    : _tint(), _table_names(), _table_runtime(), _tint_last(0), _dyn_tint(), _dyn_table_names(),
      _dyn_table_runtime(), _dyn_tint_last(0), _context_count(0),
      _dyn_mutex(new std::shared_mutex())

{
    uint32_t count = workgen_atomic_add32(&context_count, 1);
    if (count != 1)
        THROW("multiple Contexts not supported");
    _context_count = count;
}

ContextInternal::~ContextInternal() {}

int
ContextInternal::create_all(WT_CONNECTION *conn)
{
    if (_table_runtime.size() < _tint_last) {
        // The array references are 1-based, we'll waste one entry.
        _table_runtime.resize(_tint_last + 1);
    }

    /*
     * Populate the structure for the dynamic tables. We are single threaded here, so no need to
     * lock. We walk the WiredTiger metadata and filter out tables based on app_metadata. The
     * dynamic set of tables are marked separately during creation.
     */
    WT_SESSION *session;
    if (conn->open_session(conn, nullptr, nullptr, &session) != 0) {
        THROW("Error opening a session.");
    }

    WT_DECL_RET;
    WT_CURSOR *cursor;
    if ((ret = session->open_cursor(session, "metadata:", NULL, NULL, &cursor)) != 0) {
        /* If there is no metadata (yet), this will return ENOENT. */
        if (ret == ENOENT) {
            THROW("No metadata found while extracting dynamic set of tables.");
        }
    }

    /* Walk the entries in the metadata and extract the dynamic set. */
    while ((ret = cursor->next(cursor)) == 0) {
        const char *key, *v;
        if ((ret = cursor->get_key(cursor, &key)) != 0) {
            THROW(
              "Error getting the key for a metadata entry while extracting dynamic set of tables.");
        }
        if ((ret = cursor->get_value(cursor, &v)) != 0) {
            THROW(
              "Error getting the value for a metadata entry while extracting dynamic set of "
              "tables.");
        }

        std::string value = std::string(v);
        size_t pos = value.find(DYN_TABLE_APP_METADATA);
        if (pos != std::string::npos && WT_PREFIX_MATCH(key, "table:")) {
            // Add the table into the list of dynamic set. We are single threaded here and hence
            // do not yet need to protect the dynamic table structures with a lock.
            _dyn_tint[key] = _dyn_tint_last;
            _dyn_table_names[_dyn_tint_last] = key;
            _dyn_table_runtime[_dyn_tint_last] = TableRuntime();

            size_t start, end;
            pos = value.find(MIRROR_TABLE_APP_METADATA);
            // If the table has a mirror, add the mirror name to the runtime data.
            if (pos != std::string::npos) {
                start = value.find_first_of('=', pos);
                end = value.find_first_of(",\"", start);
                if (start == std::string::npos || end == std::string::npos) {
                    THROW("Unable to retrieve mirror name from metadata.");
                }
                _dyn_table_runtime[_dyn_tint_last]._mirror =
                  value.substr(start + 1, end - start - 1);
            }

            pos = value.find(BASE_TABLE_APP_METADATA);
            // Is this a base table or a mirror?
            if (pos != std::string::npos) {
                start = value.find_first_of('=', pos);
                end = value.find_first_of(",\"", start);
                if (start == std::string::npos || end == std::string::npos) {
                    THROW("Unable to retrieve base table status from metadata.");
                }
                _dyn_table_runtime[_dyn_tint_last]._is_base =
                  (value.substr(start + 1, end - start - 1) == "true");
            }

            ++_dyn_tint_last;
        }
    }
    if (ret != WT_NOTFOUND) {
        THROW("Error extracting dynamic set of tables from the metadata.");
    }

    if ((ret = cursor->close(cursor)) != 0) {
        THROW("Cursor close failed.");
    }
    if ((ret = session->close(session, NULL)) != 0) {
        THROW("Session close failed.");
    }

    return (0);
}

Monitor::Monitor(WorkloadRunner &wrunner)
    : _errno(0), _exception(), _wrunner(wrunner), _stop(false), _handle(), _out(nullptr),
      _json(nullptr)
{
}

int
Monitor::run()
{
    struct timespec t;
    struct tm *tm, _tm;
    char version[100];
    Stats prev_totals;
    WorkloadOptions *options = &_wrunner._workload->options;
    uint64_t latency_max = (uint64_t)options->max_latency * THOUSAND;
    bool first_iteration = true;

    // Format header of the table in _out stream.
    if (_out != nullptr)
        _format_out_header();

    workgen_version(version, sizeof(version));
    Stats prev_interval;

    // The whole and fractional part of sample_interval are separated,
    // we don't want to sleep longer than a second.
    int sample_secs = ms_to_sec(options->sample_interval_ms);
    useconds_t sample_usecs = ms_to_us(options->sample_interval_ms) - sec_to_us(sample_secs);

    // Format JSON prefix.
    if (_json != nullptr)
        _format_json_prefix(version);

    while (!_stop) {
        int waitsecs;
        useconds_t waitusecs;

        if (first_iteration && options->warmup > 0) {
            waitsecs = options->warmup;
            waitusecs = 0;
        } else {
            waitsecs = sample_secs;
            waitusecs = sample_usecs;
        }
        for (int i = 0; i < waitsecs && !_stop; i++)
            sleep(1);
        if (_stop)
            break;
        if (waitusecs > 0)
            usleep(waitusecs);
        if (_stop)
            break;

        workgen_epoch(&t);
        tm = localtime_r(&t.tv_sec, &_tm);

        Stats new_totals(true);
        for (std::vector<ThreadRunner>::iterator tr = _wrunner._trunners.begin();
             tr != _wrunner._trunners.end(); tr++)
            new_totals.add(tr->_stats, true);
        Stats interval(new_totals);
        interval.subtract(prev_totals);

        bool checkpointing =
          new_totals.checkpoint.ops_in_progress > 0 || interval.checkpoint.ops > 0;
        double interval_secs = options->sample_interval_ms / 1000.0;

        // Format entry into _out stream.
        if (_out != nullptr)
            _format_out_entry(interval, interval_secs, t, checkpointing, *tm);

        // Format entry into _json stream.
        if (_json != nullptr)
            _format_json_entry(*tm, t, first_iteration, interval, checkpointing, interval_secs);

        // Check latency threshold. Write warning into std::cerr in case read, insert or update
        // exceeds latency_max.
        _check_latency_threshold(interval, latency_max);

        prev_interval.assign(interval);
        prev_totals.assign(new_totals);

        first_iteration = false;
    }

    // Format JSON suffix.
    if (_json != nullptr)
        _format_json_suffix();

    return (0);
}

void
Monitor::_format_out_header()
{
    (*_out) << "#time,"
            << "totalsec,"
            << "read ops per second,"
            << "insert ops per second,"
            << "update ops per second,"
            << "checkpoints,"
            << "read average latency(uS),"
            << "read minimum latency(uS),"
            << "read maximum latency(uS),"
            << "insert average latency(uS),"
            << "insert min latency(uS),"
            << "insert maximum latency(uS),"
            << "update average latency(uS),"
            << "update min latency(uS),"
            << "update maximum latency(uS)" << std::endl;
}

void
Monitor::_format_out_entry(const Stats &interval, double interval_secs, const timespec &timespec,
  bool checkpointing, const tm &tm)
{
    char time_buf[64];
    uint64_t cur_reads = (uint64_t)(interval.read.ops / interval_secs);
    uint64_t cur_inserts = (uint64_t)(interval.insert.ops / interval_secs);
    uint64_t cur_updates = (uint64_t)(interval.update.ops / interval_secs);
    uint64_t totalsec = ts_sec(timespec - _wrunner._start);

    (void)strftime(time_buf, sizeof(time_buf), "%b %d %H:%M:%S", &tm);
    (*_out) << time_buf << "," << totalsec << "," << cur_reads << "," << cur_inserts << ","
            << cur_updates << "," << (checkpointing ? 'Y' : 'N') << ","
            << interval.read.average_latency() << "," << interval.read.min_latency << ","
            << interval.read.max_latency << "," << interval.insert.average_latency() << ","
            << interval.insert.min_latency << "," << interval.insert.max_latency << ","
            << interval.update.average_latency() << "," << interval.update.min_latency << ","
            << interval.update.max_latency << std::endl;
}

void
Monitor::_format_json_prefix(const std::string &version)
{
    (*_json) << "{";
    (*_json) << "\"version\":\"" << version << "\",";
    (*_json) << "\"workgen\":[";
}

void
Monitor::_format_json_entry(const tm &tm, const timespec &timespec, bool first_iteration,
  const Stats &interval, bool checkpointing, double interval_secs)
{
#define WORKGEN_TIMESTAMP_JSON "%Y-%m-%dT%H:%M:%S"
#define TRACK_JSON(f, name, t, percentiles, extra)                                               \
    do {                                                                                         \
        int _i;                                                                                  \
        (f) << "\"" << (name) << "\":{" << extra                                                 \
            << "\"ops per sec\":" << (uint64_t)((t).ops / interval_secs)                         \
            << ",\"rollbacks\":" << ((t).rollbacks)                                              \
            << ",\"average latency\":" << (t).average_latency()                                  \
            << ",\"min latency\":" << (t).min_latency << ",\"max latency\":" << (t).max_latency; \
        for (_i = 0; (percentiles)[_i] != 0; _i++)                                               \
            (f) << ",\"" << (percentiles)[_i]                                                    \
                << "% latency\":" << (t).percentile_latency(percentiles[_i]);                    \
        (f) << "}";                                                                              \
    } while (0)

    // Note: we could allow this to be configurable.
    int percentiles[4] = {50, 95, 99, 0};
    size_t buf_size;
    char time_buf[64];

    buf_size = strftime(time_buf, sizeof(time_buf), WORKGEN_TIMESTAMP_JSON, &tm);
    ASSERT(buf_size <= sizeof(time_buf));
    snprintf(&time_buf[buf_size], sizeof(time_buf) - buf_size, ".%3.3" PRIu64 "Z",
      (uint64_t)ns_to_ms(timespec.tv_nsec));

    if (!first_iteration)
        (*_json) << ",";

    (*_json) << "{";
    (*_json) << "\"localTime\":\"" << time_buf << "\",";
    TRACK_JSON(*_json, "read", interval.read, percentiles, "");
    (*_json) << ",";
    TRACK_JSON(*_json, "insert", interval.insert, percentiles, "");
    (*_json) << ",";
    TRACK_JSON(*_json, "update", interval.update, percentiles, "");
    (*_json) << ",";
    TRACK_JSON(*_json, "checkpoint", interval.checkpoint, percentiles,
      "\"active\":" << (checkpointing ? "1," : "0,"));
    (*_json) << "}" << std::endl;
}

void
Monitor::_format_json_suffix()
{
    (*_json) << "]}" << std::endl;
}

void
Monitor::_check_latency_threshold(const Stats &interval, uint64_t latency_max)
{
    uint64_t read_max = interval.read.max_latency;
    uint64_t insert_max = interval.insert.max_latency;
    uint64_t update_max = interval.update.max_latency;

    if (read_max > latency_max)
        std::cerr << "WARNING: max latency exceeded for read operation. Threshold " << latency_max
                  << " us, recorded " << read_max << " us, diff " << (read_max - latency_max)
                  << " us." << std::endl;
    if (insert_max > latency_max)
        std::cerr << "WARNING: max latency exceeded for insert operation. Threshold " << latency_max
                  << " us, recorded " << insert_max << " us, diff " << (insert_max - latency_max)
                  << " us." << std::endl;
    if (update_max > latency_max)
        std::cerr << "WARNING: max latency exceeded for update operation. Threshold " << latency_max
                  << " us, recorded " << insert_max << " us, diff " << (update_max - latency_max)
                  << " us." << std::endl;
}

ParetoOptions ParetoOptions::DEFAULT;
ParetoOptions::ParetoOptions(int param_arg)
    : param(param_arg), range_low(0.0), range_high(1.0), _options()
{
    _options.add_int("param", param,
      "0 is disabled, otherwise a range from 1 (most aggressive) to "
      "100 (least aggressive)");
    _options.add_double(
      "range_low", range_low, "between 0.0 and 1.0, starting range of the pareto distribution");
    _options.add_double(
      "range_high", range_high, "between 0.0 and 1.0, ending range of the pareto distribution");
}
ParetoOptions::ParetoOptions(const ParetoOptions &other)
    : param(other.param), range_low(other.range_low), range_high(other.range_high),
      _options(other._options)
{
}

ThreadRunner::ThreadRunner()
    : _errno(0), _exception(), _thread(nullptr), _context(nullptr), _icontext(nullptr),
      _workload(nullptr), _wrunner(nullptr), _rand_state(nullptr), _throttle(nullptr),
      _throttle_ops(0), _throttle_limit(0), _in_transaction(false), _start_time_us(0),
      _op_time_us(0), _number(0), _stats(false), _table_usage(), _cursors(nullptr), _stop(false),
      _session(nullptr), _keybuf(nullptr), _valuebuf(nullptr), _repeat(false)
{
}

ThreadRunner::~ThreadRunner()
{
    free_all();
}

int
ThreadRunner::create_all(WT_CONNECTION *conn)
{
    WT_RET(close_all());
    ASSERT(_session == nullptr);
    if (_thread->options.synchronized)
        _thread->_op.synchronized_check();
    WT_RET(conn->open_session(conn, nullptr, _thread->options.session_config.c_str(), &_session));
    _table_usage.clear();
    _stats.track_latency(_workload->options.sample_interval_ms > 0);
    WT_RET(workgen_random_alloc(_session, &_rand_state));
    _throttle_ops = 0;
    _throttle_limit = 0;
    _in_transaction = 0;

    size_t keysize = 1;
    size_t valuesize = 1;
    op_create_all(&_thread->_op, keysize, valuesize);
    _keybuf = new char[keysize];
    _valuebuf = new char[valuesize];
    _keybuf[keysize - 1] = '\0';
    _valuebuf[valuesize - 1] = '\0';
    return (0);
}

int
ThreadRunner::open_all()
{
    typedef WT_CURSOR *WT_CURSOR_PTR;
    if (_cursors != nullptr)
        delete _cursors;
    _cursors = new WT_CURSOR_PTR[_icontext->_tint_last + 1];
    memset(_cursors, 0, sizeof(WT_CURSOR *) * (_icontext->_tint_last + 1));
    for (std::map<uint32_t, uint32_t>::iterator i = _table_usage.begin(); i != _table_usage.end();
         i++) {
        uint32_t tindex = i->first;
        const std::string uri(_icontext->_table_names[tindex]);
        WT_RET(_session->open_cursor(_session, uri.c_str(), nullptr, nullptr, &_cursors[tindex]));
    }
    return (0);
}

int
ThreadRunner::close_all()
{
    if (_throttle != nullptr) {
        delete _throttle;
        _throttle = nullptr;
    }
    if (_session != nullptr) {
        WT_RET(_session->close(_session, nullptr));
        _session = nullptr;
    }
    free_all();
    return (0);
}

void
ThreadRunner::free_all()
{
    if (_rand_state != nullptr) {
        workgen_random_free(_rand_state);
        _rand_state = nullptr;
    }
    if (_cursors != nullptr) {
        delete _cursors;
        _cursors = nullptr;
    }
    if (_keybuf != nullptr) {
        delete _keybuf;
        _keybuf = nullptr;
    }
    if (_valuebuf != nullptr) {
        delete _valuebuf;
        _valuebuf = nullptr;
    }
}

int
ThreadRunner::cross_check(std::vector<ThreadRunner> &runners)
{
    std::map<uint32_t, uint32_t> usage;

    // Determine which tables have cross usage
    for (std::vector<ThreadRunner>::iterator r = runners.begin(); r != runners.end(); r++) {
        for (std::map<uint32_t, uint32_t>::iterator i = r->_table_usage.begin();
             i != r->_table_usage.end(); i++) {
            uint32_t tindex = i->first;
            uint32_t thisusage = i->second;
            uint32_t curusage = CONTAINER_VALUE(usage, tindex, 0);
            if (CROSS_USAGE(curusage, thisusage))
                curusage |= USAGE_MIXED;
            usage[tindex] = curusage;
        }
    }
    for (std::map<uint32_t, uint32_t>::iterator i = usage.begin(); i != usage.end(); i++) {
        if ((i->second & USAGE_MIXED) != 0) {
            for (std::vector<ThreadRunner>::iterator r = runners.begin(); r != runners.end(); r++) {
                r->_table_usage[i->first] |= USAGE_MIXED;
            }
        }
    }
    return (0);
}

int
ThreadRunner::run()
{
    WT_DECL_RET;
    ThreadOptions *options = &_thread->options;
    std::string name = options->name;

    timespec start_time;
    workgen_epoch(&start_time);
    _start_time_us = ts_us(start_time);
    _op_time_us = _start_time_us;

    VERBOSE(*this, "thread " << name << " running");
    if (options->throttle != 0) {
        _throttle = new Throttle(*this, options->throttle, options->throttle_burst);
    }
    for (int cnt = 0; !_stop && (_repeat || cnt < 1) && ret == 0; cnt++) {
        WT_ERR(op_run_setup(&_thread->_op));
    }

err :
#ifdef _DEBUG
{
    std::string messages = this->get_debug();
    if (!messages.empty())
        std::cerr << "DEBUG (thread " << name << "): " << messages << std::endl;
}
#endif
    if (ret != 0)
        std::cerr << "thread " << name << " failed err=" << ret << std::endl;
    VERBOSE(*this, "thread " << name << "finished");
    return (ret);
}

void
ThreadRunner::get_static_counts(Stats &stats)
{
    _thread->_op.get_static_counts(stats, 1);
}

void
ThreadRunner::op_create_all(Operation *op, size_t &keysize, size_t &valuesize)
{
    op->create_all();
    if (op->is_table_op()) {
        op->kv_compute_max(true, false);
        if (OP_HAS_VALUE(op)) {
            op->kv_compute_max(
              false, (op->_table.options.random_value || _workload->options.random_table_values));
        }
        if (op->_key._keytype == Key::KEYGEN_PARETO && op->_key._pareto.param == 0)
            THROW("Key._pareto value must be set if KEYGEN_PARETO specified");
        op->kv_size_buffer(true, keysize);
        op->kv_size_buffer(false, valuesize);

        // Note: to support multiple contexts we'd need a generation
        // count whenever we execute.
        if (!op->_random_table && op->_table._internal->_context_count != 0 &&
          op->_table._internal->_context_count != _icontext->_context_count)
            THROW("multiple Contexts not supported");

        if (!op->_random_table) {
            tint_t tint = op->_table._internal->_tint;
            if (tint == 0) {
                std::string uri = op->_table._uri;

                // We are single threaded in this function, so do not have
                // to worry about locking.
                if (_icontext->_tint.count(uri) == 0) {
                    // TODO: don't use atomic add, it's overkill.
                    tint = workgen_atomic_add32(&_icontext->_tint_last, 1);
                    _icontext->_tint[uri] = tint;
                    _icontext->_table_names[tint] = uri;
                } else
                    tint = _icontext->_tint[uri];
                op->_table._internal->_tint = tint;
            }

            uint32_t usage_flags = CONTAINER_VALUE(_table_usage, op->_table._internal->_tint, 0);
            if (op->_optype == Operation::OP_SEARCH)
                usage_flags |= ThreadRunner::USAGE_READ;
            else
                usage_flags |= ThreadRunner::USAGE_WRITE;
            _table_usage[op->_table._internal->_tint] = usage_flags;
        } else {
            // Set size of vector storing thread-to-table mappings for the operation.
            if (op->_tables.size() != _wrunner->_trunners.size()) {
                op->_tables.assign(_wrunner->_trunners.size(), std::string());
            }
        }
    }
    if (op->_group != nullptr)
        for (std::vector<Operation>::iterator i = op->_group->begin(); i != op->_group->end(); i++)
            op_create_all(&*i, keysize, valuesize);
}

#define PARETO_SHAPE 1.5

/*
 * Return a value within the interval [ 0, recno_max ) that is weighted toward lower numbers with
 * pareto_param at 0 (the minimum), and more evenly distributed with pareto_param at 100 (the
 * maximum).
 */
static uint64_t
pareto_calculation(uint32_t randint, uint64_t recno_max, ParetoOptions &pareto)
{
    double r = static_cast<double>(randint);
    if (pareto.range_high != 1.0 || pareto.range_low != 0.0) {
        if (pareto.range_high <= pareto.range_low || pareto.range_high > 1.0 ||
          pareto.range_low < 0.0)
            THROW("Pareto illegal range");
        r = (pareto.range_low * static_cast<double>(UINT32_MAX)) +
          r * (pareto.range_high - pareto.range_low);
    }
    return testutil_pareto((uint64_t)r, recno_max, pareto.param);
}

uint64_t
ThreadRunner::op_get_key_recno(Operation *op, uint64_t range, tint_t tint)
{
    uint64_t recno_count;

    (void)op;
    if (range > 0)
        recno_count = range;
    else {
        if (op->_random_table) {
            const std::shared_lock lock(*_icontext->_dyn_mutex);
            recno_count = _icontext->_dyn_table_runtime.at(tint)._max_recno;
        } else
            recno_count = _icontext->_table_runtime[tint]._max_recno;
    }
    if (recno_count == 0)
        // The file has no entries, returning 0 forces a WT_NOTFOUND return.
        return (0);
    uint32_t rval = random_value();
    if (op->_key._keytype == Key::KEYGEN_PARETO)
        rval = pareto_calculation(rval, recno_count, op->_key._pareto);
    return (rval % recno_count + 1); // recnos are one-based.
}

// This runner's thread completed the operation and is no longer using the assigned
// dynamic table. Remove the (thread,table) map entry for the operation.
void
ThreadRunner::op_clear_table(Operation *op)
{
    op->_tables[_number] = std::string();
}

// Get the uri and tint for the table assigned to the specified operation for this
// runner's thread.
std::tuple<std::string, tint_t>
ThreadRunner::op_get_table(Operation *op) const
{
    if (!op->_random_table) {
        return {op->_table._uri, op->_table._internal->_tint};
    }

    std::string uri = op->_tables[_number];
    tint_t tint = 0;
    if (uri != std::string()) {
        const std::shared_lock lock(*_icontext->_dyn_mutex);
        tint = _icontext->_dyn_tint[uri];
    }
    return {uri, tint};
}

/*
 * Check if the specified operation has an assigned table. For static tables, this information is
 * saved in the Operation structure. For dynamic tables, the operation maintains a table assignment
 * for each thread running the operation.
 */
bool
ThreadRunner::op_has_table(Operation *op) const
{
    if (op->_random_table) {
        return (!op->_tables[_number].empty());
    } else {
        return (!op->_table._uri.empty());
    }
}

// Set the table uri for the thread running this operation. Used for dynamic table operations.
void
ThreadRunner::op_set_table(Operation *op, const std::string &uri)
{
    if (op->_random_table) {
        op->_tables[_number] = uri;
    }
}

// Generate and set a key and value for the operation managed by this thread.
void
ThreadRunner::op_kv_gen(Operation *op, const tint_t tint)
{
    if (!op->is_table_op()) {
        return;
    }

    /*
     * A potential race: thread1 is inserting, and increments Context->_recno[] for fileX.wt.
     * thread2 is doing one of remove/search/update and grabs the new value of Context->_recno[] for
     * fileX.wt. thread2 randomly chooses the highest recno (which has not yet been inserted by
     * thread1), and when it accesses the record will get WT_NOTFOUND. It should be somewhat rare
     * (and most likely when the threads are first beginning). Any WT_NOTFOUND returns are allowed
     * and get their own statistic bumped.
     */
    uint64_t recno = 0;
    uint64_t range = op->_table.options.range;
    if (op->_optype == Operation::OP_INSERT) {
        if (op->_key._keytype == Key::KEYGEN_APPEND || op->_key._keytype == Key::KEYGEN_AUTO) {
            if (op->_random_table) {
                const std::shared_lock lock(*_icontext->_dyn_mutex);
                recno = workgen_atomic_add64(&_icontext->_dyn_table_runtime.at(tint)._max_recno, 1);
            } else {
                recno = workgen_atomic_add64(&_icontext->_table_runtime[tint]._max_recno, 1);
            }
        } else {
            recno = op_get_key_recno(op, range, tint);
        }
    } else {
        recno = op_get_key_recno(op, range, tint);
    }

    VERBOSE(*this,
      "OP " << op->_optype << " " << _icontext->_dyn_table_names[tint].c_str()
            << ", recno=" << recno);

    // Generate the key and value for the operation.
    op->kv_gen(this, true, 100, recno, _keybuf);
    if (OP_HAS_VALUE(op)) {
        uint64_t compressibility =
          (op->_table.options.random_value || _workload->options.random_table_values) ?
          0 :
          op->_table.options.value_compressibility;
        op->kv_gen(this, false, compressibility, recno, _valuebuf);
    }
}

int
ThreadRunner::op_run_setup(Operation *op)
{
    WT_DECL_RET;

    if (_throttle != nullptr) {
        while (_throttle_ops >= _throttle_limit && !_in_transaction && !_stop) {
            /*
             * Calling throttle causes a sleep until the next time division, and we are given a new
             * batch of operations to do before calling throttle again. If the number of operations
             * in the batch is zero, we'll need to go around and throttle again.
             */
            if ((ret = _throttle->throttle(_throttle_ops, &_throttle_limit)) != 0)
                return ret;
            _throttle_ops = 0;
            if (_throttle_limit != 0)
                break;
        }
        if (op->is_table_op())
            ++_throttle_ops;
    }

    // If this is not a table operation, we have nothing more to do here.
    if (!op->is_table_op()) {
        return op_run(op);
    }

    // If the operation already has a table, it's ready to run.
    if (op_has_table(op)) {
        // If this is not a dynamic table operation, we need to generate keys and values.
        if (!op->_random_table) {
            tint_t tint = op->_table._internal->_tint;
            op_kv_gen(op, tint);
        }
        return op_run(op);
    }

    // Find a random table to operate on.
    ASSERT(op->_random_table);
    Operation base_op, mirror_op;

    {
        const std::shared_lock lock(*_icontext->_dyn_mutex);

        // Select a random base table that is not flagged for deletion.
        std::map<std::string, tint_t>::iterator itr;
        uint32_t retries = 0;
        size_t num_tables = 0;

        while (
          (num_tables = _icontext->_dyn_table_names.size()) > 0 && ++retries < TABLE_MAX_RETRIES) {
            itr = _icontext->_dyn_tint.begin();
            std::advance(itr, random_value() % num_tables);

            if (_icontext->_dyn_table_runtime[itr->second]._is_base &&
              !_icontext->_dyn_table_runtime[itr->second]._pending_delete) {
                break;
            }
        }
        if (num_tables == 0 || retries >= TABLE_MAX_RETRIES) { // Try again next time.
            return 0;
        }

        std::string op_uri = itr->first; // Get the table name.
        tint_t op_tint = itr->second;    // Get the tint.
        op_kv_gen(op, op_tint);          // Set the key and value for the operation.

        // Use atomic here as we can race with another thread that acquires the shared lock.
        (void)workgen_atomic_add32(&_icontext->_dyn_table_runtime[op_tint]._in_use, 1);

        // Do we need to mirror operations? If not, we are done here.
        if (!_icontext->_dyn_table_runtime[op_tint].has_mirror()) {
            op_set_table(op, op_uri);
            return op_run(op);
        }

        // Copy this operation to two new operations on the base table and the mirror.
        base_op = *op;
        op_set_table(&base_op, op_uri);

        mirror_op = *op;
        std::string mirror_op_uri = _icontext->_dyn_table_runtime[op_tint]._mirror;
        tint_t mirror_op_tint = _icontext->_dyn_tint[mirror_op_uri];
        op_set_table(&mirror_op, mirror_op_uri);
        (void)workgen_atomic_add32(&_icontext->_dyn_table_runtime[mirror_op_tint]._in_use, 1);
        ASSERT(!_icontext->_dyn_table_runtime[mirror_op_tint]._pending_delete);
    }

    // Create a new operation with the base and mirror table ops. Group them in a transaction
    // unless we are already in a transaction.
    Operation new_op;
    std::vector<Operation> new_group = {base_op, mirror_op};
    new_op._group = &new_group;
    new_op._repeatgroup = 1;

    Transaction txn;
    if (!_in_transaction) {
        new_op.transaction = &txn;
    }

    return op_run(&new_op);
}

int
ThreadRunner::op_run(Operation *op)
{
    Track *track;
    WT_CURSOR *cursor;
    WT_ITEM item;
    WT_DECL_RET;
    bool measure_latency, own_cursor, retry_op;
    timespec start_time;
    uint64_t time_us;
    char buf[BUF_SIZE];
    auto [table_uri, tint] = op_get_table(op);

    WT_CLEAR(item);
    track = nullptr;
    cursor = nullptr;
    own_cursor = false;
    retry_op = true;

    switch (op->_optype) {
    case Operation::OP_CHECKPOINT:
        track = &_stats.checkpoint;
        break;
    case Operation::OP_INSERT:
        track = &_stats.insert;
        break;
    case Operation::OP_LOG_FLUSH:
    case Operation::OP_NONE:
    case Operation::OP_NOOP:
        break;
    case Operation::OP_REMOVE:
        track = &_stats.remove;
        break;
    case Operation::OP_SEARCH:
        track = &_stats.read;
        break;
    case Operation::OP_UPDATE:
        track = &_stats.update;
        break;
    case Operation::OP_SLEEP:
        break;
    }

    if (op->_random_table || ((op->_internal->_flags & WORKGEN_OP_REOPEN) != 0)) {
        WT_ERR(_session->open_cursor(_session, table_uri.c_str(), nullptr, nullptr, &cursor));
        own_cursor = true;
    } else {
        cursor = _cursors[tint];
    }

    measure_latency = track != nullptr && track->ops != 0 && track->track_latency() &&
      (track->ops % _workload->options.sample_rate == 0);

    uint64_t start;
    if (measure_latency)
        workgen_clock(&start);

    // Whether or not we are measuring latency, we track how many operations
    // are in progress, or that complete.
    if (track != nullptr)
        track->begin();

    // Set the cursor for the key and value first, outside the transaction which may
    // be retried. The key and value are generated in op_run_setup.
    if (op->is_table_op()) {

        const std::string key_format(cursor->key_format);
        if (key_format == "S") {
            cursor->set_key(cursor, _keybuf);
        } else if (key_format == "u") {
            item.data = _keybuf;
            item.size = strlen(_keybuf);
            cursor->set_key(cursor, &item);
        } else {
            THROW("The key format ('" << key_format << "') must be 'u' or 'S'.");
        }
        if (OP_HAS_VALUE(op)) {

            const std::string value_format(cursor->value_format);
            if (value_format == "S") {
                cursor->set_value(cursor, _valuebuf);
            } else if (value_format == "u") {
                item.data = _valuebuf;
                item.size = strlen(_valuebuf);
                cursor->set_value(cursor, &item);
            } else {
                THROW("The value format ('" << value_format << "') must be 'u' or 'S'.");
            }
        }
    }
    // Retry on rollback until success.
    while (retry_op) {
        if (op->transaction != nullptr) {
            if (_in_transaction)
                THROW("nested transactions not supported");
            if (op->transaction->use_commit_timestamp && op->transaction->use_prepare_timestamp) {
                THROW("Either use_prepare_timestamp or use_commit_timestamp must be set.");
            }
            if (op->transaction->read_timestamp_lag > 0) {
                uint64_t read =
                  WorkgenTimeStamp::get_timestamp_lag(op->transaction->read_timestamp_lag);
                snprintf(
                  buf, BUF_SIZE, "%s=%" PRIu64, op->transaction->_begin_config.c_str(), read);
            } else {
                snprintf(buf, BUF_SIZE, "%s", op->transaction->_begin_config.c_str());
            }
            WT_ERR(_session->begin_transaction(_session, buf));

            _in_transaction = true;
        }
        if (op->is_table_op()) {
            switch (op->_optype) {
            case Operation::OP_INSERT:
                ret = cursor->insert(cursor);
                break;
            case Operation::OP_REMOVE:
                ret = cursor->remove(cursor);
                if (ret == WT_NOTFOUND)
                    ret = 0;
                break;
            case Operation::OP_SEARCH:
                ret = cursor->search(cursor);
                if (ret == WT_NOTFOUND) {
                    ret = 0;
                    track = &_stats.not_found;
                }
                break;
            case Operation::OP_UPDATE:
                ret = cursor->update(cursor);
                if (ret == WT_NOTFOUND)
                    ret = 0;
                break;
            default:
                ASSERT(false);
            }

            // Assume success and no retry unless ROLLBACK.
            retry_op = false;
            if (ret != 0 && ret != WT_ROLLBACK)
                WT_ERR(ret);
            if (ret == 0)
                cursor->reset(cursor);
            else {
                retry_op = true;
                track->rollbacks++;
                WT_ERR(_session->rollback_transaction(_session, nullptr));
                _in_transaction = false;
                ret = 0;
            }
        } else {
            // Never retry on an internal op.
            retry_op = false;
            WT_ERR(op->_internal->run(this, _session));
            _op_time_us += op->_internal->sync_time_us();
        }
    }

    if (measure_latency) {
        uint64_t stop;
        workgen_clock(&stop);
        track->complete_with_latency(ns_to_us(stop - start));
    } else if (track != nullptr)
        track->complete();

    if (op->_group != nullptr) {
        uint64_t endtime = 0;
        uint64_t now;

        if (op->_timed != 0.0)
            endtime = _op_time_us + secs_us(op->_timed);

        VERBOSE(
          *this, "GROUP operation " << op->_timed << " secs, " << op->_repeatgroup << "times");

        do {
            // Wait for transactions to complete before stopping.
            for (int count = 0; (!_stop || _in_transaction) && count < op->_repeatgroup; count++) {
                for (std::vector<Operation>::iterator i = op->_group->begin();
                     i != op->_group->end(); i++) {
                    WT_ERR(op_run_setup(&*i));
                }
            }
            workgen_clock(&now);
        } while ((!_stop || _in_transaction) && ns_to_us(now) < endtime);

        if (op->_timed != 0.0)
            _op_time_us = endtime;
    }
err:
    if (own_cursor)
        WT_TRET(cursor->close(cursor));
    if (op->transaction != nullptr) {
        if (ret != 0 || op->transaction->_rollback) {
            WT_TRET(_session->rollback_transaction(_session, nullptr));
        } else if (_in_transaction) {
            // Set prepare, commit and durable timestamp if prepare is set.
            if (op->transaction->use_prepare_timestamp) {
                time_us = WorkgenTimeStamp::get_timestamp();
                snprintf(buf, BUF_SIZE, "prepare_timestamp=%" PRIu64, time_us);
                ret = _session->prepare_transaction(_session, buf);
                snprintf(buf, BUF_SIZE, "commit_timestamp=%" PRIu64 ",durable_timestamp=%" PRIu64,
                  time_us, time_us);
                ret = _session->commit_transaction(_session, buf);
            } else if (op->transaction->use_commit_timestamp) {
                uint64_t commit_time_us = WorkgenTimeStamp::get_timestamp();
                snprintf(buf, BUF_SIZE, "commit_timestamp=%" PRIu64, commit_time_us);
                ret = _session->commit_transaction(_session, buf);
            } else {
                ret =
                  _session->commit_transaction(_session, op->transaction->_commit_config.c_str());
            }
        }
        _in_transaction = false;
    }

    if (op->_random_table) {
        const std::shared_lock lock(*_icontext->_dyn_mutex);
        // For operations on random tables, if a table has been selected, decrement the
        // reference counter.
        ASSERT(_icontext->_dyn_table_runtime[tint]._in_use > 0);
        // Use atomic here as we can race with another thread that acquires the shared lock.
        (void)workgen_atomic_sub32(&_icontext->_dyn_table_runtime[tint]._in_use, 1);
        op_clear_table(op);
    }

    return (ret);
}

#ifdef _DEBUG
std::string
ThreadRunner::get_debug()
{
    return (_debug_messages.str());
}
#endif

uint32_t
ThreadRunner::random_value()
{
    return (workgen_random(_rand_state));
}

// Generate a random 32-bit value then return a float value equally distributed
// between -1.0 and 1.0.
float
ThreadRunner::random_signed()
{
    uint32_t r = random_value();
    int sign = ((r & 0x1) == 0 ? 1 : -1);
    return ((r * sign) / static_cast<float>(UINT32_MAX));
}

Throttle::Throttle(ThreadRunner &runner, double throttle, double throttle_burst)
    : _runner(runner), _throttle(throttle), _burst(throttle_burst), _next_div(), _ops_delta(0),
      _ops_prev(0), _ops_per_div(0), _ms_per_div(0), _ops_left_this_second(throttle), _div_pos(0),
      _started(false)
{

    /*
     * Our throttling is done by dividing each second into THROTTLE_PER_SEC parts (we call the parts
     * divisions). In each division, we perform a certain number of operations. This number is
     * approximately throttle/THROTTLE_PER_SEC, except that throttle is not necessarily a multiple
     * of THROTTLE_PER_SEC, nor is it even necessarily an integer. (That way we can have 1000
     * threads each inserting 0.5 a second).
     */
    ts_clear(_next_div);
    ASSERT(1000 % THROTTLE_PER_SEC == 0); // must evenly divide
    _ms_per_div = 1000 / THROTTLE_PER_SEC;
    _ops_per_div = (uint64_t)ceill(_throttle / THROTTLE_PER_SEC);
}

/*
 * Each time throttle is called, we sleep and return a number of operations to perform next. To
 * implement this we keep a time calculation in _next_div set initially to the current time +
 * 1/THROTTLE_PER_SEC. Each call to throttle advances _next_div by 1/THROTTLE_PER_SEC, and if
 * _next_div is in the future, we sleep for the difference between the _next_div and the
 * current_time. We we return (Thread.options.throttle / THROTTLE_PER_SEC) as the number of
 * operations, if it does not divide evenly, we'll make sure to not exceed the number of operations
 * requested per second.
 *
 * The only variation is that the amount of individual sleeps is modified by a random amount (which
 * varies more widely as Thread.options.throttle_burst is greater). This has the effect of
 * randomizing how much clumping happens, and ensures that multiple threads aren't executing in lock
 * step.
 */
int
Throttle::throttle(uint64_t op_count, uint64_t *op_limit)
{
    timespec now;

    workgen_epoch(&now);
    DEBUG_CAPTURE(_runner, "throttle: ops=" << op_count);
    if (!_started) {
        _next_div = ts_add_ms(now, _ms_per_div);
        _started = true;
    } else {
        if (_burst != 0.0)
            _ops_delta += (op_count - _ops_prev);

        // Sleep until the next division, but potentially with some randomness.
        if (now < _next_div) {
            int64_t sleep_ms = ts_ms(_next_div - now);
            sleep_ms += (_ms_per_div * _burst * _runner.random_signed());
            if (sleep_ms > 0) {
                DEBUG_CAPTURE(_runner, ", sleep=" << sleep_ms);
                usleep((useconds_t)ms_to_us(sleep_ms));
            }
        }
        _next_div = ts_add_ms(_next_div, _ms_per_div);
    }

    uint64_t ops;
    if (_burst == 0.0)
        ops = _ops_left_this_second;
    else
        ops = _ops_per_div;

    if (_ops_delta < (int64_t)ops) {
        ops -= _ops_delta;
        _ops_delta = 0;
    } else {
        _ops_delta -= ops;
        ops = 0;
    }

    // Enforce that we haven't exceeded the number of operations this second.
    // Note that _ops_left_this_second may be fractional.
    if (ops > _ops_left_this_second)
        ops = (uint64_t)floorl(_ops_left_this_second);
    _ops_left_this_second -= ops;
    ASSERT(_ops_left_this_second >= 0.0);
    *op_limit = ops;
    _ops_prev = ops;

    // Advance the division, and if we pass into a new second, allocate
    // more operations into the count of operations left this second.
    _div_pos = (_div_pos + 1) % THROTTLE_PER_SEC;
    if (_div_pos == 0)
        _ops_left_this_second += _throttle;
    DEBUG_CAPTURE(_runner, ", return=" << ops << std::endl);
    return (0);
}

ThreadOptions::ThreadOptions()
    : name(), session_config(), throttle(0.0), throttle_burst(1.0), synchronized(false), _options()
{
    _options.add_string("name", name, "name of the thread");
    _options.add_string(
      "session_config", session_config, "session config which is passed to open_session");
    _options.add_double("throttle", throttle, "Limit to this number of operations per second");
    _options.add_double("throttle_burst", throttle_burst,
      "Changes characteristic of throttling from smooth (0.0) "
      "to having large bursts with lulls (10.0 or larger)");
}
ThreadOptions::ThreadOptions(const ThreadOptions &other)
    : name(other.name), session_config(other.session_config), throttle(other.throttle),
      throttle_burst(other.throttle_burst), synchronized(other.synchronized),
      _options(other._options)
{
}

void
ThreadListWrapper::extend(const ThreadListWrapper &other)
{
    for (std::vector<Thread>::const_iterator i = other._threads.begin(); i != other._threads.end();
         i++)
        _threads.push_back(*i);
}

void
ThreadListWrapper::append(const Thread &t)
{
    _threads.push_back(t);
}

void
ThreadListWrapper::multiply(const int n)
{
    if (n == 0) {
        _threads.clear();
    } else {
        std::vector<Thread> copy(_threads);
        for (int cnt = 1; cnt < n; cnt++)
            extend(copy);
    }
}

Thread::Thread() : options(), _op() {}
Thread::Thread(const Operation &op) : options(), _op(op) {}
Thread::Thread(const Thread &other) : options(other.options), _op(other._op) {}

void
Thread::describe(std::ostream &os) const
{
    os << "Thread: [" << std::endl;
    _op.describe(os);
    os << std::endl;
    os << "]";
}

Operation::Operation()
    : _optype(OP_NONE), _internal(nullptr), _table(), _key(), _value(), _config(),
      transaction(nullptr), _group(nullptr), _repeatgroup(0), _timed(0.0), _random_table(false)
{
    init_internal(nullptr);
}

Operation::Operation(OpType optype, Table table, Key key, Value value)
    : _optype(optype), _internal(nullptr), _table(table), _key(key), _value(value), _config(),
      transaction(nullptr), _group(nullptr), _repeatgroup(0), _timed(0.0), _random_table(false)
{
    init_internal(nullptr);
    size_check();
}

Operation::Operation(OpType optype, Table table, Key key)
    : _optype(optype), _internal(nullptr), _table(table), _key(key), _value(), _config(),
      transaction(nullptr), _group(nullptr), _repeatgroup(0), _timed(0.0), _random_table(false)
{
    init_internal(nullptr);
    size_check();
}

Operation::Operation(OpType optype, Table table)
    : _optype(optype), _internal(nullptr), _table(table), _key(), _value(), _config(),
      transaction(nullptr), _group(nullptr), _repeatgroup(0), _timed(0.0), _random_table(false)
{
    init_internal(nullptr);
    size_check();
}

Operation::Operation(OpType optype, Key key, Value value)
    : _optype(optype), _internal(nullptr), _table(), _key(key), _value(value), _config(),
      transaction(nullptr), _group(nullptr), _repeatgroup(0), _timed(0.0), _random_table(true)
{
    init_internal(nullptr);
    size_check();
}

Operation::Operation(const Operation &other)
    : _optype(other._optype), _internal(nullptr), _table(other._table), _key(other._key),
      _value(other._value), _config(other._config), transaction(other.transaction),
      _group(other._group), _repeatgroup(other._repeatgroup), _timed(other._timed),
      _random_table(other._random_table), _tables(other._tables)
{
    // Creation and destruction of _group and transaction is managed
    // by Python.
    init_internal(other._internal);
}

Operation::Operation(OpType optype, const std::string &config)
    : _optype(optype), _internal(nullptr), _table(), _key(), _value(), _config(config),
      transaction(nullptr), _group(nullptr), _repeatgroup(0), _timed(0.0), _random_table(false)
{
    init_internal(nullptr);
}

Operation::~Operation()
{
    // Creation and destruction of _group, transaction is managed by Python.
    delete _internal;
}

Operation &
Operation::operator=(const Operation &other)
{
    _optype = other._optype;
    _table = other._table;
    _key = other._key;
    _value = other._value;
    transaction = other.transaction;
    _group = other._group;
    _repeatgroup = other._repeatgroup;
    _timed = other._timed;
    _random_table = other._random_table;
    _tables = other._tables;
    delete _internal;
    _internal = nullptr;
    init_internal(other._internal);
    return (*this);
}

void
Operation::init_internal(OperationInternal *other)
{
    ASSERT(_internal == nullptr);

    switch (_optype) {
    case OP_CHECKPOINT:
        if (other == nullptr)
            _internal = new CheckpointOperationInternal();
        else
            _internal =
              new CheckpointOperationInternal(*static_cast<CheckpointOperationInternal *>(other));
        break;
    case OP_INSERT:
    case OP_REMOVE:
    case OP_SEARCH:
    case OP_UPDATE:
        if (other == nullptr)
            _internal = new TableOperationInternal();
        else
            _internal = new TableOperationInternal(*static_cast<TableOperationInternal *>(other));
        break;
    case OP_LOG_FLUSH:
        _internal = new LogFlushOperationInternal();
        break;
    case OP_NONE:
    case OP_NOOP:
        if (other == nullptr)
            _internal = new OperationInternal();
        else
            _internal = new OperationInternal(*other);
        break;
    case OP_SLEEP:
        if (other == nullptr)
            _internal = new SleepOperationInternal();
        else
            _internal = new SleepOperationInternal(*static_cast<SleepOperationInternal *>(other));
        break;
    default:
        ASSERT(false);
    }
}

bool
Operation::combinable() const
{
    return (_group != nullptr && _repeatgroup == 1 && _timed == 0.0 && transaction == nullptr &&
      _config.empty());
}

void
Operation::create_all()
{
    size_check();

    _internal->_flags = 0;
    _internal->parse_config(_config);
}

void
Operation::describe(std::ostream &os) const
{
    os << "Operation: " << _optype;
    if (is_table_op()) {
        os << ", ";
        _table.describe(os);
        os << ", ";
        _key.describe(os);
        os << ", ";
        _value.describe(os);
    }
    if (!_config.empty())
        os << ", '" << _config << "'";
    if (transaction != nullptr) {
        os << ", [";
        transaction->describe(os);
        os << "]";
    }
    if (_timed != 0.0)
        os << ", [timed " << _timed << " secs]";
    if (_group != nullptr) {
        os << ", group";
        if (_repeatgroup != 1)
            os << "[repeat " << _repeatgroup << "]";
        os << ": {";
        bool first = true;
        for (std::vector<Operation>::const_iterator i = _group->begin(); i != _group->end(); i++) {
            if (!first)
                os << "}, {";
            i->describe(os);
            first = false;
        }
        os << "}";
    }
}

void
Operation::get_static_counts(Stats &stats, int multiplier)
{
    if (is_table_op())
        switch (_optype) {
        case OP_INSERT:
            stats.insert.ops += multiplier;
            break;
        case OP_REMOVE:
            stats.remove.ops += multiplier;
            break;
        case OP_SEARCH:
            stats.read.ops += multiplier;
            break;
        case OP_UPDATE:
            stats.update.ops += multiplier;
            break;
        default:
            ASSERT(false);
        }
    else if (_optype == OP_CHECKPOINT)
        stats.checkpoint.ops += multiplier;

    if (_group != nullptr)
        for (std::vector<Operation>::iterator i = _group->begin(); i != _group->end(); i++)
            i->get_static_counts(stats, multiplier * _repeatgroup);
}

bool
Operation::is_table_op() const
{
    return (
      _optype == OP_INSERT || _optype == OP_REMOVE || _optype == OP_SEARCH || _optype == OP_UPDATE);
}

void
Operation::kv_compute_max(bool iskey, bool has_random)
{
    ASSERT(is_table_op());
    TableOperationInternal *internal = static_cast<TableOperationInternal *>(_internal);

    int size = iskey ? _key._size : _value._size;
    if (size == 0) {
        if (_random_table) {
            const std::string err_msg(
              "Cannot have a size of 0 when auto selecting table operations.");
            THROW(err_msg);
        } else {
            size = iskey ? _table.options.key_size : _table.options.value_size;
        }
    }

    std::string err_msg;
    err_msg = iskey ? "Key" : "Value";
    err_msg += " too small";
    if (!_random_table)
        err_msg += " for table " + _table._uri;

    if (iskey && size < 2)
        THROW(err_msg);
    if (!iskey && size < 1)
        THROW(err_msg);

    if (has_random && iskey)
        THROW("Random keys not allowed");

    uint64_t max;
    if (size > 1)
        max = power64(10, (size - 1)) - 1;
    else
        max = 0;

    if (iskey) {
        internal->_keysize = size;
        internal->_keymax = max;
    } else {
        internal->_valuesize = size;
        internal->_valuemax = max;
    }
}

void
Operation::kv_size_buffer(bool iskey, size_t &maxsize) const
{
    ASSERT(is_table_op());
    TableOperationInternal *internal = static_cast<TableOperationInternal *>(_internal);

    if (iskey) {
        if ((size_t)internal->_keysize > maxsize)
            maxsize = internal->_keysize;
    } else {
        if ((size_t)internal->_valuesize > maxsize)
            maxsize = internal->_valuesize;
    }
}

void
Operation::kv_gen(
  ThreadRunner *runner, bool iskey, uint64_t compressibility, uint64_t n, char *result) const
{
    ASSERT(is_table_op());
    TableOperationInternal *internal = static_cast<TableOperationInternal *>(_internal);

    uint_t size = iskey ? internal->_keysize : internal->_valuesize;
    uint_t max = iskey ? internal->_keymax : internal->_valuemax;
    if (n > max)
        THROW((iskey ? "Key" : "Value") << " (" << n << ") too large for size (" << size << ")");
    /* Setup the buffer, defaulting to zero filled. */
    workgen_u64_to_string_zf(n, result, size);

    /*
     * Compressibility is a percentage, 100 is all zeroes, it applies to the proportion of the value
     * that can't be used for the identifier.
     */
    if (size > 20 && compressibility < 100) {
        static const char alphanum[] =
          "0123456789"
          "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
          "abcdefghijklmnopqrstuvwxyz";
        /*
         * The random length is the proportion of the string that should not
         * be compressible. As an example a compressibility of 25 in a value
         * of length 100 should be:
         * 100 - ((100 * 25) / 100) = 75
         * That means that 75% of the string will be random numbers, and 25
         * will be easily compressible zero-fill.
         */
        uint_t random_len = size - ((size * compressibility) / 100);

        /* Never overwrite the record number identifier */
        if (random_len > size - 20)
            random_len = size - 20;

        for (uint64_t i = 0; i < random_len; ++i)
            /*
             * TODO: It'd be nice to use workgen_rand here, but this class is without the context of
             * a runner thread, so it's not easy to get access to a state.
             */
            result[i] = alphanum[runner->random_value() % (sizeof(alphanum) - 1)];
    }
}

void
Operation::size_check() const
{
    if (is_table_op()) {
        if (_key._size == 0 && _table.options.key_size == 0)
            THROW("operation requires a key size");
        if (OP_HAS_VALUE(this) && _value._size == 0 && _table.options.value_size == 0)
            THROW("operation requires a value size");
    }
}

void
Operation::synchronized_check() const
{
    if (_timed != 0.0)
        return;
    if (_optype != Operation::OP_NONE) {
        if (is_table_op() || _internal->sync_time_us() == 0)
            THROW("operation cannot be synchronized, needs to be timed()");
    } else if (_group != nullptr) {
        for (std::vector<Operation>::iterator i = _group->begin(); i != _group->end(); i++)
            i->synchronized_check();
    }
}

int
CheckpointOperationInternal::run(ThreadRunner *runner, WT_SESSION *session)
{
    (void)runner; /* not used */
    return (session->checkpoint(session, ckpt_config.c_str()));
}

int
LogFlushOperationInternal::run(ThreadRunner *runner, WT_SESSION *session)
{
    (void)runner; /* not used */
    return (session->log_flush(session, nullptr));
}

void
SleepOperationInternal::parse_config(const std::string &config)
{
    if (!config.empty())
        _sleepvalue = std::stof(config);

    if (config.empty() || _sleepvalue < 0.0)
        THROW(
          "sleep operation requires a configuration string as "
          "a non-negative float, e.g. '1.5'");
}

int
SleepOperationInternal::run(ThreadRunner *runner, WT_SESSION *session)
{
    (void)runner;  /* not used */
    (void)session; /* not used */

    uint64_t now;
    workgen_clock(&now);

    uint64_t now_us = ns_to_us(now);
    uint64_t endtime;

    if (runner->_thread->options.synchronized)
        endtime = runner->_op_time_us + secs_us(_sleepvalue);
    else
        endtime = now_us + secs_us(_sleepvalue);

    // Sleep for up to a second at a time, so we'll break out if
    // we should stop.
    while (!runner->_stop && now_us < endtime) {
        uint64_t sleep_us = endtime - now_us;
        if (sleep_us >= WT_MILLION) // one second
            sleep(1);
        else
            usleep(sleep_us);

        workgen_clock(&now);
        now_us = ns_to_us(now);
    }
    return (0);
}

uint64_t
SleepOperationInternal::sync_time_us() const
{
    return (secs_us(_sleepvalue));
}

void
CheckpointOperationInternal::parse_config(const std::string &config)
{
    if (!config.empty())
        ckpt_config = config;
}

void
TableOperationInternal::parse_config(const std::string &config)
{
    if (!config.empty()) {
        if (config == "reopen")
            _flags |= WORKGEN_OP_REOPEN;
        else
            THROW("table operation has illegal config: \"" << config << "\"");
    }
}

Track::Track(bool latency_tracking)
    : ops_in_progress(0), ops(0), rollbacks(0), latency_ops(0), latency(0), bucket_ops(0),
      min_latency(0), max_latency(0), us(nullptr), ms(nullptr), sec(nullptr)
{
    track_latency(latency_tracking);
}

Track::Track(const Track &other)
    : ops_in_progress(other.ops_in_progress), ops(other.ops), rollbacks(other.rollbacks),
      latency_ops(other.latency_ops), latency(other.latency), bucket_ops(other.bucket_ops),
      min_latency(other.min_latency), max_latency(other.max_latency), us(nullptr), ms(nullptr),
      sec(nullptr)
{
    if (other.us != nullptr) {
        us = new uint32_t[LATENCY_US_BUCKETS];
        ms = new uint32_t[LATENCY_MS_BUCKETS];
        sec = new uint32_t[LATENCY_SEC_BUCKETS];
        memcpy(us, other.us, sizeof(uint32_t) * LATENCY_US_BUCKETS);
        memcpy(ms, other.ms, sizeof(uint32_t) * LATENCY_MS_BUCKETS);
        memcpy(sec, other.sec, sizeof(uint32_t) * LATENCY_SEC_BUCKETS);
    }
}

Track::~Track()
{
    if (us != nullptr) {
        delete us;
        delete ms;
        delete sec;
    }
}

void
Track::add(Track &other, bool reset)
{
    ops_in_progress += other.ops_in_progress;
    ops += other.ops;
    latency_ops += other.latency_ops;
    latency += other.latency;

    min_latency = MIN(min_latency, other.min_latency);
    if (reset)
        other.min_latency = 0;
    max_latency = MAX(max_latency, other.max_latency);
    if (reset)
        other.max_latency = 0;

    if (us != nullptr && other.us != nullptr) {
        for (int i = 0; i < LATENCY_US_BUCKETS; i++)
            us[i] += other.us[i];
        for (int i = 0; i < LATENCY_MS_BUCKETS; i++)
            ms[i] += other.ms[i];
        for (int i = 0; i < LATENCY_SEC_BUCKETS; i++)
            sec[i] += other.sec[i];
    }
}

void
Track::assign(const Track &other)
{
    ops_in_progress = other.ops_in_progress;
    ops = other.ops;
    latency_ops = other.latency_ops;
    latency = other.latency;
    min_latency = other.min_latency;
    max_latency = other.max_latency;

    if (other.us == nullptr && us != nullptr) {
        delete us;
        delete ms;
        delete sec;
        us = nullptr;
        ms = nullptr;
        sec = nullptr;
    } else if (other.us != nullptr && us == nullptr) {
        us = new uint32_t[LATENCY_US_BUCKETS];
        ms = new uint32_t[LATENCY_MS_BUCKETS];
        sec = new uint32_t[LATENCY_SEC_BUCKETS];
    }
    if (us != nullptr) {
        memcpy(us, other.us, sizeof(uint32_t) * LATENCY_US_BUCKETS);
        memcpy(ms, other.ms, sizeof(uint32_t) * LATENCY_MS_BUCKETS);
        memcpy(sec, other.sec, sizeof(uint32_t) * LATENCY_SEC_BUCKETS);
    }
}

uint64_t
Track::average_latency() const
{
    if (latency_ops == 0)
        return (0);
    else
        return (latency / latency_ops);
}

void
Track::begin()
{
    ops_in_progress++;
}

void
Track::clear()
{
    ops_in_progress = 0;
    ops = 0;
    rollbacks = 0;
    latency_ops = 0;
    latency = 0;
    bucket_ops = 0;
    min_latency = 0;
    max_latency = 0;
    if (us != nullptr) {
        memset(us, 0, sizeof(uint32_t) * LATENCY_US_BUCKETS);
        memset(ms, 0, sizeof(uint32_t) * LATENCY_MS_BUCKETS);
        memset(sec, 0, sizeof(uint32_t) * LATENCY_SEC_BUCKETS);
    }
}

void
Track::complete()
{
    --ops_in_progress;
    ops++;
}

void
Track::complete_with_latency(uint64_t usecs)
{
    ASSERT(us != nullptr);

    --ops_in_progress;
    ops++;
    latency_ops++;
    latency += usecs;
    if (usecs > max_latency)
        max_latency = (uint32_t)usecs;
    if (usecs < min_latency)
        min_latency = (uint32_t)usecs;

    // Update a latency bucket.
    // First buckets: usecs from 100us to 1000us at 100us each.
    if (usecs < LATENCY_US_BUCKETS)
        us[usecs]++;

    // Second buckets: milliseconds from 1ms to 1000ms, at 1ms each.
    else if (usecs < ms_to_us(LATENCY_MS_BUCKETS))
        ms[us_to_ms(usecs)]++;

    // Third buckets are seconds from 1s to 100s, at 1s each.
    else if (usecs < sec_to_us(LATENCY_SEC_BUCKETS))
        sec[us_to_sec(usecs)]++;

    // >100 seconds, accumulate in the biggest bucket. */
    else
        sec[LATENCY_SEC_BUCKETS - 1]++;
}

/*
 * Return the latency for which the given percent is lower than it. E.g. for percent == 95, returns
 * the latency for which 95% of latencies are faster (lower), and 5% are slower (higher).
 */
uint64_t
Track::percentile_latency(int percent) const
{
    /*
     * Get the total number of operations in the latency buckets. We can't reliably use latency_ops,
     * because this struct was added up from Track structures that were being copied while being
     * updated.
     */
    uint64_t total = 0;
    for (int i = 0; i < LATENCY_SEC_BUCKETS; i++)
        total += sec[i];
    for (int i = 0; i < LATENCY_MS_BUCKETS; i++)
        total += ms[i];
    for (int i = 0; i < LATENCY_US_BUCKETS; i++)
        total += us[i];
    if (total == 0)
        return (0);

    // optimized for percent values over 50, we start counting from above.
    uint64_t n = 0;
    uint64_t k = (100 - percent) * total / 100;
    if (k == 0)
        return (0);
    for (int i = LATENCY_SEC_BUCKETS - 1; i >= 0; --i) {
        n += sec[i];
        if (n >= k)
            return (sec_to_us(i));
    }
    for (int i = LATENCY_MS_BUCKETS - 1; i >= 0; --i) {
        n += ms[i];
        if (n >= k)
            return (ms_to_us(i));
    }
    for (int i = LATENCY_US_BUCKETS - 1; i >= 0; --i) {
        n += us[i];
        if (n >= k)
            return (100 * i);
    }
    // We should have accounted for all the buckets.
    ASSERT(false);
    return (0);
}

void
Track::subtract(const Track &other)
{
    ops_in_progress -= other.ops_in_progress;
    ops -= other.ops;
    latency_ops -= other.latency_ops;
    latency -= other.latency;

    // There's no sensible thing to be done for min/max_latency.

    if (us != nullptr && other.us != nullptr) {
        for (int i = 0; i < LATENCY_US_BUCKETS; i++)
            us[i] -= other.us[i];
        for (int i = 0; i < LATENCY_MS_BUCKETS; i++)
            ms[i] -= other.ms[i];
        for (int i = 0; i < LATENCY_SEC_BUCKETS; i++)
            sec[i] -= other.sec[i];
    }
}

void
Track::track_latency(bool newval)
{
    if (newval) {
        if (us == nullptr) {
            us = new uint32_t[LATENCY_US_BUCKETS];
            ms = new uint32_t[LATENCY_MS_BUCKETS];
            sec = new uint32_t[LATENCY_SEC_BUCKETS];
            memset(us, 0, sizeof(uint32_t) * LATENCY_US_BUCKETS);
            memset(ms, 0, sizeof(uint32_t) * LATENCY_MS_BUCKETS);
            memset(sec, 0, sizeof(uint32_t) * LATENCY_SEC_BUCKETS);
        }
    } else {
        if (us != nullptr) {
            delete us;
            delete ms;
            delete sec;
            us = nullptr;
            ms = nullptr;
            sec = nullptr;
        }
    }
}

void
Track::_get_us(long *result)
{
    if (us != nullptr) {
        for (int i = 0; i < LATENCY_US_BUCKETS; i++)
            result[i] = (long)us[i];
    } else
        memset(result, 0, sizeof(long) * LATENCY_US_BUCKETS);
}
void
Track::_get_ms(long *result)
{
    if (ms != nullptr) {
        for (int i = 0; i < LATENCY_MS_BUCKETS; i++)
            result[i] = (long)ms[i];
    } else
        memset(result, 0, sizeof(long) * LATENCY_MS_BUCKETS);
}
void
Track::_get_sec(long *result)
{
    if (sec != nullptr) {
        for (int i = 0; i < LATENCY_SEC_BUCKETS; i++)
            result[i] = (long)sec[i];
    } else
        memset(result, 0, sizeof(long) * LATENCY_SEC_BUCKETS);
}

Stats::Stats(bool latency)
    : checkpoint(latency), insert(latency), not_found(latency), read(latency), remove(latency),
      update(latency), truncate(latency)
{
}

Stats::Stats(const Stats &other)
    : checkpoint(other.checkpoint), insert(other.insert), not_found(other.not_found),
      read(other.read), remove(other.remove), update(other.update), truncate(other.truncate)
{
}

void
Stats::add(Stats &other, bool reset)
{
    checkpoint.add(other.checkpoint, reset);
    insert.add(other.insert, reset);
    not_found.add(other.not_found, reset);
    read.add(other.read, reset);
    remove.add(other.remove, reset);
    update.add(other.update, reset);
    truncate.add(other.truncate, reset);
}

void
Stats::assign(const Stats &other)
{
    checkpoint.assign(other.checkpoint);
    insert.assign(other.insert);
    not_found.assign(other.not_found);
    read.assign(other.read);
    remove.assign(other.remove);
    update.assign(other.update);
    truncate.assign(other.truncate);
}

void
Stats::clear()
{
    checkpoint.clear();
    insert.clear();
    not_found.clear();
    read.clear();
    remove.clear();
    update.clear();
    truncate.clear();
}

void
Stats::describe(std::ostream &os) const
{
    os << "Stats: reads " << read.ops;
    if (not_found.ops > 0) {
        os << " (" << not_found.ops << " not found)";
    }
    os << ", inserts " << insert.ops;
    os << ", updates " << update.ops;
    os << ", truncates " << truncate.ops;
    os << ", removes " << remove.ops;
    os << ", checkpoints " << checkpoint.ops;
}

void
Stats::final_report(std::ostream &os, timespec &totalsecs) const
{
    uint64_t ops = 0;
    ops += checkpoint.ops;
    ops += read.ops;
    ops += not_found.ops;
    ops += insert.ops;
    ops += update.ops;
    ops += truncate.ops;
    ops += remove.ops;

#define FINAL_OUTPUT(os, field, singular, ops, totalsecs)                                   \
    os << "Executed " << field << " " #singular " operations (" << PCT(field, ops) << "%) " \
       << OPS_PER_SEC(field, totalsecs) << " ops/sec" << std::endl

    FINAL_OUTPUT(os, read.ops, read, ops, totalsecs);
    FINAL_OUTPUT(os, not_found.ops, not found, ops, totalsecs);
    FINAL_OUTPUT(os, insert.ops, insert, ops, totalsecs);
    FINAL_OUTPUT(os, update.ops, update, ops, totalsecs);
    FINAL_OUTPUT(os, truncate.ops, truncate, ops, totalsecs);
    FINAL_OUTPUT(os, remove.ops, remove, ops, totalsecs);
    FINAL_OUTPUT(os, checkpoint.ops, checkpoint, ops, totalsecs);
}

void
Stats::report(std::ostream &os) const
{
    os << read.ops << " reads";
    if (not_found.ops > 0) {
        os << " (" << not_found.ops << " not found)";
    }
    os << ", " << insert.ops << " inserts, ";
    os << update.ops << " updates, ";
    os << truncate.ops << " truncates, ";
    os << remove.ops << " removes, ";
    os << checkpoint.ops << " checkpoints";
}

void
Stats::subtract(const Stats &other)
{
    checkpoint.subtract(other.checkpoint);
    insert.subtract(other.insert);
    not_found.subtract(other.not_found);
    read.subtract(other.read);
    remove.subtract(other.remove);
    update.subtract(other.update);
    truncate.subtract(other.truncate);
}

void
Stats::track_latency(bool latency)
{
    checkpoint.track_latency(latency);
    insert.track_latency(latency);
    not_found.track_latency(latency);
    read.track_latency(latency);
    remove.track_latency(latency);
    update.track_latency(latency);
    truncate.track_latency(latency);
}

TableOptions::TableOptions()
    : key_size(0), value_size(0), value_compressibility(100), random_value(false), range(0),
      _options()
{
    _options.add_int(
      "key_size", key_size, "default size of the key, unless overridden by Key.size");
    _options.add_int(
      "value_size", value_size, "default size of the value, unless overridden by Value.size");
    _options.add_bool("random_value", random_value, "generate random content for the value");
    _options.add_bool("value_compressibility", value_compressibility,
      "How compressible the generated value should be");
    _options.add_int("range", range,
      "if zero, keys are inserted at the end and reads/updates are in the current range, if "
      "non-zero, inserts/reads/updates are at a random key between 0 and the given range");
}
TableOptions::TableOptions(const TableOptions &other)
    : key_size(other.key_size), value_size(other.value_size),
      value_compressibility(other.value_compressibility), random_value(other.random_value),
      range(other.range), _options(other._options)
{
}

Table::Table() : options(), _uri(), _internal(new TableInternal()) {}
Table::Table(const std::string &uri) : options(), _uri(uri), _internal(new TableInternal()) {}
Table::Table(const Table &other)
    : options(other.options), _uri(other._uri), _internal(new TableInternal(*other._internal))
{
}
Table::~Table()
{
    delete _internal;
}
Table &
Table::operator=(const Table &other)
{
    options = other.options;
    _uri = other._uri;
    *_internal = *other._internal;
    return (*this);
}

void
Table::describe(std::ostream &os) const
{
    os << "Table: " << _uri;
}

TableInternal::TableInternal() : _tint(0), _context_count(0) {}
TableInternal::TableInternal(const TableInternal &other)
    : _tint(other._tint), _context_count(other._context_count)
{
}

WorkloadOptions::WorkloadOptions()
    : max_latency(0), report_enabled(true), report_file("workload.stat"), report_interval(0),
      run_time(0), sample_file("monitor.json"), sample_interval_ms(0), max_idle_table_cycle(0),
      sample_rate(1), warmup(0), oldest_timestamp_lag(0.0), stable_timestamp_lag(0.0),
      timestamp_advance(0.0), max_idle_table_cycle_fatal(false), create_count(0),
      create_interval(0), create_prefix(""), create_target(0), create_trigger(0), drop_count(0),
      drop_interval(0), drop_target(0), drop_trigger(0), random_table_values(false),
      mirror_tables(false), mirror_suffix("_mirror"), _options()
{
    _options.add_int("max_latency", max_latency,
      "prints warning if any latency measured exceeds this number of "
      "milliseconds. Requires sample_interval to be configured.");
    _options.add_int("report_interval", report_interval,
      "output throughput information every interval seconds, 0 to disable");
    _options.add_bool("report_enabled", report_enabled, "Enable collecting run output");
    _options.add_string("report_file", report_file,
      "file name for collecting run output, "
      "including output from the report_interval option. "
      "The file name is relative to the connection's home directory. "
      "When set to the empty string, stdout is used.");
    _options.add_int("run_time", run_time, "total workload seconds");
    _options.add_string("sample_file", sample_file,
      "file name for collecting latency output in a JSON-like format, "
      "enabled by the report_interval option. "
      "The file name is relative to the connection's home directory. "
      "When set to the empty string, no JSON is emitted.");
    _options.add_int("sample_interval_ms", sample_interval_ms,
      "performance logging every interval milliseconds, 0 to disable");
    _options.add_int("max_idle_table_cycle", max_idle_table_cycle,
      "maximum number of seconds a create or drop is allowed before aborting "
      "or printing a warning based on max_idle_table_cycle_fatal setting.");
    _options.add_int("sample_rate", sample_rate,
      "how often the latency of operations is measured. 1 for every operation, "
      "2 for every second operation, 3 for every third operation etc.");
    _options.add_int(
      "warmup", warmup, "how long to run the workload phase before starting measurements");
    _options.add_double("oldest_timestamp_lag", oldest_timestamp_lag,
      "how much lag to the oldest timestamp from epoch time");
    _options.add_double("stable_timestamp_lag", stable_timestamp_lag,
      "how much lag to the oldest timestamp from epoch time");
    _options.add_double("timestamp_advance", timestamp_advance,
      "how many seconds to wait before moving oldest and stable"
      "timestamp forward");
    _options.add_bool("max_idle_table_cycle_fatal", max_idle_table_cycle_fatal,
      "print warning (false) or abort (true) of max_idle_table_cycle failure");
    _options.add_int("create_interval", create_interval,
      "table creation frequency in seconds. The number of tables created is specified by"
      " the create_count setting");
    _options.add_int("create_count", create_count, "number of tables to create each time interval");
    _options.add_string(
      "create_prefix", create_prefix, "the prefix to prepend to the auto generated table names");
    _options.add_int("create_trigger", create_trigger,
      "if specified, start creating tables when the database drops below this size in MB");
    _options.add_int("create_target", create_target,
      "if specified, stop creating tables when the database exceeds this size in MB");
    _options.add_int("drop_interval", drop_interval,
      "table drop frequency in seconds. The number of tables dropped is specified by"
      " the drop_count setting");
    _options.add_int("drop_count", drop_count, "number of tables to drop each time interval");
    _options.add_int("drop_trigger", drop_trigger,
      "if specified, start dropping tables when the database exceeds this size in MB");
    _options.add_int("drop_target", drop_target,
      "if specified, stop dropping tables when the database drops below this size in MB");
    _options.add_bool(
      "random_table_values", random_table_values, "generate random content for the table value");
    _options.add_bool("mirror_tables", mirror_tables, "mirror database operations");
    _options.add_string(
      "mirror_suffix", mirror_suffix, "the suffix to append to mirrored table names");
}

WorkloadOptions::WorkloadOptions(const WorkloadOptions &other)
    : max_latency(other.max_latency), report_interval(other.report_interval),
      run_time(other.run_time), sample_interval_ms(other.sample_interval_ms),
      sample_rate(other.sample_rate), _options(other._options)
{
}

Workload::Workload(Context *context, const ThreadListWrapper &tlw)
    : options(), stats(), _context(context), _threads(tlw._threads)
{
    if (context == nullptr)
        THROW("Workload constructor requires a Context");
}

Workload::Workload(Context *context, const Thread &thread)
    : options(), stats(), _context(context), _threads()
{
    if (context == nullptr)
        THROW("Workload constructor requires a Context");
    _threads.push_back(thread);
}

Workload::Workload(const Workload &other)
    : options(other.options), stats(other.stats), _context(other._context), _threads(other._threads)
{
}

Workload &
Workload::operator=(const Workload &other)
{
    options = other.options;
    stats.assign(other.stats);
    *_context = *other._context;
    _threads = other._threads;
    return (*this);
}

int
Workload::run(WT_CONNECTION *conn)
{
    WorkloadRunner runner(this);
    return (runner.run(conn));
}

WorkloadRunner::WorkloadRunner(Workload *workload)
    : _workload(workload), _rand_state(nullptr), _trunners(workload->_threads.size()),
      _report_out(&std::cout), _start(), stopping(false)
{
    ts_clear(_start);
}

WorkloadRunner::~WorkloadRunner()
{
    if (_rand_state != nullptr) {
        workgen_random_free(_rand_state);
        _rand_state = nullptr;
    }
}

int
WorkloadRunner::run(WT_CONNECTION *conn)
{
    WT_DECL_RET;
    WorkloadOptions *options = &_workload->options;

    _wt_home = conn->get_home(conn);

    if ((options->oldest_timestamp_lag > 0 || options->stable_timestamp_lag > 0) &&
      options->timestamp_advance < 0)
        THROW(
          "Workload.options.timestamp_advance must be positive if either "
          "Workload.options.oldest_timestamp_lag or Workload.options.stable_timestamp_lag is "
          "set");

    if (options->sample_interval_ms > 0 && options->sample_rate <= 0)
        THROW("Workload.options.sample_rate must be positive");

    std::ofstream report_out;
    if (options->report_enabled && !options->report_file.empty()) {
        open_report_file(report_out, options->report_file, "Workload.options.report_file");
        _report_out = &report_out;
    }

    /* Create a randomizer for the workload before we do anything else. */
    WT_SESSION *session;
    WT_ERR(conn->open_session(conn, nullptr, nullptr, &session));
    WT_ERR(workgen_random_alloc(session, &_rand_state));

    /* Initiate everything else, and start the workload. */
    WT_ERR(create_all(conn, _workload->_context));
    WT_ERR(open_all());
    WT_ERR(ThreadRunner::cross_check(_trunners));
    WT_ERR(run_all(conn));
err:
    // TODO: (void)close_all();
    _report_out = &std::cout;
    return (ret);
}

int
WorkloadRunner::open_all()
{
    for (size_t i = 0; i < _trunners.size(); i++) {
        WT_RET(_trunners[i].open_all());
    }
    return (0);
}

void
WorkloadRunner::open_report_file(
  std::ofstream &of, const std::string &filename, const std::string &desc)
{
    std::stringstream sstm;

    if (!_wt_home.empty())
        sstm << _wt_home << "/";
    sstm << filename;
    of.open(sstm.str().c_str(), std::fstream::app);
    if (!of)
        THROW_ERRNO(errno, desc << ": \"" << sstm.str() << "\" could not be opened");
}

int
WorkloadRunner::create_all(WT_CONNECTION *conn, Context *context)
{
    for (size_t i = 0; i < _trunners.size(); i++) {
        ThreadRunner *runner = &_trunners[i];
        std::stringstream sstm;
        Thread *thread = &_workload->_threads[i];
        if (thread->options.name.empty()) {
            sstm << "thread" << i;
            thread->options.name = sstm.str();
        }
        runner->_thread = thread;
        runner->_context = context;
        runner->_icontext = context->_internal;
        runner->_workload = _workload;
        runner->_wrunner = this;
        runner->_number = (uint32_t)i;
        // TODO: recover from partial failure here
        WT_RET(runner->create_all(conn));
    }
    WT_RET(context->_internal->create_all(conn));
    return (0);
}

int
WorkloadRunner::close_all()
{
    for (size_t i = 0; i < _trunners.size(); i++)
        _trunners[i].close_all();

    return (0);
}

void
WorkloadRunner::get_stats(Stats *result)
{
    for (size_t i = 0; i < _trunners.size(); i++)
        result->add(_trunners[i]._stats);
}

void
WorkloadRunner::report(time_t interval, time_t totalsecs, Stats *prev_totals)
{
    std::ostream &out = *_report_out;
    Stats new_totals(prev_totals->track_latency());

    get_stats(&new_totals);
    Stats diff(new_totals);
    diff.subtract(*prev_totals);
    prev_totals->assign(new_totals);
    diff.report(out);
    out << " in " << interval << " secs (" << totalsecs << " total secs)" << std::endl;
}

void
WorkloadRunner::final_report(timespec &totalsecs)
{
    std::ostream &out = *_report_out;
    Stats *stats = &_workload->stats;

    stats->clear();
    stats->track_latency(_workload->options.sample_interval_ms > 0);

    get_stats(stats);
    stats->final_report(out, totalsecs);
    out << "Run completed: " << totalsecs << " seconds" << std::endl;
}

int
WorkloadRunner::run_all(WT_CONNECTION *conn)
{
    WT_DECL_RET;

    // Register signal handlers for SIGINT (Ctrl-C) and SIGTERM.
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    Stats counts(false);
    for (size_t i = 0; i < _trunners.size(); i++)
        _trunners[i].get_static_counts(counts);

    if (_workload->options.report_enabled) {
        std::ostream &out = *_report_out;
        out << "Starting workload: " << _trunners.size() << " threads, ";
        counts.report(out);
        out << std::endl;
    }

    // Start all threads
    WorkloadOptions *options = &_workload->options;
    Monitor monitor(*this);
    std::ofstream monitor_out;
    std::ofstream monitor_json;
    if (options->sample_interval_ms > 0) {
        open_report_file(monitor_out, "monitor", "monitor output file");
        monitor._out = &monitor_out;

        if (!options->sample_file.empty()) {
            open_report_file(monitor_json, options->sample_file, "sample JSON output file");
            monitor._json = &monitor_json;
        }

        if ((ret = pthread_create(&monitor._handle, nullptr, monitor_main, &monitor)) != 0) {
            std::cerr << "monitor thread failed err=" << ret << std::endl;
            return (ret);
        }
    }

    void *status;
    std::vector<pthread_t> thread_handles;
    for (size_t i = 0; i < _trunners.size(); i++) {
        pthread_t thandle;
        ThreadRunner *runner = &_trunners[i];
        runner->_stop = false;
        runner->_repeat = (options->run_time != 0);
        if ((ret = pthread_create(&thandle, nullptr, thread_runner_main, runner)) != 0) {
            std::cerr << "pthread_create failed err=" << ret << std::endl;
            std::cerr << "Stopping all threads." << std::endl;
            for (size_t j = 0; j < thread_handles.size(); j++) {
                _trunners[j]._stop = true;
                (void)pthread_join(thread_handles[j], &status);
                _trunners[j].close_all();
            }
            return (ret);
        }
        thread_handles.push_back(thandle);
    }

    // Start Timestamp increment thread
    pthread_t time_thandle;
    WorkloadRunnerConnection *runnerConnection = nullptr;
    if (options->oldest_timestamp_lag > 0 || options->stable_timestamp_lag > 0) {
        runnerConnection = new WorkloadRunnerConnection();
        runnerConnection->runner = this;
        runnerConnection->connection = conn;
        if ((ret = pthread_create(&time_thandle, nullptr, thread_workload, runnerConnection)) !=
          0) {
            std::cerr << "pthread_create failed err=" << ret << std::endl;
            delete runnerConnection;
            runnerConnection = nullptr;
            stopping = true;
        }
    }

    // Start Idle table cycle thread
    pthread_t idle_table_thandle;
    WorkloadRunnerConnection *createDropTableCycle = nullptr;
    if (options->max_idle_table_cycle > 0) {
        createDropTableCycle = new WorkloadRunnerConnection();
        createDropTableCycle->runner = this;
        createDropTableCycle->connection = conn;
        if ((ret = pthread_create(&idle_table_thandle, nullptr, thread_idle_table_cycle_workload,
               createDropTableCycle)) != 0) {
            std::cerr << "pthread_create failed err=" << ret << std::endl;
            delete createDropTableCycle;
            createDropTableCycle = nullptr;
            stopping = true;
        }
    }

    // Start a thread to create tables
    pthread_t tables_create_thandle;
    WorkloadRunnerConnection *tableCreate = nullptr;
    if (options->create_interval > 0) {
        if (options->create_count < 1) {
            std::cerr << "create_count needs to be greater than 0" << std::endl;
            stopping = true;
        } else if (options->create_target > 0 && options->create_trigger <= 0) {
            std::cerr << "Need to specify a create_trigger when setting a create_target."
                      << std::endl;
            stopping = true;
        } else if (options->create_trigger > 0 && options->create_target <= 0) {
            std::cerr << "Need to specify a create_target when setting a create_trigger."
                      << std::endl;
            stopping = true;
        } else if (options->create_trigger != 0 &&
          options->create_target < options->create_trigger) {
            std::cerr << "create_target should be greater than create_trigger." << std::endl;
            stopping = true;
        } else {
            tableCreate = new WorkloadRunnerConnection();
            tableCreate->runner = this;
            tableCreate->connection = conn;
            if ((ret = pthread_create(&tables_create_thandle, nullptr,
                   thread_tables_create_workload, tableCreate)) != 0) {
                std::cerr << "pthread_create failed err=" << ret << std::endl;
                delete tableCreate;
                tableCreate = nullptr;
                stopping = true;
            }
        }
    }

    // Start a thread to drop tables
    pthread_t tables_drop_thandle;
    WorkloadRunnerConnection *tableDrop = nullptr;
    if (options->drop_interval > 0) {
        if (options->drop_count < 1) {
            std::cerr << "drop_count needs to be greater than 0" << std::endl;
            stopping = true;
        } else if (options->drop_target > 0 && options->drop_trigger <= 0) {
            std::cerr << "Need to specify a drop_trigger when setting a drop_target." << std::endl;
            stopping = true;
        } else if (options->drop_trigger > 0 && options->drop_target <= 0) {
            std::cerr << "Need to specify a drop_target when setting a drop_trigger." << std::endl;
            stopping = true;
        } else if (options->drop_target != 0 && options->drop_trigger < options->drop_target) {
            std::cerr << "drop_trigger should be greater than drop_target." << std::endl;
            stopping = true;
        } else {
            tableDrop = new WorkloadRunnerConnection();
            tableDrop->runner = this;
            tableDrop->connection = conn;
            if ((ret = pthread_create(
                   &tables_drop_thandle, nullptr, thread_tables_drop_workload, tableDrop)) != 0) {
                std::cerr << "pthread_create failed err=" << ret << std::endl;
                delete tableDrop;
                tableDrop = nullptr;
                stopping = true;
            }
        }
    }

    timespec now;

    /* Don't run the test if any of the above pthread_create fails. */
    if (!stopping && ret == 0) {
        // Treat warmup separately from report interval so that if we have a
        // warmup period we clear and ignore stats after it ends.
        if (options->warmup != 0)
            sleep((unsigned int)options->warmup);

        // Clear stats after any warmup period completes.
        for (size_t i = 0; i < _trunners.size(); i++) {
            ThreadRunner *runner = &_trunners[i];
            runner->_stats.clear();
        }

        workgen_epoch(&_start);
        timespec end = _start + options->run_time;
        timespec next_report = _start + options->report_interval;

        // Let the test run, reporting as needed. Exit when we exceed the run time or
        // when a registered signal is received.
        Stats curstats(false);
        now = _start;
        while (now < end && !signal_raised) {
            timespec sleep_amt;

            sleep_amt = end - now;
            if (next_report != 0) {
                timespec next_diff = next_report - now;
                if (next_diff < next_report)
                    sleep_amt = next_diff;
            }
            if (sleep_amt.tv_sec > 0)
                sleep((unsigned int)sleep_amt.tv_sec);
            else
                usleep((useconds_t)((sleep_amt.tv_nsec + 999) / 1000));

            workgen_epoch(&now);
            if (now >= next_report && now < end && options->report_interval != 0) {
                report(options->report_interval, (now - _start).tv_sec, &curstats);
                while (now >= next_report)
                    next_report += options->report_interval;
            }
        }
    }

    // signal all threads to stop.
    if (options->run_time != 0)
        for (size_t i = 0; i < _trunners.size(); i++)
            _trunners[i]._stop = true;
    if (options->sample_interval_ms > 0)
        monitor._stop = true;

    // Signal timestamp and idle table cycle thread to stop.
    stopping = true;

    // wait for all threads
    WorkgenException *exception = nullptr;
    for (size_t i = 0; i < _trunners.size(); i++) {
        WT_TRET(pthread_join(thread_handles[i], &status));
        if (_trunners[i]._errno != 0)
            VERBOSE(_trunners[i], "Thread " << i << " has errno " << _trunners[i]._errno);
        WT_TRET(_trunners[i]._errno);
        _trunners[i].close_all();
        if (exception == nullptr && !_trunners[i]._exception._str.empty())
            exception = &_trunners[i]._exception;
    }

    // Wait for the time increment thread
    if (runnerConnection != nullptr) {
        WT_TRET(pthread_join(time_thandle, &status));
        delete runnerConnection;
    }

    // Wait for the idle table cycle thread.
    if (createDropTableCycle != nullptr) {
        WT_TRET(pthread_join(idle_table_thandle, &status));
        delete createDropTableCycle;
    }

    // Wait for the table create table thread.
    if (tableCreate != nullptr) {
        WT_TRET(pthread_join(tables_create_thandle, &status));
        delete tableCreate;
    }

    // Wait for the table drop table thread.
    if (tableDrop != nullptr) {
        WT_TRET(pthread_join(tables_drop_thandle, &status));
        delete tableDrop;
    }

    workgen_epoch(&now);
    if (options->sample_interval_ms > 0) {
        WT_TRET(pthread_join(monitor._handle, &status));
        if (monitor._errno != 0)
            std::cerr << "Monitor thread has errno " << monitor._errno << std::endl;
        if (exception == nullptr && !monitor._exception._str.empty())
            exception = &monitor._exception;

        monitor_out.close();
        if (!options->sample_file.empty())
            monitor_json.close();
    }

    // Issue the final report.
    if (options->report_enabled) {
        timespec finalsecs = now - _start;
        final_report(finalsecs);
    }

    if (ret != 0)
        std::cerr << "run_all failed err=" << ret << std::endl;
    if (options->report_enabled)
        (*_report_out) << std::endl;
    if (exception != nullptr)
        throw *exception;
    return (ret);
}

} // namespace workgen
