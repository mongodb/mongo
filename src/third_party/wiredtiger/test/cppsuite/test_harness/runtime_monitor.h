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

#ifndef RUNTIME_MONITOR_H
#define RUNTIME_MONITOR_H

#include <string>
#include <vector>

extern "C" {
#include "test_util.h"
#include "wiredtiger.h"
}

#include "util/scoped_types.h"
#include "workload/database_model.h"

/* Forward declarations for classes to reduce compilation time and modules coupling. */
class configuration;

namespace test_harness {

class statistics {
    public:
    statistics() = default;
    explicit statistics(configuration &config, const std::string &stat_name, int stat_field);
    virtual ~statistics() = default;

    /* Check that the statistics are within bounds. */
    virtual void check(scoped_cursor &cursor);

    /* Retrieve the value associated to the stat in a string format. */
    virtual std::string get_value_str(scoped_cursor &cursor);

    /* Getters. */
    int get_field() const;
    int64_t get_max() const;
    int64_t get_min() const;
    const std::string &get_name() const;
    bool get_postrun() const;
    bool get_runtime() const;
    bool get_save() const;

    protected:
    int field;
    int64_t max;
    int64_t min;
    std::string name;
    bool postrun;
    bool runtime;
    bool save;
};

class cache_limit_statistic : public statistics {
    public:
    explicit cache_limit_statistic(configuration &config, const std::string &name);
    virtual ~cache_limit_statistic() = default;

    void check(scoped_cursor &cursor) override final;
    std::string get_value_str(scoped_cursor &cursor) override final;

    private:
    double get_cache_value(scoped_cursor &cursor);
};

class db_size_statistic : public statistics {
    public:
    explicit db_size_statistic(configuration &config, const std::string &name, database &database);
    virtual ~db_size_statistic() = default;

    /* Don't need the stat cursor for these. */
    void check(scoped_cursor &) override final;
    std::string get_value_str(scoped_cursor &) override final;

    private:
    size_t get_db_size() const;
    const std::vector<std::string> get_file_names() const;

    private:
    database &_database;
};

/*
 * The runtime monitor class is designed to track various statistics or other runtime signals
 * relevant to the given workload.
 */
class runtime_monitor : public component {
    public:
    static void get_stat(scoped_cursor &, int, int64_t *);

    public:
    explicit runtime_monitor(
      const std::string &test_name, configuration *config, database &database);
    virtual ~runtime_monitor() = default;

    /* Delete the copy constructor and the assignment operator. */
    runtime_monitor(const runtime_monitor &) = delete;
    runtime_monitor &operator=(const runtime_monitor &) = delete;

    void load() override final;
    void do_work() override final;
    void finish() override final;

    private:
    void save_stats(const std::string &filename);

    private:
    scoped_session _session;
    scoped_cursor _cursor;
    const std::string _test_name;
    std::vector<std::unique_ptr<statistics>> _stats;
    database &_database;
};
} // namespace test_harness

#endif
