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

#include "metrics_monitor.h"

#include <fstream>

#include "metrics_writer.h"
#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/storage/connection_manager.h"
#include "statistics/cache_limit.h"
#include "statistics/database_size.h"
#include "statistics/statistics.h"

extern "C" {
#include "test_util.h"
}

namespace test_harness {

/*
 * The WiredTiger configuration API doesn't accept string statistic names when retrieving statistic
 * values. This function provides the required mapping to statistic id. We should consider
 * generating it programmatically in `stat.py` to avoid having to manually add a condition every
 * time we want to observe a new postrun statistic.
 */
inline int
get_stat_field(const std::string &name)
{
    if (name == CACHE_HS_INSERT)
        return (WT_STAT_CONN_CACHE_HS_INSERT);
    else if (name == CC_PAGES_REMOVED)
        return (WT_STAT_CONN_CC_PAGES_REMOVED);
    testutil_die(EINVAL, "get_stat_field: Stat \"%s\" is unrecognized", name.c_str());
}

void
metrics_monitor::get_stat(scoped_cursor &cursor, int stat_field, int64_t *valuep)
{
    const char *desc, *pvalue;
    cursor->set_key(cursor.get(), stat_field);
    testutil_check(cursor->search(cursor.get()));
    testutil_check(cursor->get_value(cursor.get(), &desc, &pvalue, valuep));
    testutil_check(cursor->reset(cursor.get()));
}

metrics_monitor::metrics_monitor(
  const std::string &test_name, configuration *config, database &database)
    : component(METRICS_MONITOR, config), _test_name(test_name), _database(database)
{
}

void
metrics_monitor::load()
{
    /* Load the general component things. */
    component::load();

    /* If the component is enabled, load all the known statistics. */
    if (_enabled) {

        std::unique_ptr<configuration> stat_config(_config->get_subconfig(STAT_CACHE_SIZE));
        _stats.push_back(
          std::unique_ptr<cache_limit>(new cache_limit(*stat_config, STAT_CACHE_SIZE)));

        stat_config.reset(_config->get_subconfig(STAT_DB_SIZE));
        _stats.push_back(
          std::unique_ptr<database_size>(new database_size(*stat_config, STAT_DB_SIZE, _database)));

        stat_config.reset(_config->get_subconfig(CACHE_HS_INSERT));
        _stats.push_back(std::unique_ptr<statistics>(
          new statistics(*stat_config, CACHE_HS_INSERT, get_stat_field(CACHE_HS_INSERT))));

        stat_config.reset(_config->get_subconfig(CC_PAGES_REMOVED));
        _stats.push_back(std::unique_ptr<statistics>(
          new statistics(*stat_config, CC_PAGES_REMOVED, get_stat_field(CC_PAGES_REMOVED))));

        /* Open our statistic cursor. */
        _session = connection_manager::instance().create_session();
        _cursor = _session.open_scoped_cursor(STATISTICS_URI);
    }
}

void
metrics_monitor::do_work()
{
    /* Check runtime statistics. */
    for (const auto &stat : _stats) {
        if (stat->get_runtime())
            stat->check(_cursor);
    }
}

void
metrics_monitor::finish()
{
    component::finish();

    bool success = true;

    for (const auto &stat : _stats) {

        const std::string stat_name = stat->get_name();

        /* Append stats to the statistics writer if it needs to be saved. */
        if (stat->get_save()) {
            auto stat_str =
              "{\"name\":\"" + stat_name + "\",\"value\":" + stat->get_value_str(_cursor) + "}";
            metrics_writer::instance().add_stat(stat_str);
        }

        if (!stat->get_postrun())
            continue;

        int64_t stat_max = stat->get_max();
        int64_t stat_min = stat->get_min();
        int64_t stat_value = std::stoi(stat->get_value_str(_cursor));

        if (stat_value < stat_min || stat_value > stat_max) {
            const std::string error_string = "metrics_monitor: Post-run stat \"" + stat_name +
              "\" was outside of the specified limits. Min=" + std::to_string(stat_min) +
              " Max=" + std::to_string(stat_max) + " Actual=" + std::to_string(stat_value);
            logger::log_msg(LOG_ERROR, error_string);
            success = false;
        }

        logger::log_msg(LOG_INFO,
          "metrics_monitor: Final value of stat " + stat_name +
            " is: " + std::to_string(stat_value));
    }

    if (!success)
        testutil_die(-1,
          "metrics_monitor: One or more postrun statistics were outside of their specified "
          "limits.");
}
} // namespace test_harness
