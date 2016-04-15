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

TEST(BackgroundThreadClockSource, CreateAndTerminate) {
    auto clockSource = stdx::make_unique<ClockSourceMock>();
    auto btClockSource =
        stdx::make_unique<BackgroundThreadClockSource>(std::move(clockSource), Milliseconds(1));
    btClockSource.reset();  // destroys the clock source

    clockSource = stdx::make_unique<ClockSourceMock>();
    btClockSource =
        stdx::make_unique<BackgroundThreadClockSource>(std::move(clockSource), Hours(48));
    btClockSource.reset();  // destroys the clock source
}

TEST(BackgroundThreadClockSource, TimeKeeping) {
    auto clockSource = stdx::make_unique<ClockSourceMock>();
    ClockSourceMock* clockSourceMock = clockSource.get();

    auto btClockSource =
        stdx::make_unique<BackgroundThreadClockSource>(std::move(clockSource), Milliseconds(1));
    ASSERT_EQUALS(btClockSource->now(), clockSourceMock->now());

    clockSourceMock->advance(Milliseconds(100));
    sleepFor(Milliseconds(10));  // give the btClockSource opportunity to read the new time
    ASSERT_EQUALS(btClockSource->now(), clockSourceMock->now());
}

}  // namespace
