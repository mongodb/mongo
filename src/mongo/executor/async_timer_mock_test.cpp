/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/executor/async_timer_mock.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace executor {

TEST(AsyncTimerMock, BasicTest) {
    AsyncTimerFactoryMock factory;

    // Set an early timer
    bool timer1Fired = false;
    auto timer1 = factory.make(Milliseconds(1000));
    timer1->asyncWait([&timer1Fired](std::error_code ec) {
        ASSERT(!ec);
        timer1Fired = true;
    });

    // Set a later timer
    bool timer2Fired = false;
    auto timer2 = factory.make(Milliseconds(2000));
    timer2->asyncWait([&timer2Fired](std::error_code ec) {
        ASSERT(!ec);
        timer2Fired = true;
    });

    // Advance clock a little, nothing should fire
    factory.fastForward(Milliseconds(500));
    ASSERT(!timer1Fired);
    ASSERT(!timer2Fired);

    // Advance clock so early timer fires
    factory.fastForward(Milliseconds(600));
    ASSERT(timer1Fired);

    // Second timer should still be waiting
    ASSERT(!timer2Fired);

    // Advance clock so second timer fires
    factory.fastForward(Milliseconds(1000));
    ASSERT(timer2Fired);
}

TEST(AsyncTimerMock, Cancel) {
    AsyncTimerFactoryMock factory;

    // Set a timer
    bool fired = false;
    auto timer = factory.make(Milliseconds(100));
    timer->asyncWait([&fired](std::error_code ec) {
        // This timer should have been canceled
        ASSERT(ec);
        ASSERT(ec == asio::error::operation_aborted);
        fired = true;
    });

    // Cancel timer
    timer->cancel();

    // Ensure that its handler was called
    ASSERT(fired);
}

TEST(AsyncTimerMock, CancelExpired) {
    AsyncTimerFactoryMock factory;

    // Set a timer
    bool fired = false;
    auto timer = factory.make(Milliseconds(100));
    timer->asyncWait([&fired](std::error_code ec) {
        // This timer should NOT have been canceled
        ASSERT(!ec);
        fired = true;
    });

    // Fast forward so it expires
    factory.fastForward(Milliseconds(200));
    ASSERT(fired);

    fired = false;

    // Cancel it, should not fire again
    timer->cancel();
    ASSERT(!fired);
}

}  // namespace executor
}  // namespace mongo
