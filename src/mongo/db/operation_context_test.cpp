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
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
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
    }

    ClockSourceMock* mockClock;
    std::unique_ptr<ServiceContext> service;
};

TEST_F(OperationDeadlineTests, OperationDeadlineExpiration) {
    auto client = service->makeClient("CurOpTest");
    auto txn = client->makeOperationContext();
    txn->setMaxTimeMicros(durationCount<Microseconds>(Seconds{1}));
    mockClock->advance(Milliseconds{500});
    ASSERT_OK(txn->checkForInterruptNoAssert());
    mockClock->advance(Milliseconds{499});
    ASSERT_OK(txn->checkForInterruptNoAssert());
    mockClock->advance(Milliseconds{1});
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, txn->checkForInterruptNoAssert());
    mockClock->advance(Milliseconds{1});
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, txn->checkForInterruptNoAssert());
}

}  // namespace

}  // namespace mongo
