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

#include "timestamp_manager.h"

#include <sstream>

#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/common/random_generator.h"
#include "src/storage/connection_manager.h"

namespace test_harness {
const std::string
timestamp_manager::decimal_to_hex(uint64_t value)
{
    std::stringstream ss;
    ss << std::hex << value;
    std::string res(ss.str());
    return (res);
}

timestamp_manager::timestamp_manager(configuration *config) : component("timestamp_manager", config)
{
}

void
timestamp_manager::load()
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
timestamp_manager::do_work()
{
    std::string config, log_msg;
    /* latest_ts_s represents the time component of the latest timestamp provided. */
    wt_timestamp_t latest_ts_s = get_time_now_s();

    /*
     * Keep a time window between the latest and stable ts less than the max defined in the
     * configuration.
     */
    wt_timestamp_t new_stable_ts = _stable_ts;
    testutil_assert(latest_ts_s >= _stable_ts);
    if ((latest_ts_s - _stable_ts) > _stable_lag) {
        log_msg = "Timestamp_manager: Stable timestamp expired.";
        new_stable_ts = latest_ts_s - _stable_lag;
        config += STABLE_TS + "=" + decimal_to_hex(new_stable_ts);
    }

    /*
     * Keep a time window between the stable and oldest ts less than the max defined in the
     * configuration.
     */
    wt_timestamp_t new_oldest_ts = _oldest_ts;
    testutil_assert(_stable_ts >= _oldest_ts);
    if ((new_stable_ts - _oldest_ts) > _oldest_lag) {
        if (log_msg.empty())
            log_msg = "Timestamp_manager: Oldest timestamp expired.";
        else
            log_msg += " Oldest timestamp expired.";
        new_oldest_ts = new_stable_ts - _oldest_lag;
        if (!config.empty())
            config += ",";
        config += OLDEST_TS + "=" + decimal_to_hex(new_oldest_ts);
    }

    if (!log_msg.empty())
        logger::log_msg(LOG_TRACE, log_msg);

    /*
     * Save the new timestamps. Any timestamps that we're viewing from another thread should be set
     * AFTER we've saved the new timestamps to avoid races where we sweep data that is not yet
     * obsolete.
     */
    if (!config.empty()) {
        connection_manager::instance().set_timestamp(config);
        _oldest_ts = new_oldest_ts;
        _stable_ts = new_stable_ts;
    }
}

wt_timestamp_t
timestamp_manager::get_next_ts()
{
    uint64_t current_time = get_time_now_s();
    uint64_t increment = _increment_ts.fetch_add(1);
    current_time |= (increment & 0x00000000FFFFFFFF);
    return (current_time);
}

wt_timestamp_t
timestamp_manager::get_oldest_ts() const
{
    return (_oldest_ts);
}

wt_timestamp_t
timestamp_manager::get_valid_read_ts() const
{
    /* Use get_oldest_ts here to convert from atomic to wt_timestamp_t. */
    wt_timestamp_t current_oldest = get_oldest_ts();
    wt_timestamp_t current_stable = _stable_ts;
    if (current_stable > current_oldest) {
        --current_stable;
    }
    /*
     * Assert that our stable and oldest match if 0 or that the stable is greater than or equal to
     * the oldest. Ensuring that the oldest is never greater than the stable.
     */
    testutil_assert(
      (current_stable == 0 && current_oldest == 0) || current_stable >= current_oldest);
    /*
     * Its okay to return a timestamp less than a concurrently updated oldest timestamp as all
     * readers should be reading with timestamp rounding.
     */
    return random_generator::instance().generate_integer<wt_timestamp_t>(
      current_oldest, current_stable);
}

uint64_t
timestamp_manager::get_time_now_s() const
{
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t current_time_s =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count()) << 32;
    return (current_time_s);
}
} // namespace test_harness
