/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

namespace m = unittest::match;

template <typename SourceDuration, typename OutDuration>
auto tickToDuration(int ticks) {
    TickSourceMock<SourceDuration> ts;
    ts.reset(ticks);
    return ts.template ticksTo<OutDuration>(ts.getTicks()).count();
}

TEST(TickSourceTest, TicksToDurationConversion) {
    ASSERT_EQ((tickToDuration<Seconds, Seconds>(1)), 1);
    ASSERT_EQ((tickToDuration<Seconds, Milliseconds>(1)), 1'000);
    ASSERT_EQ((tickToDuration<Seconds, Microseconds>(1)), 1'000'000);
    ASSERT_EQ((tickToDuration<Milliseconds, Milliseconds>(1)), 1);
    ASSERT_EQ((tickToDuration<Milliseconds, Microseconds>(1)), 1'000);
    ASSERT_EQ((tickToDuration<Microseconds, Microseconds>(1)), 1);
}

TEST(SystemTickSourceTest, TicksPerSecond) {
    ASSERT_EQ(makeSystemTickSource()->getTicksPerSecond(), 1'000'000'000);
}

TEST(SystemTickSourceTest, GetTicks) {
    auto ts = makeSystemTickSource();
    auto t0 = ts->getTicks();
    for (int reps = 20; reps--;) {
        static constexpr Milliseconds delay{200};
        static constexpr Milliseconds err{20};
        sleepFor(delay);
        auto t1 = ts->getTicks();
        ASSERT_THAT(ts->ticksTo<Milliseconds>(t1 - t0),
                    m::AllOf(m::Ge(delay - err), m::Le(delay + err)));
        t0 = t1;
    }
}

TEST(SystemTickSourceTest, Monotonic) {
    auto ts = makeSystemTickSource();
    auto t0 = ts->getTicks();
    std::vector<TickSource::Tick> samples;
    samples.reserve(1'000'000);
    do {
        samples.clear();
        for (size_t i = 0; i < 1'000'000; ++i)
            samples.push_back(ts->getTicks());
        ASSERT_TRUE(std::is_sorted(samples.begin(), samples.end()));
    } while (ts->ticksTo<Milliseconds>(ts->getTicks() - t0) < Seconds{5});
}
}  // namespace
}  // namespace mongo
