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

#include "mongo/db/local_catalog/lock_manager/lock_stats.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/lock_manager/locker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

class LockStatsTest : public ServiceContextTest {};

TEST_F(LockStatsTest, NoWait) {
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "LockStats.NoWait"));

    resetGlobalLockStats();

    auto opCtx = makeOperationContext();
    Locker locker(getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);
    ON_BLOCK_EXIT([&] { locker.unlockGlobal(); });
    locker.lock(opCtx.get(), resId, MODE_X);
    locker.unlock(resId);

    // Make sure that the waits/blocks are zero
    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);

    ASSERT_EQUALS(1, stats.get(resId, MODE_X).numAcquisitions);
    ASSERT_EQUALS(0, stats.get(resId, MODE_X).numWaits);
    ASSERT_EQUALS(0, stats.get(resId, MODE_X).combinedWaitTimeMicros);
}

TEST_F(LockStatsTest, Wait) {
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "LockStats.Wait"));

    resetGlobalLockStats();

    auto opCtx = makeOperationContext();
    Locker locker(getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);
    ON_BLOCK_EXIT([&] { locker.unlockGlobal(); });
    locker.lock(opCtx.get(), resId, MODE_X);

    {
        // This will block
        Locker lockerConflict(getServiceContext());
        lockerConflict.lockGlobal(opCtx.get(), MODE_IX);
        ON_BLOCK_EXIT([&] { lockerConflict.unlockGlobal(); });
        ASSERT_EQUALS(LOCK_WAITING, lockerConflict.lockBeginForTest(opCtx.get(), resId, MODE_S));

        // Sleep 1 millisecond so the wait time passes
        ASSERT_THROWS_CODE(lockerConflict.lockCompleteForTest(
                               opCtx.get(), resId, MODE_S, Date_t::now() + Milliseconds(5)),
                           AssertionException,
                           ErrorCodes::LockTimeout);
    }

    // Make sure that the waits/blocks are non-zero
    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);

    ASSERT_EQUALS(1, stats.get(resId, MODE_X).numAcquisitions);
    ASSERT_EQUALS(0, stats.get(resId, MODE_X).numWaits);
    ASSERT_EQUALS(0, stats.get(resId, MODE_X).combinedWaitTimeMicros);

    ASSERT_EQUALS(1, stats.get(resId, MODE_S).numAcquisitions);
    ASSERT_EQUALS(1, stats.get(resId, MODE_S).numWaits);
    ASSERT_GREATER_THAN(stats.get(resId, MODE_S).combinedWaitTimeMicros, 0);
}

TEST_F(LockStatsTest, Reporting) {
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "LockStats.Reporting"));

    resetGlobalLockStats();

    auto opCtx = makeOperationContext();
    Locker locker(getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);
    ON_BLOCK_EXIT([&] { locker.unlockGlobal(); });
    locker.lock(opCtx.get(), resId, MODE_X);
    locker.unlock(resId);

    // Make sure that the waits/blocks are zero
    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);

    BSONObjBuilder builder;
    stats.report(&builder);
}

TEST_F(LockStatsTest, Subtraction) {
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "LockStats.Subtraction"));

    resetGlobalLockStats();

    auto opCtx = makeOperationContext();
    Locker locker(getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);
    ON_BLOCK_EXIT([&] { locker.unlockGlobal(); });
    locker.lock(opCtx.get(), resId, MODE_X);

    {
        Locker lockerConflict(getServiceContext());
        lockerConflict.lockGlobal(opCtx.get(), MODE_IX);
        ON_BLOCK_EXIT([&] { lockerConflict.unlockGlobal(); });
        ASSERT_THROWS_CODE(
            lockerConflict.lock(opCtx.get(), resId, MODE_S, Date_t::now() + Milliseconds(5)),
            AssertionException,
            ErrorCodes::LockTimeout);
    }

    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);
    ASSERT_EQUALS(1, stats.get(resId, MODE_S).numAcquisitions);
    ASSERT_EQUALS(1, stats.get(resId, MODE_S).numWaits);
    ASSERT_GREATER_THAN(stats.get(resId, MODE_S).combinedWaitTimeMicros, 0);

    {
        Locker lockerConflict(getServiceContext());
        lockerConflict.lockGlobal(opCtx.get(), MODE_IX);
        ON_BLOCK_EXIT([&] { lockerConflict.unlockGlobal(); });
        ASSERT_THROWS_CODE(
            lockerConflict.lock(opCtx.get(), resId, MODE_S, Date_t::now() + Milliseconds(5)),
            AssertionException,
            ErrorCodes::LockTimeout);
    }

    SingleThreadedLockStats stats2;
    reportGlobalLockingStats(&stats2);
    ASSERT_EQUALS(2, stats2.get(resId, MODE_S).numAcquisitions);
    ASSERT_EQUALS(2, stats2.get(resId, MODE_S).numWaits);
    ASSERT_GREATER_THAN(stats2.get(resId, MODE_S).combinedWaitTimeMicros, 0);

    stats2.subtract(stats);
    ASSERT_EQUALS(1, stats2.get(resId, MODE_S).numAcquisitions);
    ASSERT_EQUALS(1, stats2.get(resId, MODE_S).numWaits);
    ASSERT_GREATER_THAN(stats2.get(resId, MODE_S).combinedWaitTimeMicros, 0);
}

