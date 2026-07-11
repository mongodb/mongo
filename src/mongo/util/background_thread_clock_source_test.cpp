// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/background_thread_clock_source.h"

#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <utility>

namespace {

using namespace mongo;

class BTCSTest : public mongo::unittest::Test {
public:
    void setUpClocks(Milliseconds granularity) {
        auto csMock = std::make_unique<ClockSourceMock>();
        _csMock = csMock.get();
        _btcs = std::make_unique<BackgroundThreadClockSource>(std::move(csMock), granularity);
    }

    void setUpRealClocks(Milliseconds granularity) {
        _btcs = std::make_unique<BackgroundThreadClockSource>(std::make_unique<SystemClockSource>(),
                                                              granularity);
    }

    void tearDown() override {
        _btcs.reset();
    }

protected:
    size_t waitForIdleDetection() {
        auto lastTimesPaused = _lastTimesPaused;
        while (lastTimesPaused == checkTimesPaused()) {
            _csMock->advance(Milliseconds(1));
            sleepFor(Milliseconds(1));
        }

        return _lastTimesPaused;
    }

    size_t checkTimesPaused() {
        _lastTimesPaused = _btcs->timesPausedForTest();

        return _lastTimesPaused;
    }

    ClockSourceMock* _csMock;
    std::unique_ptr<BackgroundThreadClockSource> _btcs;
    size_t _lastTimesPaused = 0;
};

TEST_F(BTCSTest, CreateAndTerminate) {
    setUpClocks(Milliseconds(1));
    _btcs.reset();  // destroys the clock source

    setUpClocks(Hours(48));
    _btcs.reset();
}

TEST_F(BTCSTest, PausesAfterRead) {
    setUpClocks(Milliseconds(1));
    auto preCount = waitForIdleDetection();
    _btcs->now();
    auto after = waitForIdleDetection();
    ASSERT_LT(preCount, after);
}

TEST_F(BTCSTest, NowWorks) {
    setUpRealClocks(Milliseconds(1));

    const auto then = _btcs->now();
    sleepFor(Milliseconds(100));
    const auto now = _btcs->now();
    ASSERT_GTE(now, then);
    ASSERT_LTE(now, _btcs->now());
}

TEST_F(BTCSTest, GetPrecision) {
    setUpClocks(Milliseconds(1));
    ASSERT_EQUALS(_btcs->getPrecision(), Milliseconds(1));
}

TEST_F(BTCSTest, StartsPaused) {
    setUpClocks(Milliseconds(1));
    ASSERT_EQUALS(checkTimesPaused(), 1ull);
}

TEST_F(BTCSTest, DoesntPauseWhenInUse) {
    const auto kGranularity = Milliseconds(3);
    setUpClocks(kGranularity);

    _btcs->now();
    auto count = waitForIdleDetection();

    for (int i = 0; i < 100; ++i) {
        ASSERT_EQ(count, checkTimesPaused());
        _btcs->now();
        _csMock->advance(Milliseconds(1));
        sleepFor(Milliseconds(1));
    }
}

}  // namespace
