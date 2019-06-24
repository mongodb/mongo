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

#include "mongo/platform/basic.h"

#include "mongo/util/background_thread_clock_source.h"

#include <memory>

#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/time_support.h"

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