namespace {
/**
 * Locks 'rid' and then checks the global lock stat is reported correctly. Either the global lock is
 * reported locked if 'rid' is the global lock resource, or unlocked if 'rid' is not the global lock
 * resource.
 */
void assertGlobalAcquisitionStats(OperationContext* opCtx, ResourceId rid) {
    resetGlobalLockStats();

    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);
    ASSERT_EQUALS(0, stats.get(rid, LockMode::MODE_IX).numAcquisitions);

    Locker locker(opCtx->getServiceContext());
    if (rid == resourceIdGlobal) {
        locker.lockGlobal(opCtx, LockMode::MODE_IX);
    } else {
        locker.lock(opCtx, rid, LockMode::MODE_IX);
    }

    reportGlobalLockingStats(&stats);
    if (rid == resourceIdGlobal) {
        ASSERT_EQUALS(1, stats.get(resourceIdGlobal, LockMode::MODE_IX).numAcquisitions);
    } else {
        ASSERT_EQUALS(0, stats.get(resourceIdGlobal, LockMode::MODE_IX).numAcquisitions);
    }

    if (rid == resourceIdGlobal) {
        ASSERT_TRUE(locker.unlockGlobal());
    } else {
        ASSERT_TRUE(locker.unlock(rid));
    }
}
}  // namespace

TEST_F(LockStatsTest, GlobalRetrievableSeparately) {
    auto opCtx = makeOperationContext();
    assertGlobalAcquisitionStats(opCtx.get(), resourceIdGlobal);
    assertGlobalAcquisitionStats(opCtx.get(), resourceIdReplicationStateTransitionLock);
}

TEST_F(LockStatsTest, ServerStatus) {
    resetGlobalLockStats();

    // If there are no locks, nothing is reported.
    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);
    BSONObjBuilder builder;
    stats.report(&builder);
    ASSERT_EQUALS(0, builder.done().nFields());

    // Take the global and RSTL locks in MODE_IX to create acquisition stats for them.
    auto opCtx = makeOperationContext();
    Locker locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), LockMode::MODE_IX);
    locker.lock(opCtx.get(), resourceIdReplicationStateTransitionLock, LockMode::MODE_IX);

    locker.unlock(resourceIdReplicationStateTransitionLock);
    locker.unlockGlobal();

    // Now the MODE_IX lock acquisitions should be reported, separately for each lock type.
    reportGlobalLockingStats(&stats);
    BSONObjBuilder builder2;
    stats.report(&builder2);
    auto lockingStats = builder2.done();
    ASSERT_EQUALS(
        1, lockingStats.getObjectField("Global").getObjectField("acquireCount").getIntField("w"));
    ASSERT_EQUALS(1,
                  lockingStats.getObjectField("ReplicationStateTransition")
                      .getObjectField("acquireCount")
                      .getIntField("w"));
}

TEST_F(LockStatsTest, CumulativeWaitTime) {
    const ResourceId resId1(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "LockStats.Wait1"));
    const ResourceId resId2(
        RESOURCE_DATABASE,
        NamespaceString::createNamespaceString_forTest(boost::none, "LockStats.Wait2"));

    auto opCtx = makeOperationContext();
    Locker locker(getServiceContext());

    resetGlobalLockStats();
    locker.lockGlobal(opCtx.get(), MODE_IX);
    ON_BLOCK_EXIT([&] { locker.unlockGlobal(); });
    locker.lock(opCtx.get(), resId1, MODE_X);
    locker.lock(opCtx.get(), resId2, MODE_X);

    {
        Locker lockerConflict(getServiceContext());
        lockerConflict.lockGlobal(opCtx.get(), MODE_IX);
        ON_BLOCK_EXIT([&] { lockerConflict.unlockGlobal(); });

        ASSERT_THROWS_CODE(
            lockerConflict.lock(opCtx.get(), resId1, MODE_S, Date_t::now() + Seconds(2)),
            AssertionException,
            ErrorCodes::LockTimeout);

        ASSERT_THROWS_CODE(
            lockerConflict.lock(opCtx.get(), resId2, MODE_S, Date_t::now() + Seconds(2)),
            AssertionException,
            ErrorCodes::LockTimeout);
    }

    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);

    auto wait1 = stats.get(resId1, MODE_S).combinedWaitTimeMicros;
    auto wait2 = stats.get(resId2, MODE_S).combinedWaitTimeMicros;
    // Lower bound with a bit of leeway, should be close to 2 seconds.
    ASSERT_GREATER_THAN_OR_EQUALS(wait1, 1.9 * 1000 * 1000);
    ASSERT_GREATER_THAN_OR_EQUALS(wait2, 1.9 * 1000 * 1000);
    // We give a generous leeway of 500msecs (2.5 seconds, or 2,500,000 microseconds) for the upper
    // bound of the calculation to ensure it's growing correctly.
    ASSERT_LESS_THAN(wait1, 2.5 * 1000 * 1000);
    ASSERT_LESS_THAN(wait2, 2.5 * 1000 * 1000);
    ASSERT_EQ(stats.getCumulativeWaitTimeMicros(), wait1 + wait2);
}

}  // namespace
}  // namespace mongo
