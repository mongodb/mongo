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

#include <thread>

extern "C" {
#include "wiredtiger.h"
}

#include "api_const.h"
#include "component.h"
#include "connection_manager.h"
#include "debug_utils.h"

namespace test_harness {
/* Static statistic get function. */
static void
get_stat(WT_CURSOR *cursor, int stat_field, int64_t *valuep)
{
    const char *desc, *pvalue;
    cursor->set_key(cursor, stat_field);
    testutil_check(cursor->search(cursor));
    testutil_check(cursor->get_value(cursor, &desc, &pvalue, valuep));
}

class statistic {
    public:
    statistic(configuration *config)
    {
        testutil_assert(config != nullptr);
        testutil_check(config->get_bool(ENABLED, _enabled));
    }

    /* Check that the given statistic is within bounds. */
    virtual void check(WT_CURSOR *cursor) = 0;

    /* Suppress warning about destructor being non-virtual. */
    virtual ~statistic() {}

    bool
    is_enabled() const
    {
        return _enabled;
    }

    protected:
    bool _enabled = false;
};

class cache_limit_statistic : public statistic {
    public:
    cache_limit_statistic(configuration *config) : statistic(config)
    {
        testutil_check(config->get_int(LIMIT, limit));
    }

    void
    check(WT_CURSOR *cursor)
    {
        testutil_assert(cursor != nullptr);
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
            std::string error_string =
              "runtime_monitor: Cache usage exceeded during test! Limit: " + std::to_string(limit) +
              " usage: " + std::to_string(use_percent);
            debug_print(error_string, DEBUG_ERROR);
            testutil_assert(use_percent < limit);
        } else
            debug_print("Cache usage: " + std::to_string(use_percent), DEBUG_TRACE);
    }

    private:
    int64_t limit;
};

/*
 * The runtime monitor class is designed to track various statistics or other runtime signals
 * relevant to the given workload.
 */
class runtime_monitor : public component {
    public:
    runtime_monitor(configuration *config) : component(config), _ops(1) {}

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
    load()
    {
        configuration *sub_config;
        std::string statistic_list;
        /* Parse the configuration for the runtime monitor. */
        testutil_check(_config->get_int(RATE_PER_SECOND, _ops));

        /* Load known statistics. */
        sub_config = _config->get_subconfig(STAT_CACHE_SIZE);
        _stats.push_back(new cache_limit_statistic(sub_config));
        delete sub_config;
        component::load();
    }

    void
    run()
    {
        WT_SESSION *session = connection_manager::instance().create_session();
        WT_CURSOR *cursor = nullptr;

        /* Open a statistics cursor. */
        testutil_check(session->open_cursor(session, STATISTICS_URI, nullptr, nullptr, &cursor));

        while (_running) {
            /* Sleep so that we do x operations per second. To be replaced by throttles. */
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / _ops));
            for (const auto &it : _stats) {
                if (it->is_enabled())
                    it->check(cursor);
            }
        }
    }

    private:
    int64_t _ops;
    std::vector<statistic *> _stats;
};
} // namespace test_harness

#endif
