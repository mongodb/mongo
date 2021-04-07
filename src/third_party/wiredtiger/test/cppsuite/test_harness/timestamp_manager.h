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

#ifndef TIMESTAMP_MANAGER_H
#define TIMESTAMP_MANAGER_H

#include <atomic>
#include <chrono>
#include <sstream>
#include <thread>

#include "component.h"

namespace test_harness {
/*
 * The timestamp monitor class manages global timestamp state for all components in the test
 * harness. It also manages the global timestamps within WiredTiger. All timestamps are in seconds
 * unless specified otherwise.
 */
class timestamp_manager : public component {
    public:
    timestamp_manager(configuration *config)
        : /* _periodic_update_s is hardcoded to 1 second for now. */
          component(config), _increment_ts(0U), _latest_ts(0U), _oldest_lag(0), _oldest_ts(0U),
          _periodic_update_s(1), _stable_lag(0), _stable_ts(0U)
    {
    }

    void
    load()
    {
        testutil_assert(_config != nullptr);
        testutil_check(_config->get_int(OLDEST_LAG, _oldest_lag));
        testutil_assert(_oldest_lag >= 0);
        testutil_check(_config->get_int(STABLE_LAG, _stable_lag));
        testutil_assert(_stable_lag >= 0);
        testutil_check(_config->get_bool(ENABLED, _enabled));
        component::load();
    }

    void
    run()
    {
        std::string config;
        /* latest_ts_s represents the time component of the latest timestamp provided. */
        wt_timestamp_t latest_ts_s;

        while (_enabled && _running) {
            /* Timestamps are checked periodically. */
            std::this_thread::sleep_for(std::chrono::seconds(_periodic_update_s));
            latest_ts_s = (_latest_ts >> 32);
            /*
             * Keep a time window between the latest and stable ts less than the max defined in the
             * configuration.
             */
            testutil_assert(latest_ts_s >= _stable_ts);
            if ((latest_ts_s - _stable_ts) > _stable_lag) {
                _stable_ts = latest_ts_s - _stable_lag;
                config += std::string(STABLE_TS) + "=" + decimal_to_hex(_stable_ts);
            }

            /*
             * Keep a time window between the stable and oldest ts less than the max defined in the
             * configuration.
             */
            testutil_assert(_stable_ts >= _oldest_ts);
            if ((_stable_ts - _oldest_ts) > _oldest_lag) {
                _oldest_ts = _stable_ts - _oldest_lag;
                if (!config.empty())
                    config += ",";
                config += std::string(OLDEST_TS) + "=" + decimal_to_hex(_oldest_ts);
            }

            /* Save the new timestamps. */
            if (!config.empty()) {
                connection_manager::instance().set_timestamp(config);
                config = "";
            }
        }
    }

    /*
     * Get a unique commit timestamp. The first 32 bits represent the epoch time in seconds. The
     * last 32 bits represent an increment for uniqueness.
     */
    wt_timestamp_t
    get_next_ts()
    {
        uint64_t current_time = get_time_now_s();
        _increment_ts.fetch_add(1);

        current_time = (current_time << 32) | (_increment_ts & 0x00000000FFFFFFFF);
        _latest_ts = current_time;

        return (_latest_ts);
    }

    static const std::string
    decimal_to_hex(int64_t value)
    {
        std::stringstream ss;
        ss << std::hex << value;
        std::string res(ss.str());
        return (res);
    }

    private:
    uint64_t
    get_time_now_s() const
    {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        uint64_t current_time_s =
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
        return (current_time_s);
    }

    private:
    const wt_timestamp_t _periodic_update_s;
    std::atomic<wt_timestamp_t> _increment_ts;
    wt_timestamp_t _latest_ts, _oldest_ts, _stable_ts;
    /*
     * _oldest_lag is the time window between the stable and oldest timestamps.
     * _stable_lag is the time window between the latest and stable timestamps.
     */
    int64_t _oldest_lag, _stable_lag;
};
} // namespace test_harness

#endif
