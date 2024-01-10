/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include <boost/none.hpp>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_recipient_access_blocker.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace repl {

class TenantMigrationRecipientAccessBlockerTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto serviceContext = getServiceContext();
        TenantMigrationAccessBlockerRegistry::get(serviceContext).startup();

        _opCtx = makeOperationContext();

        StorageInterface::set(serviceContext, std::make_unique<StorageInterfaceImpl>());

        auto replCoord = std::make_unique<ReplicationCoordinatorMock>(serviceContext);
        ASSERT_OK(replCoord->updateTerm(opCtx(), 1));
        replCoord->setMyLastAppliedOpTimeAndWallTimeForward(
            OpTimeAndWallTime(OpTime(Timestamp(1, 1), 1), Date_t()));
        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
        ReplicationCoordinator::set(serviceContext, std::move(replCoord));
    }

    void tearDown() override {
        TenantMigrationAccessBlockerRegistry::get(getServiceContext()).shutDown();
        ServiceContextMongoDTest::tearDown();
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

    UUID getMigrationId() const {
        return _migrationId;
    }

    std::string getTenantId() const {
        return _tenantId;
    }

private:
    const UUID _migrationId = UUID::gen();
    const std::string _tenantId = "tenant123";
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(TenantMigrationRecipientAccessBlockerTest, NoopFunctions) {
    TenantMigrationRecipientAccessBlocker mtab(getServiceContext(), getMigrationId());

    // These functions are noop functions and should not throw even in reject state.
    ASSERT_OK(mtab.checkIfCanWrite(Timestamp()));
    ASSERT_OK(mtab.checkIfLinearizableReadWasAllowed(opCtx()));
    ASSERT_OK(mtab.checkIfCanBuildIndex());
}

TEST_F(TenantMigrationRecipientAccessBlockerTest, StateRejectReadsAndWrites) {
    TenantMigrationRecipientAccessBlocker mtab(getServiceContext(), getMigrationId());

    {
        BSONObjBuilder builder;
        mtab.appendInfoForServerStatus(&builder);
        ASSERT_BSONOBJ_EQ(builder.obj(),
                          BSON("migrationId" << getMigrationId() << "state"
                                             << "rejectReadsAndWrites"
                                             << "ttlIsBlocked" << true));
    }

    // Default read concern.
    ASSERT_THROWS_CODE(mtab.getCanRunCommandFuture(opCtx(), "find").get(),
                       DBException,
                       ErrorCodes::IllegalOperation);

    // Majority read concern.
    ReadConcernArgs::get(opCtx()) = ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern);
    ASSERT_THROWS_CODE(mtab.getCanRunCommandFuture(opCtx(), "find").get(),
                       DBException,
                       ErrorCodes::IllegalOperation);

    // Snapshot read concern.
    ReadConcernArgs::get(opCtx()) = ReadConcernArgs(ReadConcernLevel::kSnapshotReadConcern);
    shard_role_details::getRecoveryUnit(opCtx())->setTimestampReadSource(
        RecoveryUnit::ReadSource::kProvided, Timestamp(1, 1));
    ASSERT_THROWS_CODE(mtab.getCanRunCommandFuture(opCtx(), "find").get(),
                       DBException,
                       ErrorCodes::IllegalOperation);

    // Snapshot read concern with atClusterTime.
    ReadConcernArgs::get(opCtx()) = ReadConcernArgs(ReadConcernLevel::kSnapshotReadConcern);
    ReadConcernArgs::get(opCtx()).setArgsAtClusterTimeForSnapshot(Timestamp(1, 1));
    ASSERT_THROWS_CODE(mtab.getCanRunCommandFuture(opCtx(), "find").get(),
                       DBException,
                       ErrorCodes::IllegalOperation);
}

TEST_F(TenantMigrationRecipientAccessBlockerTest, StateRejectReadsBefore) {
    TenantMigrationRecipientAccessBlocker mtab(getServiceContext(), getMigrationId());

    mtab.startRejectingReadsBefore(Timestamp(1, 1));
    {
        BSONObjBuilder builder;
        mtab.appendInfoForServerStatus(&builder);
        ASSERT_BSONOBJ_EQ(builder.obj(),
                          BSON("migrationId" << getMigrationId() << "state"
                                             << "rejectReadsBefore"
                                             << "rejectBeforeTimestamp" << Timestamp(1, 1)
                                             << "ttlIsBlocked" << true));
    }

    // Advance 'rejectBeforeTimestamp'.
    mtab.startRejectingReadsBefore(Timestamp(2, 1));
    {
        BSONObjBuilder builder;
        mtab.appendInfoForServerStatus(&builder);
        ASSERT_BSONOBJ_EQ(builder.obj(),
                          BSON("migrationId" << getMigrationId() << "state"
                                             << "rejectReadsBefore"
                                             << "rejectBeforeTimestamp" << Timestamp(2, 1)
                                             << "ttlIsBlocked" << true));
    }

    // Default read concern.
    ASSERT_OK(mtab.getCanRunCommandFuture(opCtx(), "find").getNoThrow());

    // Majority read concern.
    ReadConcernArgs::get(opCtx()) = ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern);
    ASSERT_OK(mtab.getCanRunCommandFuture(opCtx(), "find").getNoThrow());

    // Snapshot read at a later timestamp.
    ReadConcernArgs::get(opCtx()) = ReadConcernArgs(ReadConcernLevel::kSnapshotReadConcern);
    ReadConcernArgs::get(opCtx()).setArgsAtClusterTimeForSnapshot(Timestamp(3, 1));
    ASSERT_OK(mtab.getCanRunCommandFuture(opCtx(), "find").getNoThrow());

    // Snapshot read at an earlier timestamp.
    ReadConcernArgs::get(opCtx()) = ReadConcernArgs(ReadConcernLevel::kSnapshotReadConcern);
    ReadConcernArgs::get(opCtx()).setArgsAtClusterTimeForSnapshot(Timestamp(1, 1));
    ASSERT_THROWS_CODE(mtab.getCanRunCommandFuture(opCtx(), "find").get(),
                       DBException,
                       ErrorCodes::SnapshotTooOld);
}

}  // namespace repl
}  // namespace mongo
