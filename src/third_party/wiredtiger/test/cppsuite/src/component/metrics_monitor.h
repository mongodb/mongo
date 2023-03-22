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

#ifndef METRICS_MONITOR_H
#define METRICS_MONITOR_H

#include <memory>
#include <string>
#include <vector>

#include "src/main/configuration.h"
#include "src/main/database.h"
#include "src/storage/scoped_cursor.h"
#include "src/storage/scoped_session.h"
#include "statistics/statistics.h"

namespace test_harness {

/*
 * The statistics monitor class is designed to track various statistics or other runtime signals
 * relevant to the given workload.
 */
class metrics_monitor : public component {
public:
    static void get_stat(scoped_cursor &, int, int64_t *);

public:
    metrics_monitor(const std::string &test_name, configuration *config, database &database);
    virtual ~metrics_monitor() = default;

    /* Delete the copy constructor and the assignment operator. */
    metrics_monitor(const metrics_monitor &) = delete;
    metrics_monitor &operator=(const metrics_monitor &) = delete;

    void load() override final;
    void do_work() override final;
    void finish() override final;

private:
    scoped_session _session;
    scoped_cursor _cursor;
    const std::string _test_name;
    std::vector<std::unique_ptr<statistics>> _stats;
    database &_database;
};
} // namespace test_harness

#endif
