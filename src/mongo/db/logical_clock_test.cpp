/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#include "mongo/db/logical_clock.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/platform/basic.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * Setup LogicalClock with invalid initial time.
 */
class LogicalClockTestBase : public unittest::Test {
protected:
    void setUp() {
        _serviceContext = stdx::make_unique<ServiceContextNoop>();
        auto pTps = stdx::make_unique<TimeProofService>();
        _timeProofService = pTps.get();
        _clock = stdx::make_unique<LogicalClock>(_serviceContext.get(), std::move(pTps), true);
    }

    void tearDown() {
        _clock.reset();
        _serviceContext.reset();
    }

    LogicalClock* getClock() {
        return _clock.get();
    }

    SignedLogicalTime makeSignedLogicalTime(LogicalTime logicalTime) {
        return SignedLogicalTime(logicalTime, _timeProofService->getProof(logicalTime));
    }

private:
    TimeProofService* _timeProofService;
    std::unique_ptr<ServiceContextNoop> _serviceContext;
    std::unique_ptr<LogicalClock> _clock;
};

// Check that the initial time does not change during logicalClock creation.
TEST_F(LogicalClockTestBase, roundtrip) {
    // Create different logicalClock instance to validate that the initial time is preserved.
    ServiceContextNoop serviceContext;
    Timestamp tX(1);
    auto pTps = stdx::make_unique<TimeProofService>();
    auto time = LogicalTime(tX);

    LogicalClock logicalClock(&serviceContext, std::move(pTps), true);
    logicalClock.initClusterTimeFromTrustedSource(time);
    auto storedTime(logicalClock.getClusterTime());

    ASSERT_TRUE(storedTime.getTime() == time);
}

// Verify the reserve ticks functionality.
TEST_F(LogicalClockTestBase, reserveTicks) {
    auto t1 = getClock()->reserveTicks(1);
    auto t2(getClock()->getClusterTime());
    ASSERT_TRUE(t1 == t2.getTime());

    auto t3 = getClock()->reserveTicks(1);
    t1.addTicks(1);
    ASSERT_TRUE(t3 == t1);

    t3 = getClock()->reserveTicks(100);
    t1.addTicks(1);
    ASSERT_TRUE(t3 == t1);

    t3 = getClock()->reserveTicks(1);
    t1.addTicks(100);
    ASSERT_TRUE(t3 == t1);
}

// Verify the advanceClusterTime functionality.
TEST_F(LogicalClockTestBase, advanceClusterTime) {
    auto t1 = getClock()->reserveTicks(1);
    t1.addTicks(100);
    SignedLogicalTime l1 = makeSignedLogicalTime(t1);
    ASSERT_OK(getClock()->advanceClusterTime(l1));
    auto l2(getClock()->getClusterTime());
    ASSERT_TRUE(l1.getTime() == l2.getTime());
}

}  // unnamed namespace
}  // namespace mongo
