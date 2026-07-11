// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/lock_manager/fill_locker_info.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/database_name.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/lock_manager/lock_stats.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <string_view>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using LockerInfo = Locker::LockerInfo;
using OneLock = Locker::OneLock;

TEST(FillLockerInfo, DoesReportWaitingForLockIfWaiting) {
    LockerInfo info;
    info.waitingResource = resourceIdGlobal;
    ASSERT_TRUE(info.waitingResource.isValid());

    BSONObjBuilder infoBuilder;
    fillLockerInfo(info, infoBuilder);
    const BSONObj infoObj = infoBuilder.done();

    ASSERT(infoObj["waitingForLock"].type() == BSONType::boolean);
    ASSERT_TRUE(infoObj["waitingForLock"].Bool());
}

TEST(FillLockerInfo, DoesNotReportWaitingForLockIfNotWaiting) {
    LockerInfo info;
    info.waitingResource = ResourceId();  // This means it is not waiting for anything.
    ASSERT_FALSE(info.waitingResource.isValid());

    BSONObjBuilder infoBuilder;
    fillLockerInfo(info, infoBuilder);
    const BSONObj infoObj = infoBuilder.done();

    ASSERT(infoObj["waitingForLock"].type() == BSONType::boolean);
    ASSERT_FALSE(infoObj["waitingForLock"].Bool());
}

TEST(FillLockerInfo, DoesReportLockStats) {
    LockerInfo info;
    SingleThreadedLockStats stats;
    stats.recordAcquisition(resourceIdGlobal, MODE_IX);
    info.stats = stats;

    BSONObjBuilder infoBuilder;
    fillLockerInfo(info, infoBuilder);
    const BSONObj infoObj = infoBuilder.done();

    ASSERT_EQ(infoObj["lockStats"].type(), BSONType::object);
}

TEST(FillLockerInfo, DoesReportLocksHeld) {
    const ResourceId dbId(RESOURCE_DATABASE,
                          DatabaseName::createDatabaseName_forTest(boost::none, "TestDB"sv));
    LockerInfo info;
    info.locks = {OneLock{resourceIdGlobal, MODE_IX}, OneLock{dbId, MODE_IX}};

    BSONObjBuilder infoBuilder;
    fillLockerInfo(info, infoBuilder);
    const BSONObj infoObj = infoBuilder.done();

    ASSERT_EQ(infoObj["locks"].type(), BSONType::object);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(resourceIdGlobal.getType())].type(),
              BSONType::string);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(resourceIdGlobal.getType())].String(), "w");
    ASSERT_EQ(infoObj["locks"][resourceTypeName(dbId.getType())].type(), BSONType::string);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(dbId.getType())].String(), "w");
}

TEST(FillLockerInfo, ShouldReportMaxTypeHeldForResourceType) {
    const ResourceId firstDbId(RESOURCE_DATABASE,
                               DatabaseName::createDatabaseName_forTest(boost::none, "FirstDB"sv));
    const ResourceId secondDbId(
        RESOURCE_DATABASE, DatabaseName::createDatabaseName_forTest(boost::none, "SecondDB"sv));
    LockerInfo info;
    info.locks = {OneLock{resourceIdGlobal, MODE_IX},
                  OneLock{firstDbId, MODE_IX},
                  OneLock{secondDbId, MODE_X}};

    BSONObjBuilder infoBuilder;
    fillLockerInfo(info, infoBuilder);
    BSONObj infoObj = infoBuilder.done();

    ASSERT_EQ(infoObj["locks"].type(), BSONType::object);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(firstDbId.getType())].type(), BSONType::string);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(firstDbId.getType())].String(),
              "W");  // One is held in IX, one in X, so X should win and be displayed as "W".

    // Ensure it still works if locks are reported in the opposite order.
    info.locks = {OneLock{resourceIdGlobal, MODE_IX},
                  OneLock{secondDbId, MODE_X},
                  OneLock{firstDbId, MODE_IX}};

    ASSERT_EQ(infoObj["locks"].type(), BSONType::object);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(firstDbId.getType())].type(), BSONType::string);
    ASSERT_EQ(infoObj["locks"][resourceTypeName(firstDbId.getType())].String(), "W");
}

}  // namespace
}  // namespace mongo
