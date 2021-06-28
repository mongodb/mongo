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

extern "C" {
#include "wiredtiger.h"
}

#include "util/debug_utils.h"
#include "util/api_const.h"
#include "core/component.h"
#include "core/throttle.h"
#include "connection_manager.h"
#include "workload/database_operation.h"

namespace test_harness {
/* Static statistic get function. */
static void
get_stat(scoped_cursor &cursor, int stat_field, int64_t *valuep)
{
    const char *desc, *pvalue;
    cursor->set_key(cursor.get(), stat_field);
    testutil_check(cursor->search(cursor.get()));
    testutil_check(cursor->get_value(cursor.get(), &desc, &pvalue, valuep));
}

class statistic {
    public:
    explicit statistic(configuration *config)
    {
        _enabled = config->get_bool(ENABLED);
    }

    /* Check that the given statistic is within bounds. */
    virtual void check(scoped_cursor &cursor) = 0;

    /* Suppress warning about destructor being non-virtual. */
    virtual ~statistic() {}

    bool
    enabled() const
    {
        return (_enabled);
    }

    protected:
    bool _enabled = false;
};

class cache_limit_statistic : public statistic {
    public:
    explicit cache_limit_statistic(configuration *config) : statistic(config)
    {
        limit = config->get_int(LIMIT);
    }

    void
    check(scoped_cursor &cursor) override final
    {
        testutil_assert(cursor.get() != nullptr);
        int64_t cache_bytes_image, cache_bytes_other, cache_bytes_max;
        double use_percent;
        /* Three statistics are required to compute cache use percentage. */
        get_stat(cursor, WT_STAT_CONN_CACHE_BYTES_IMAGE, &cache_bytes_image);
        get_stat(cursor, WT_STAT_CONN_CACHE_BYTES_OTHER, &cache_bytes_other);
        get_stat(cursor, WT_STAT_CONN_CACHE_BYTES_MAX, &cache_bytes_max);
        /*
         * Assert that we never exceed our configured limit for cache usage. Add 0.0 to avoid
         * floating point conversion errors.
         */
        use_percent = ((cache_bytes_image + cache_bytes_other + 0.0) / cache_bytes_max) * 100;
        if (use_percent > limit) {
            const std::string error_string =
              "runtime_monitor: Cache usage exceeded during test! Limit: " + std::to_string(limit) +
              " usage: " + std::to_string(use_percent);
            testutil_die(-1, error_string.c_str());
        } else
            debug_print("Cache usage: " + std::to_string(use_percent), DEBUG_TRACE);
    }

    private:
    int64_t limit;
};

static std::string
collection_name_to_file_name(const std::string &collection_name)
{
    /* Strip out the URI prefix. */
    const size_t colon_pos = collection_name.find(':');
    testutil_assert(colon_pos != std::string::npos);
    const auto stripped_name = collection_name.substr(colon_pos + 1);

    /* Now add the directory and file extension. */
    return std::string(DEFAULT_DIR) + "/" + stripped_name + ".wt";
}

class db_size_statistic : public statistic {
    public:
    db_size_statistic(configuration *config, database &database)
        : statistic(config), _database(database)
    {
        _limit = config->get_int(LIMIT);
#ifdef _WIN32
        debug_print("Database size checking is not implemented on Windows", DEBUG_ERROR);
#endif
    }
    virtual ~db_size_statistic() = default;

    /* Don't need the stat cursor for this. */
    void
    check(scoped_cursor &) override final
    {
        const auto file_names = get_file_names();
#ifndef _WIN32
        size_t db_size = 0;
        for (const auto &name : file_names) {
            struct stat sb;
            if (stat(name.c_str(), &sb) == 0) {
                db_size += sb.st_size;
                debug_print(name + " was " + std::to_string(sb.st_size) + " bytes", DEBUG_TRACE);
            } else
                /* The only good reason for this to fail is if the file hasn't been created yet. */
                testutil_assert(errno == ENOENT);
        }
        debug_print("Current database size is " + std::to_string(db_size) + " bytes", DEBUG_TRACE);
        if (db_size > _limit) {
            const std::string error_string =
              "runtime_monitor: Database size limit exceeded during test! Limit: " +
              std::to_string(_limit) + " db size: " + std::to_string(db_size);
            testutil_die(-1, error_string.c_str());
        }
#else
        static_cast<void>(file_names);
        static_cast<void>(_database);
        static_cast<void>(_limit);
#endif
    }

    private:
    std::vector<std::string>
    get_file_names()
    {
        std::vector<std::string> file_names;
        for (const auto &name : _database.get_collection_names())
            file_names.push_back(collection_name_to_file_name(name));

        /* Add WiredTiger internal tables. */
        file_names.push_back(std::string(DEFAULT_DIR) + "/" + WT_HS_FILE);
        file_names.push_back(std::string(DEFAULT_DIR) + "/" + WT_METAFILE);

        return file_names;
    }

    database &_database;
    int64_t _limit;
};

/*
 * The runtime monitor class is designed to track various statistics or other runtime signals
 * relevant to the given workload.
 */
class runtime_monitor : public component {
    public:
    runtime_monitor(configuration *config, database &database)
        : component("runtime_monitor", config), _database(database)
    {
    }

    ~runtime_monitor()
    {
        for (auto &it : _stats)
            delete it;
        _stats.clear();
    }

    /* Delete the copy constructor and the assignment operator. */
    runtime_monitor(const runtime_monitor &) = delete;
    runtime_monitor &operator=(const runtime_monitor &) = delete;

    void
    load() override final
    {
        configuration *sub_config;
        std::string statistic_list;

        /* Load the general component things. */
        component::load();

        if (_enabled) {
            _session = connection_manager::instance().create_session();

            /* Open our statistic cursor. */
            _cursor = _session.open_scoped_cursor(STATISTICS_URI);

            /* Load known statistics. */
            sub_config = _config->get_subconfig(STAT_CACHE_SIZE);
            _stats.push_back(new cache_limit_statistic(sub_config));
            delete sub_config;

            sub_config = _config->get_subconfig(STAT_DB_SIZE);
            _stats.push_back(new db_size_statistic(sub_config, _database));
            delete sub_config;
        }
    }

    void
    do_work() override final
    {
        for (const auto &it : _stats) {
            if (it->enabled())
                it->check(_cursor);
        }
    }

    private:
    scoped_session _session;
    scoped_cursor _cursor;
    std::vector<statistic *> _stats;
    database &_database;
};
} // namespace test_harness

#endif
