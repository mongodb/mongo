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

class runtime_statistic {
    public:
    explicit runtime_statistic(configuration *config);
    virtual ~runtime_statistic() = default;

    /* Check that the given statistic is within bounds. */
    virtual void check(scoped_cursor &cursor) = 0;

    bool enabled() const;

    protected:
    bool _enabled = false;
};

class cache_limit_statistic : public runtime_statistic {
    public:
    explicit cache_limit_statistic(configuration *config);
    virtual ~cache_limit_statistic() = default;

    void check(scoped_cursor &cursor) override final;

    private:
    int64_t _limit;
};

class db_size_statistic : public runtime_statistic {
    public:
    explicit db_size_statistic(configuration *config, database &database);
    virtual ~db_size_statistic() = default;

    /* Don't need the stat cursor for this. */
    void check(scoped_cursor &) override final;

    private:
    std::vector<std::string> get_file_names();

    private:
    database &_database;
    int64_t _limit;
};

class postrun_statistic_check {
    public:
    explicit postrun_statistic_check(configuration *config);

    void check(scoped_cursor &cursor) const;

    private:
    struct postrun_statistic {
        postrun_statistic(std::string &&name, const int64_t min_limit, const int64_t max_limit);

        const std::string name;
        const int field;
        const int64_t min_limit, max_limit;
    };

    private:
    bool check_stat(scoped_cursor &cursor, const postrun_statistic &stat) const;

    private:
    std::vector<postrun_statistic> _stats;
};

/*
 * The runtime monitor class is designed to track various statistics or other runtime signals
 * relevant to the given workload.
 */
class runtime_monitor : public component {
    public:
    static void get_stat(scoped_cursor &, int, int64_t *);

    public:
    explicit runtime_monitor(configuration *config, database &database);
    ~runtime_monitor();

    /* Delete the copy constructor and the assignment operator. */
    runtime_monitor(const runtime_monitor &) = delete;
    runtime_monitor &operator=(const runtime_monitor &) = delete;

    void load() override final;
    void do_work() override final;
    void finish() override final;

    private:
    scoped_session _session;
    scoped_cursor _cursor;
    std::vector<runtime_statistic *> _stats;
    postrun_statistic_check _postrun_stats;
    database &_database;
};
} // namespace test_harness

#endif
