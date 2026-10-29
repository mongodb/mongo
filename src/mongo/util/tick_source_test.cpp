// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/tick_source.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/tick_source_mock.h"

#include <algorithm>
#include <chrono>  // NOLINT
#include <cstddef>
#include <memory>
#include <ratio>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/chrono.h>  // IWYU pragma: keep
#include <fmt/format.h>

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
    using namespace std::chrono_literals;
#ifdef _WIN32
    // Error upper bound increased for Windows systems because sleep_for is known to possibly
    // sleep longer than the duration specified with high margin of error.
    const static double kMaxError = 5.0;
#else
    // Even if sleep is strict, there's no guarantee the scheduler will
    // immediately schedule your process when it's done sleeping. We
    // need to allow for that in the upper bound.
    const static double kMaxError = 0.3;
#endif
    // Because sleep_for guarantees blocking for at least sleep duration, the error lower
    // bound only needs to account for the potential time difference between the intervals
    // of getTicks and sleep_for.
    const static double kMinError = -0.01;
    auto ts = makeSystemTickSource();
    auto tTick = 1.0s / ts->getTicksPerSecond();
    for (int i = 0; i != 5; ++i) {
        auto delay = 1000ms + 50ms * i;
        auto n0 = ts->getTicks();
        std::this_thread::sleep_for(delay);
        auto n1 = ts->getTicks();
        auto dt = (n1 - n0) * tTick;
        double err = (dt - delay) / delay;
        ASSERT_THAT(err, m::AllOf(m::Ge(kMinError), m::Le(kMaxError)))
            << fmt::format(" n0={}, n1={}, tTick={}, delay={}, dt={}", n0, n1, tTick, delay, dt);
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
