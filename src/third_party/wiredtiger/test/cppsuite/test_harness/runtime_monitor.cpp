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

#include <fstream>

#include "connection_manager.h"
#include "core/component.h"
#include "core/configuration.h"
#include "core/throttle.h"
#include "runtime_monitor.h"
#include "util/api_const.h"
#include "util/logger.h"

namespace test_harness {

/* Static methods implementation. */
static std::string
collection_name_to_file_name(const std::string &collection_name)
{
    /* Strip out the URI prefix. */
    const size_t colon_pos = collection_name.find(':');
    testutil_assert(colon_pos != std::string::npos);
    const auto stripped_name = collection_name.substr(colon_pos + 1);

    /* Now add the directory and file extension. */
    return (std::string(DEFAULT_DIR) + "/" + stripped_name + ".wt");
}

/* Inline methods implementation. */

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

/* statistics class implementation */
statistics::statistics(configuration &config, const std::string &stat_name, int stat_field)
    : field(stat_field), max(config.get_int(MAX)), min(config.get_int(MIN)), name(stat_name),
      postrun(config.get_bool(POSTRUN_STATISTICS)), runtime(config.get_bool(RUNTIME_STATISTICS)),
      save(config.get_bool(SAVE))
{
}

void
statistics::check(scoped_cursor &cursor)
{
    int64_t stat_value;
    runtime_monitor::get_stat(cursor, field, &stat_value);
    if (stat_value < min || stat_value > max) {
        const std::string error_string = "runtime_monitor: Postrun stat \"" + name +
          "\" was outside of the specified limits. Min=" + std::to_string(min) +
          " Max=" + std::to_string(max) + " Actual=" + std::to_string(stat_value);
        testutil_die(-1, error_string.c_str());
    } else
        logger::log_msg(LOG_TRACE, name + " usage: " + std::to_string(stat_value));
}

std::string
statistics::get_value_str(scoped_cursor &cursor)
{
    int64_t stat_value;
    runtime_monitor::get_stat(cursor, field, &stat_value);
    return std::to_string(stat_value);
}

int
statistics::get_field() const
{
    return field;
}

int64_t
statistics::get_max() const
{
    return max;
}

int64_t
statistics::get_min() const
{
    return min;
}

const std::string &
statistics::get_name() const
{
    return name;
}

bool
statistics::get_postrun() const
{
    return postrun;
}

bool
statistics::get_runtime() const
{
    return runtime;
}

bool
statistics::get_save() const
{
    return save;
}

/* cache_limit_statistic class implementation */
cache_limit_statistic::cache_limit_statistic(configuration &config, const std::string &name)
    : statistics(config, name, -1)
{
}

void
cache_limit_statistic::check(scoped_cursor &cursor)
{
    double use_percent = get_cache_value(cursor);
    if (use_percent > max) {
        const std::string error_string =
          "runtime_monitor: Cache usage exceeded during test! Limit: " + std::to_string(max) +
          " usage: " + std::to_string(use_percent);
        testutil_die(-1, error_string.c_str());
    } else
        logger::log_msg(LOG_TRACE, name + " usage: " + std::to_string(use_percent));
}

std::string
cache_limit_statistic::get_value_str(scoped_cursor &cursor)
{
    return std::to_string(get_cache_value(cursor));
}

double
cache_limit_statistic::get_cache_value(scoped_cursor &cursor)
{
    int64_t cache_bytes_image, cache_bytes_other, cache_bytes_max;
    double use_percent;
    /* Three statistics are required to compute cache use percentage. */
    runtime_monitor::get_stat(cursor, WT_STAT_CONN_CACHE_BYTES_IMAGE, &cache_bytes_image);
    runtime_monitor::get_stat(cursor, WT_STAT_CONN_CACHE_BYTES_OTHER, &cache_bytes_other);
    runtime_monitor::get_stat(cursor, WT_STAT_CONN_CACHE_BYTES_MAX, &cache_bytes_max);
    /*
     * Assert that we never exceed our configured limit for cache usage. Add 0.0 to avoid floating
     * point conversion errors.
     */
    testutil_assert(cache_bytes_max > 0);
    use_percent = ((cache_bytes_image + cache_bytes_other + 0.0) / cache_bytes_max) * 100;
    return use_percent;
}

/* db_size_statistic class implementation */
db_size_statistic::db_size_statistic(
  configuration &config, const std::string &name, database &database)
    : statistics(config, name, -1), _database(database)
{
#ifdef _WIN32
    Logger::log_msg("Database size checking is not implemented on Windows", LOG_ERROR);
#endif
}

void
db_size_statistic::check(scoped_cursor &)
{
#ifndef _WIN32
    const auto file_names = get_file_names();
    size_t db_size = get_db_size();
    logger::log_msg(LOG_TRACE, "Current database size is " + std::to_string(db_size) + " bytes");

    if (db_size > max) {
        const std::string error_string =
          "runtime_monitor: Database size limit exceeded during test! Limit: " +
          std::to_string(max) + " db size: " + std::to_string(db_size);
        testutil_die(-1, error_string.c_str());
    }
#endif
}

std::string
db_size_statistic::get_value_str(scoped_cursor &)
{
    return std::to_string(get_db_size());
}

size_t
db_size_statistic::get_db_size() const
{
    const auto file_names = get_file_names();
    size_t db_size = 0;

    for (const auto &name : file_names) {
        struct stat sb;
        if (stat(name.c_str(), &sb) == 0) {
            db_size += sb.st_size;
            logger::log_msg(LOG_TRACE, name + " was " + std::to_string(sb.st_size) + " bytes");
        } else
            /* The only good reason for this to fail is if the file hasn't been created yet. */
            testutil_assert(errno == ENOENT);
    }

    return db_size;
}

const std::vector<std::string>
db_size_statistic::get_file_names() const
{
    std::vector<std::string> file_names;
    for (const auto &name : _database.get_collection_names())
        file_names.push_back(collection_name_to_file_name(name));

    /* Add WiredTiger internal tables. */
    file_names.push_back(std::string(DEFAULT_DIR) + "/" + WT_HS_FILE);
    file_names.push_back(std::string(DEFAULT_DIR) + "/" + WT_METAFILE);

    return (file_names);
}

/* runtime_monitor class implementation */
void
runtime_monitor::get_stat(scoped_cursor &cursor, int stat_field, int64_t *valuep)
{
    const char *desc, *pvalue;
    cursor->set_key(cursor.get(), stat_field);
    testutil_check(cursor->search(cursor.get()));
    testutil_check(cursor->get_value(cursor.get(), &desc, &pvalue, valuep));
    testutil_check(cursor->reset(cursor.get()));
}

runtime_monitor::runtime_monitor(
  const std::string &test_name, configuration *config, database &database)
    : component("runtime_monitor", config), _test_name(test_name), _database(database)
{
}

void
runtime_monitor::load()
{
    /* Load the general component things. */
    component::load();

    /* If the component is enabled, load all the known statistics. */
    if (_enabled) {

        std::unique_ptr<configuration> stat_config(_config->get_subconfig(STAT_CACHE_SIZE));
        _stats.push_back(std::unique_ptr<cache_limit_statistic>(
          new cache_limit_statistic(*stat_config, STAT_CACHE_SIZE)));

        stat_config.reset(_config->get_subconfig(STAT_DB_SIZE));
        _stats.push_back(std::unique_ptr<db_size_statistic>(
          new db_size_statistic(*stat_config, STAT_DB_SIZE, _database)));

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
runtime_monitor::do_work()
{
    /* Check runtime statistics. */
    for (const auto &stat : _stats) {
        if (stat->get_runtime())
            stat->check(_cursor);
    }
}

void
runtime_monitor::finish()
{
    component::finish();

    /* Save stats. */
    save_stats(_test_name);

    /* Check the post run statistics now. */
    bool success = true;
    int64_t stat_max, stat_min, stat_value;
    std::string stat_name;

    for (const auto &stat : _stats) {

        if (!stat->get_postrun())
            continue;

        stat_max = stat->get_max();
        stat_min = stat->get_min();
        stat_name = stat->get_name();

        stat_value = std::stoi(stat->get_value_str(_cursor));

        if (stat_value < stat_min || stat_value > stat_max) {
            const std::string error_string = "runtime_monitor: Postrun stat \"" + stat_name +
              "\" was outside of the specified limits. Min=" + std::to_string(stat_min) +
              " Max=" + std::to_string(stat_max) + " Actual=" + std::to_string(stat_value);
            logger::log_msg(LOG_ERROR, error_string);
            success = false;
        }

        logger::log_msg(LOG_INFO,
          "runtime_monitor: Final value of stat " + stat_name +
            " is: " + std::to_string(stat_value));
    }

    if (!success)
        testutil_die(-1,
          "runtime_monitor: One or more postrun statistics were outside of their specified "
          "limits.");
}

/*
 * This function generates a file that contains the values of the different statistics that need to
 * be saved as indicated by the configuration file.
 */
void
runtime_monitor::save_stats(const std::string &filename)
{
    std::string stat_info = "[{\"info\":{\"test_name\": \"" + filename + "\"},\"metrics\": [";

    for (const auto &stat : _stats) {
        if (stat->get_save())
            stat_info += "{\"name\":\"" + stat->get_name() +
              "\",\"value\":" + stat->get_value_str(_cursor) + "},";
    }

    /* Remove last extra comma. */
    if (stat_info.back() == ',')
        stat_info.pop_back();

    std::ofstream file(filename + ".json");
    file << stat_info << "]}]";
    file.close();
}

} // namespace test_harness
