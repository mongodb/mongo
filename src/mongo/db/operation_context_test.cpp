/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {

class OperationDeadlineTests : public unittest::Test {
public:
    void setUp() {
        auto uniqueMockClock = stdx::make_unique<ClockSourceMock>();
        mockClock = uniqueMockClock.get();
        service = stdx::make_unique<ServiceContextNoop>();
        service->setFastClockSource(std::move(uniqueMockClock));
        service->setTickSource(stdx::make_unique<TickSourceMock>());
        client = service->makeClient("OperationDeadlineTest");
    }

    ClockSourceMock* mockClock;
    std::unique_ptr<ServiceContext> service;
    ServiceContext::UniqueClient client;
};

TEST_F(OperationDeadlineTests, OperationDeadlineExpiration) {
    auto txn = client->makeOperationContext();
    txn->setDeadlineAfterNowBy(Seconds{1});
    mockClock->advance(Milliseconds{500});
    ASSERT_OK(txn->checkForInterruptNoAssert());

    // 1ms before relative deadline reports no interrupt
    mockClock->advance(Milliseconds{499});
    ASSERT_OK(txn->checkForInterruptNoAssert());

    // Exactly at deadline reports no interrupt, because setDeadlineAfterNowBy adds one clock
    // precision unit to the deadline, to ensure that the deadline does not expire in less than the
    // requested amount of time.
    mockClock->advance(Milliseconds{1});
    ASSERT_OK(txn->checkForInterruptNoAssert());

    // Since the mock clock's precision is 1ms, at test start + 1001 ms, we expect
    // checkForInterruptNoAssert to return ExceededTimeLimit.
    mockClock->advance(Milliseconds{1});
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, txn->checkForInterruptNoAssert());

    // Also at times greater than start + 1001ms, we expect checkForInterruptNoAssert to keep
    // returning ExceededTimeLimit.
    mockClock->advance(Milliseconds{1});
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, txn->checkForInterruptNoAssert());
}

template <typename D>
void assertLargeRelativeDeadlineLikeInfinity(Client& client, D maxTime) {
    auto txn = client.makeOperationContext();
    txn->setDeadlineAfterNowBy(maxTime);
    ASSERT_FALSE(txn->hasDeadline()) << "Tried to set maxTime to " << maxTime;
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesHours) {
    ASSERT_FALSE(client->makeOperationContext()->hasDeadline());
    assertLargeRelativeDeadlineLikeInfinity(*client, Hours::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesMinutes) {
    assertLargeRelativeDeadlineLikeInfinity(*client, Minutes::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesSeconds) {
    assertLargeRelativeDeadlineLikeInfinity(*client, Seconds::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesMilliseconds) {
    assertLargeRelativeDeadlineLikeInfinity(*client, Milliseconds::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesMicroseconds) {
    assertLargeRelativeDeadlineLikeInfinity(*client, Microseconds::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesNanoseconds) {
    // Nanoseconds::max() is less than Microseconds::max(), so it is possible to set
    // a deadline of that duration.
    auto txn = client->makeOperationContext();
    txn->setDeadlineAfterNowBy(Nanoseconds::max());
    ASSERT_TRUE(txn->hasDeadline());
    ASSERT_EQ(mockClock->now() + mockClock->getPrecision() +
                  duration_cast<Milliseconds>(Nanoseconds::max()),
              txn->getDeadline());
}

}  // namespace

}  // namespace mongo
