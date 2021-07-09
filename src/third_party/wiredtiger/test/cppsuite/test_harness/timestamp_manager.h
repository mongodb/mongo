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

#include "core/component.h"

namespace test_harness {
/*
 * The timestamp monitor class manages global timestamp state for all components in the test
 * harness. It also manages the global timestamps within WiredTiger.
 *
 * The format of a timestamp is as follows, the first 32 bits represent the epoch time in seconds.
 * The last 32 bits represent an increment for uniqueness.
 */
class timestamp_manager : public component {
    public:
    timestamp_manager(configuration *config) : component("timestamp_manager", config) {}

    void
    load() override final
    {
        component::load();
        int64_t oldest_lag = _config->get_int(OLDEST_LAG);
        testutil_assert(oldest_lag >= 0);
        /* Cast and then shift left to match the seconds position. */
        _oldest_lag = oldest_lag;
        _oldest_lag <<= 32;

        int64_t stable_lag = _config->get_int(STABLE_LAG);
        testutil_assert(stable_lag >= 0);
        /* Cast and then shift left to match the seconds position. */
        _stable_lag = stable_lag;
        _stable_lag <<= 32;
    }

    void
    do_work() override final
    {
        std::string config;
        /* latest_ts_s represents the time component of the latest timestamp provided. */
        wt_timestamp_t latest_ts_s;

        /* Timestamps are checked periodically. */
        latest_ts_s = get_time_now_s();

        /*
         * Keep a time window between the latest and stable ts less than the max defined in the
         * configuration.
         */
        testutil_assert(latest_ts_s >= _stable_ts);
        if ((latest_ts_s - _stable_ts) > _stable_lag) {
            log_msg(LOG_INFO, "Timestamp_manager: Stable timestamp expired.");
            _stable_ts = latest_ts_s;
            config += std::string(STABLE_TS) + "=" + decimal_to_hex(_stable_ts);
        }

        /*
         * Keep a time window between the stable and oldest ts less than the max defined in the
         * configuration.
         */
        wt_timestamp_t new_oldest_ts = _oldest_ts;
        testutil_assert(_stable_ts >= _oldest_ts);
        if ((_stable_ts - _oldest_ts) > _oldest_lag) {
            log_msg(LOG_INFO, "Timestamp_manager: Oldest timestamp expired.");
            new_oldest_ts = _stable_ts - _oldest_lag;
            if (!config.empty())
                config += ",";
            config += std::string(OLDEST_TS) + "=" + decimal_to_hex(new_oldest_ts);
        }

        /*
         * Save the new timestamps. Any timestamps that we're viewing from another thread should be
         * set AFTER we've saved the new timestamps to avoid races where we sweep data that is not
         * yet obsolete.
         */
        if (!config.empty()) {
            connection_manager::instance().set_timestamp(config);
            _oldest_ts = new_oldest_ts;
        }
    }

    /*
     * Get a unique timestamp.
     */
    wt_timestamp_t
    get_next_ts()
    {
        uint64_t current_time = get_time_now_s();
        uint64_t increment = _increment_ts.fetch_add(1);
        current_time |= (increment & 0x00000000FFFFFFFF);
        return (current_time);
    }

    static const std::string
    decimal_to_hex(uint64_t value)
    {
        std::stringstream ss;
        ss << std::hex << value;
        std::string res(ss.str());
        return (res);
    }

    wt_timestamp_t
    get_oldest_ts() const
    {
        return (_oldest_ts);
    }

    private:
    /* Get the current time in seconds, bit shifted to the expected location. */
    uint64_t
    get_time_now_s() const
    {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        uint64_t current_time_s =
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count())
          << 32;
        return (current_time_s);
    }

    private:
    std::atomic<wt_timestamp_t> _increment_ts{0};
    /* The tracking table sweep needs to read the oldest timestamp. */
    std::atomic<wt_timestamp_t> _oldest_ts{0U};
    wt_timestamp_t _stable_ts = 0U;
    /*
     * _oldest_lag is the time window between the stable and oldest timestamps.
     * _stable_lag is the time window between the latest and stable timestamps.
     */
    uint64_t _oldest_lag = 0U, _stable_lag = 0U;
};
} // namespace test_harness

#endif
