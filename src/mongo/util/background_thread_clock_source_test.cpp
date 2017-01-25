/**
 *    Copyright (C) 2016 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/background_thread_clock_source.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/time_support.h"

namespace {

using namespace mongo;

class BTCSTest : public mongo::unittest::Test {
public:
    void setUpClocks(Milliseconds granularity) {
        auto csMock = stdx::make_unique<ClockSourceMock>();
        _csMock = csMock.get();
        _csMock->advance(granularity);  // Make sure the mock doesn't return time 0.
        _btcs = stdx::make_unique<BackgroundThreadClockSource>(std::move(csMock), granularity);
    }

    void tearDown() override {
        _btcs.reset();
    }

protected:
    std::unique_ptr<BackgroundThreadClockSource> _btcs;
    ClockSourceMock* _csMock;
};

TEST_F(BTCSTest, CreateAndTerminate) {
    setUpClocks(Milliseconds(1));
    _btcs.reset();  // destroys the clock source

    setUpClocks(Hours(48));
    _btcs.reset();
}

TEST_F(BTCSTest, TimeKeeping) {
    setUpClocks(Milliseconds(1));
    ASSERT_EQUALS(_btcs->now(), _csMock->now());

    _csMock->advance(Milliseconds(100));
    sleepFor(Seconds(1));  // give the _btcs opportunity to read the new time
    ASSERT_EQUALS(_btcs->now(), _csMock->now());
}

TEST_F(BTCSTest, GetPrecision) {
    setUpClocks(Milliseconds(1));
    ASSERT_EQUALS(_btcs->getPrecision(), Milliseconds(1));
}

TEST_F(BTCSTest, StartsPaused) {
    setUpClocks(Milliseconds(1));
    ASSERT_EQUALS(_btcs->peekNowForTest(), 0);
}

TEST_F(BTCSTest, PausesAfterRead) {
    setUpClocks(Milliseconds(100));

    // Wake it up.
    auto now = _btcs->now().toMillisSinceEpoch();
    ASSERT_NE(now, 0);
    ASSERT_EQ(_btcs->peekNowForTest(), now);
    _csMock->advance(Milliseconds(100));
    ASSERT_EQ(_btcs->now().toMillisSinceEpoch(), now);

    sleepFor(Seconds(1));  // give the _btcs opportunity to detect idleness.
    ASSERT_EQ(_btcs->peekNowForTest(), 0);
}

TEST_F(BTCSTest, DoesntPauseWhenInUse) {
    setUpClocks(Milliseconds(100));

    auto lastTime = _btcs->now().toMillisSinceEpoch();
    ASSERT_NE(lastTime, 0);
    ASSERT_EQ(lastTime, _btcs->now().toMillisSinceEpoch());  // Mark the timer as still in use.
    auto ticks = 0;                                          // Count of when times change.
    while (ticks < 10) {
        if (_btcs->peekNowForTest() == lastTime) {
            _csMock->advance(Milliseconds(1));
            ASSERT_LT(_csMock->now().toMillisSinceEpoch() - lastTime, 1000);
            sleepFor(Milliseconds(1));
            continue;
        }
        ticks++;

        ASSERT_NE(_btcs->peekNowForTest(), 0);
        lastTime = _btcs->now().toMillisSinceEpoch();
        ASSERT_NE(lastTime, 0);
        ASSERT_EQ(lastTime, _btcs->peekNowForTest());
    }
}

TEST_F(BTCSTest, WakesAfterPause) {
    setUpClocks(Milliseconds(10));

    // Wake it up.
    auto now = _btcs->now().toMillisSinceEpoch();
    ASSERT_NE(now, 0);
    ASSERT_EQ(_btcs->peekNowForTest(), now);
    _csMock->advance(Milliseconds(10));
    ASSERT_EQ(_btcs->now().toMillisSinceEpoch(), now);

    sleepFor(Seconds(1));  // give the _btcs opportunity to detect idleness.
    ASSERT_EQ(_btcs->peekNowForTest(), 0);

    // Wake it up again and ensure it ticks at least once.
    auto lastTime = _btcs->now().toMillisSinceEpoch();
    ASSERT_NE(lastTime, 0);
    ASSERT_EQ(lastTime, _btcs->now().toMillisSinceEpoch());  // Mark the timer as still in use.
    while (_btcs->peekNowForTest() == lastTime) {
        _csMock->advance(Milliseconds(1));
        ASSERT_LT(_csMock->now().toMillisSinceEpoch() - lastTime, 1000);
        sleepFor(Milliseconds(1));
    }
    ASSERT_NE(_btcs->peekNowForTest(), 0);
}

}  // namespace
