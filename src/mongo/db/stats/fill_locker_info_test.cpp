/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using LockerInfo = Locker::LockerInfo;
using OneLock = Locker::OneLock;

const ResourceId kGlobalId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);

TEST(FillLockerInfo, DoesReportWaitingForLockIfWaiting) {
    LockerInfo info;
    info.waitingResource = kGlobalId;
    ASSERT_TRUE(info.waitingResource.isValid());

    BSONObjBuilder infoBuilder;
    fillLockerInfo(info, infoBuilder);
    const BSONObj infoObj = infoBuilder.done();

    ASSERT(infoObj["waitingForLock"].type() == BSONType::Bool);
    ASSERT_TRUE(infoObj["waitingForLock"].Bool());
}

TEST(FillLockerInfo, DoesNotReportWaitingForLockIfNotWaiting) {
    LockerInfo info;
    info.waitingResource = ResourceId();  // This means it is not waiting for anything.
    ASSERT_FALSE(info.waitingResource.isValid());

    BSONObjBuilder infoBuilder;
    fillLockerInfo(info, infoBuilder);
    const BSONObj infoObj = infoBuilder.done();

    ASSERT(infoObj["waitingForLock"].type() == BSONType::Bool);
    ASSERT_FALSE(infoObj["waitingForLock"].Bool());
}

TEST(FillLockerInfo, DoesReportLockStats) {
    LockerInfo info;
    SingleThreadedLockStats stats;
    stats.recordAcquisition(kGlobalId, MODE_IX);
    info.stats = stats;

    BSONObjBuilder infoBuilder;
    fillLockerInfo(info, infoBuilder);
    const BSONObj infoObj = infoBuilder.done();

    ASSERT_EQ(infoObj["lockStats"].type(), BSONType::Object);
}

DEATH_TEST(FillLockerInfo, ShouldFailIfLocksAreNotSortedAppropriately, "Invariant failure") {
    LockerInfo info;
    // The global lock is supposed to come before the database lock.
    info.locks = {OneLock{ResourceId(RESOURCE_DATABASE, "TestDB"_sd), MODE_X},
                  OneLock{kGlobalId, MODE_IX}};

    BSONObjBuilder infoBuilder;
    fillLockerInfo(info, infoBuilder);
}

TEST(FillLockerInfo, DoesReportLocksHeld) {
    const ResourceId dbId(RESOURCE_DATABASE, "TestDB"_sd);
    LockerInfo info;
    info.locks = {OneLock{kGlobalId, MODE_IX}, OneLock{dbId, MODE_IX}};

    BSONObjBuilder infoBuilder;
    fillLockerInfo(info, infoBuilder);
    const BSONObj infoObj = infoBuilder.done();

    ASSERT_EQ(infoObj["locks"].type(), BSONType::Object);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(kGlobalId.getType())].type(), BSONType::String);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(kGlobalId.getType())].String(), "w");
    ASSERT_EQ(infoObj["locks"][resourceTypeName(dbId.getType())].type(), BSONType::String);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(dbId.getType())].String(), "w");
}

TEST(FillLockerInfo, ShouldReportMaxTypeHeldForResourceType) {
    const ResourceId firstDbId(RESOURCE_DATABASE, "FirstDB"_sd);
    const ResourceId secondDbId(RESOURCE_DATABASE, "SecondDB"_sd);
    LockerInfo info;
    info.locks = {
        OneLock{kGlobalId, MODE_IX}, OneLock{firstDbId, MODE_IX}, OneLock{secondDbId, MODE_X}};

    BSONObjBuilder infoBuilder;
    fillLockerInfo(info, infoBuilder);
    BSONObj infoObj = infoBuilder.done();

    ASSERT_EQ(infoObj["locks"].type(), BSONType::Object);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(firstDbId.getType())].type(), BSONType::String);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(firstDbId.getType())].String(),
              "W");  // One is held in IX, one in X, so X should win and be displayed as "W".

    // Ensure it still works if locks are reported in the opposite order.
    info.locks = {
        OneLock{kGlobalId, MODE_IX}, OneLock{secondDbId, MODE_X}, OneLock{firstDbId, MODE_IX}};

    ASSERT_EQ(infoObj["locks"].type(), BSONType::Object);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(firstDbId.getType())].type(), BSONType::String);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(firstDbId.getType())].String(), "W");
}

}  // namespace
}  // namespace mongo
