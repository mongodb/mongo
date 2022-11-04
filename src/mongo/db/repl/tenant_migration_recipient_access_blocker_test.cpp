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


#include <memory>

#include "mongo/client/connpool.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_recipient_access_blocker.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/net/ssl_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace repl {

namespace {

class RecoveryUnitMock : public RecoveryUnitNoop {
public:
    void setTimestampReadSource(RecoveryUnit::ReadSource source,
                                boost::optional<Timestamp> provided = boost::none) override {
        _source = source;
        _timestamp = provided;
    }
    RecoveryUnit::ReadSource getTimestampReadSource() const override {
        return _source;
    };
    boost::optional<Timestamp> getPointInTimeReadTimestamp(OperationContext* opCtx) override {
        return _timestamp;
    }

private:
    ReadSource _source = ReadSource::kNoTimestamp;
    boost::optional<Timestamp> _timestamp;
};

}  // namespace

class TenantMigrationRecipientAccessBlockerTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto serviceContext = getServiceContext();
        TenantMigrationAccessBlockerRegistry::get(cc().getServiceContext()).startup();

        {
            _opCtx = cc().makeOperationContext();
            _opCtx->setRecoveryUnit(std::make_unique<RecoveryUnitMock>(),
                                    WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

            StorageInterface::set(serviceContext, std::make_unique<StorageInterfaceImpl>());

            auto replCoord = std::make_unique<ReplicationCoordinatorMock>(serviceContext);
            ASSERT_OK(replCoord->updateTerm(opCtx(), 1));
            replCoord->setMyLastAppliedOpTimeAndWallTime(
                OpTimeAndWallTime(OpTime(Timestamp(1, 1), 1), Date_t()));
            ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
            ReplicationCoordinator::set(serviceContext, std::move(replCoord));
        }
    }

    void tearDown() override {
        TenantMigrationAccessBlockerRegistry::get(cc().getServiceContext()).shutDown();
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

TEST_F(TenantMigrationRecipientAccessBlockerTest, StateReject) {
    TenantMigrationRecipientAccessBlocker mtab(getServiceContext(), getMigrationId());

    {
        BSONObjBuilder builder;
        mtab.appendInfoForServerStatus(&builder);
        ASSERT_BSONOBJ_EQ(builder.obj(),
                          BSON("migrationId" << getMigrationId() << "state"
                                             << "reject"
                                             << "ttlIsBlocked" << true));
    }

    // Default read concern.
    ASSERT_THROWS_CODE(
        mtab.getCanReadFuture(opCtx(), "find").get(), DBException, ErrorCodes::SnapshotTooOld);

    // Majority read concern.
    ReadConcernArgs::get(opCtx()) = ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern);
    ASSERT_THROWS_CODE(
        mtab.getCanReadFuture(opCtx(), "find").get(), DBException, ErrorCodes::SnapshotTooOld);

    // Snapshot read concern.
    ReadConcernArgs::get(opCtx()) = ReadConcernArgs(ReadConcernLevel::kSnapshotReadConcern);
    opCtx()->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                    Timestamp(1, 1));
    ASSERT_THROWS_CODE(
        mtab.getCanReadFuture(opCtx(), "find").get(), DBException, ErrorCodes::SnapshotTooOld);

    // Snapshot read concern with atClusterTime.
    ReadConcernArgs::get(opCtx()) = ReadConcernArgs(ReadConcernLevel::kSnapshotReadConcern);
    ReadConcernArgs::get(opCtx()).setArgsAtClusterTimeForSnapshot(Timestamp(1, 1));
    ASSERT_THROWS_CODE(
        mtab.getCanReadFuture(opCtx(), "find").get(), DBException, ErrorCodes::SnapshotTooOld);
}

TEST_F(TenantMigrationRecipientAccessBlockerTest, StateRejectBefore) {
    TenantMigrationRecipientAccessBlocker mtab(getServiceContext(), getMigrationId());

    mtab.startRejectingReadsBefore(Timestamp(1, 1));
    {
        BSONObjBuilder builder;
        mtab.appendInfoForServerStatus(&builder);
        ASSERT_BSONOBJ_EQ(builder.obj(),
                          BSON("migrationId" << getMigrationId() << "state"
                                             << "rejectBefore"
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
                                             << "rejectBefore"
                                             << "rejectBeforeTimestamp" << Timestamp(2, 1)
                                             << "ttlIsBlocked" << true));
    }

    // Default read concern.
    ASSERT_OK(mtab.getCanReadFuture(opCtx(), "find").getNoThrow());

    // Majority read concern.
    ReadConcernArgs::get(opCtx()) = ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern);
    ASSERT_OK(mtab.getCanReadFuture(opCtx(), "find").getNoThrow());

    // Snapshot read at a later timestamp.
    ReadConcernArgs::get(opCtx()) = ReadConcernArgs(ReadConcernLevel::kSnapshotReadConcern);
    ReadConcernArgs::get(opCtx()).setArgsAtClusterTimeForSnapshot(Timestamp(3, 1));
    ASSERT_OK(mtab.getCanReadFuture(opCtx(), "find").getNoThrow());

    // Snapshot read at an earlier timestamp.
    ReadConcernArgs::get(opCtx()) = ReadConcernArgs(ReadConcernLevel::kSnapshotReadConcern);
    ReadConcernArgs::get(opCtx()).setArgsAtClusterTimeForSnapshot(Timestamp(1, 1));
    ASSERT_THROWS_CODE(
        mtab.getCanReadFuture(opCtx(), "find").get(), DBException, ErrorCodes::SnapshotTooOld);
}

}  // namespace repl
}  // namespace mongo
