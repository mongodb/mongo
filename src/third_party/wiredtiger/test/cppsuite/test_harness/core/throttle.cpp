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

#include <thread>

#include "configuration.h"
#include "test_harness/util/api_const.h"
#include "throttle.h"

namespace test_harness {
throttle::throttle(const std::string &throttle_rate)
{
    std::string magnitude;
    uint64_t multiplier = 0;
    /*
     * Find the ms, s, or m in the string. Searching for "ms" first as the following two searches
     * would match as well.
     */
    size_t pos = throttle_rate.find("ms");
    if (pos != std::string::npos)
        multiplier = 1;
    else {
        pos = throttle_rate.find("s");
        if (pos != std::string::npos)
            multiplier = 1000;
        else {
            pos = throttle_rate.find("m");
            if (pos != std::string::npos)
                multiplier = 60 * 1000;
            else
                testutil_die(-1, "no rate specifier given");
        }
    }
    magnitude = throttle_rate.substr(0, pos);
    /* This will throw if it can't cast, which is fine. */
    _ms = std::stoi(magnitude) * multiplier;
}

/* Use optional and default to 1s per op in case something doesn't define this. */
throttle::throttle(configuration *config) : throttle(config->get_optional_string(OP_RATE, "1s")) {}

/* Default to a second per operation. */
throttle::throttle() : throttle("1s") {}

void
throttle::sleep()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(_ms));
}
} // namespace test_harness
