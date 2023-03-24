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

#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_donor_access_blocker.h"
#include "mongo/db/repl/tenant_migration_recipient_access_blocker.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class TenantMigrationAccessBlockerUtilTest : public ServiceContextTest {
public:
    const TenantId kTenantId = TenantId(OID::gen());
    const DatabaseName kTenantDB = DatabaseName(kTenantId.toString() + "_ db");

    void setUp() {
        _opCtx = makeOperationContext();
        TenantMigrationAccessBlockerRegistry::get(getServiceContext()).startup();
    }

    void tearDown() {
        TenantMigrationAccessBlockerRegistry::get(getServiceContext()).shutDown();
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};


TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationInitiallyFalse) {
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationTrueWithDonor) {
    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, donorMtab);

    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveShardMergeTrueWithDonor) {
    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext())
        .addGlobalDonorAccessBlocker(donorMtab);
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), "local"_sd));
    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationTrueWithRecipient) {
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);

    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationTrueWithBoth) {
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);

    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, donorMtab);

    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveShardMergeTrueWithBoth) {
    auto uuid = UUID::gen();
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), uuid);
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);

    auto donorMtab = std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid);
    TenantMigrationAccessBlockerRegistry::get(getServiceContext())
        .addGlobalDonorAccessBlocker(donorMtab);
    // Access blocker do not impact ns without tenants.
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), "config"_sd));
    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationDonorFalseForNoDbName) {
    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, donorMtab);

    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), StringData()));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveShardMergeDonorFalseForNoDbName) {
    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext())
        .addGlobalDonorAccessBlocker(donorMtab);
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), StringData()));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveShardMergeRecipientFalseForNoDbName) {
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), StringData()));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationFalseForUnrelatedDb) {
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);

    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, donorMtab);

    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), "config"_sd));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationFalseAfterRemoveWithBoth) {
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);

    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, donorMtab);

    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));

    // Remove donor, should still be a migration.
    TenantMigrationAccessBlockerRegistry::get(getServiceContext())
        .remove(kTenantId, TenantMigrationAccessBlocker::BlockerType::kDonor);
    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));

    // Remove recipient, there should be no migration.
    TenantMigrationAccessBlockerRegistry::get(getServiceContext())
        .remove(kTenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveShardMergeFalseAfterRemoveWithBoth) {
    auto migrationId = UUID::gen();
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), migrationId);
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);

    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), migrationId);
    TenantMigrationAccessBlockerRegistry::get(getServiceContext())
        .addGlobalDonorAccessBlocker(donorMtab);

    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), "admin"_sd));

    // Remove donor, should still be a migration for the tenants migrating to the recipient.
    TenantMigrationAccessBlockerRegistry::get(getServiceContext())
        .removeAccessBlockersForMigration(migrationId,
                                          TenantMigrationAccessBlocker::BlockerType::kDonor);
    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), "admin"_sd));

    // Remove recipient, there should be no migration.
    TenantMigrationAccessBlockerRegistry::get(getServiceContext())
        .remove(kTenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), "admin"_sd));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, TestValidateNssBeingMigrated) {
    auto migrationId = UUID::gen();
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), migrationId);
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);

    // No tenantId should work for an adminDB.
    tenant_migration_access_blocker::validateNssIsBeingMigrated(
        boost::none,
        NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin, "test"),
        UUID::gen());

    // No tenantId will throw if it's not an adminDB.
    ASSERT_THROWS_CODE(tenant_migration_access_blocker::validateNssIsBeingMigrated(
                           boost::none,
                           NamespaceString::createNamespaceString_forTest("foo", "test"),
                           migrationId),
                       DBException,
                       ErrorCodes::InvalidTenantId);

    // A different tenantId will throw.
    ASSERT_THROWS_CODE(tenant_migration_access_blocker::validateNssIsBeingMigrated(
                           TenantId(OID::gen()),
                           NamespaceString::createNamespaceString_forTest("foo", "test"),
                           migrationId),
                       DBException,
                       ErrorCodes::InvalidTenantId);

    // A different migrationId will throw.
    ASSERT_THROWS_CODE(
        tenant_migration_access_blocker::validateNssIsBeingMigrated(
            kTenantId, NamespaceString::createNamespaceString_forTest("foo", "test"), UUID::gen()),
        DBException,
        ErrorCodes::InvalidTenantId);

    // Finally everything works.
    tenant_migration_access_blocker::validateNssIsBeingMigrated(
        kTenantId,
        NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin, "test"),
        migrationId);
}

class RecoverAccessBlockerTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext);
        _replMock = replCoord.get();
        repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

        {
            auto opCtx = makeOperationContext();
            repl::createOplog(opCtx.get());
        }

        stepUp();
    }

protected:
    void insertStateDocument(const NamespaceString& nss, const BSONObj& obj) {
        auto opCtx = makeOperationContext();

        AutoGetCollection collection(opCtx.get(), nss, MODE_IX);

        writeConflictRetry(opCtx.get(), "insertStateDocument", nss.ns(), [&]() {
            const auto filter = BSON("_id" << obj["_id"]);
            const auto updateMod = BSON("$setOnInsert" << obj);
            auto updateResult =
                Helpers::upsert(opCtx.get(), nss, filter, updateMod, /*fromMigrate=*/false);

            invariant(!updateResult.numDocsModified);
            invariant(!updateResult.upsertedId.isEmpty());
        });
    }

    void stepUp() {
        auto opCtx = makeOperationContext();
        auto replCoord = repl::ReplicationCoordinator::get(getServiceContext());
        auto currOpTime = replCoord->getMyLastAppliedOpTime();

        // Advance the term and last applied opTime. We retain the timestamp component of the
        // current last applied opTime to avoid log messages from
        // ReplClientInfo::setLastOpToSystemLastOpTime() about the opTime having moved backwards.
        ++_term;
        auto newOpTime = repl::OpTime{currOpTime.getTimestamp(), _term};

        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        ASSERT_OK(replCoord->updateTerm(opCtx.get(), _term));
        replCoord->setMyLastAppliedOpTimeAndWallTime({newOpTime, {}});
    }

    long long _term = 0;
    UUID _uuid = UUID::gen();
    MockReplicaSet _replSet{
        "donorSetForTest", 3, true /* hasPrimary */, false /* dollarPrefixHosts */};
    Timestamp _startMigration{10, 1};
    TenantMigrationRecipientDocument _recipientDoc{
        _uuid,
        _replSet.getConnectionString(),
        "" /* tenantId */,
        _startMigration,
        mongo::ReadPreferenceSetting(ReadPreference::PrimaryOnly)};
    TenantMigrationDonorDocument _donorDoc{
        _uuid,
        _replSet.getConnectionString(),
        mongo::ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        ""};
    std::vector<TenantId> _tenantIds{TenantId{OID::gen()}, TenantId{OID::gen()}};
    repl::ReplicationCoordinatorMock* _replMock;
};

TEST_F(RecoverAccessBlockerTest, ShardMergeRecipientBlockerStarted) {
    _recipientDoc.setProtocol(MigrationProtocolEnum::kShardMerge);
    _recipientDoc.setTenantIds(_tenantIds);
    _recipientDoc.setState(TenantMigrationRecipientStateEnum::kStarted);

    insertStateDocument(NamespaceString::kTenantMigrationRecipientsNamespace,
                        _recipientDoc.toBSON());

    auto opCtx = makeOperationContext();
    tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx.get());

    for (const auto& tenantId : _tenantIds) {
        auto mtab = TenantMigrationAccessBlockerRegistry::get(getServiceContext())
                        .getTenantMigrationAccessBlockerForTenantId(
                            tenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
        ASSERT(mtab);
        auto readFuture = mtab->getCanReadFuture(opCtx.get(), "dummyCmd");
        ASSERT_TRUE(readFuture.isReady());
        ASSERT_THROWS_CODE_AND_WHAT(readFuture.get(),
                                    DBException,
                                    ErrorCodes::SnapshotTooOld,
                                    "Tenant read is not allowed before migration completes");
    }
}

TEST_F(RecoverAccessBlockerTest, ShardMergeRecipientAbortedWithoutFCV) {
    _recipientDoc.setProtocol(MigrationProtocolEnum::kShardMerge);
    _recipientDoc.setTenantIds(_tenantIds);
    _recipientDoc.setState(TenantMigrationRecipientStateEnum::kAborted);

    insertStateDocument(NamespaceString::kTenantMigrationRecipientsNamespace,
                        _recipientDoc.toBSON());

    auto opCtx = makeOperationContext();
    tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx.get());

    for (const auto& tenantId : _tenantIds) {
        auto mtab = TenantMigrationAccessBlockerRegistry::get(getServiceContext())
                        .getTenantMigrationAccessBlockerForTenantId(
                            tenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
        ASSERT(!mtab);
    }
}

TEST_F(RecoverAccessBlockerTest, ShardMergeRecipientAbortedWithFCV) {
    _recipientDoc.setProtocol(MigrationProtocolEnum::kShardMerge);
    _recipientDoc.setTenantIds(_tenantIds);
    _recipientDoc.setState(TenantMigrationRecipientStateEnum::kAborted);
    _recipientDoc.setRecipientPrimaryStartingFCV(
        multiversion::FeatureCompatibilityVersion::kVersion_6_3);

    insertStateDocument(NamespaceString::kTenantMigrationRecipientsNamespace,
                        _recipientDoc.toBSON());

    auto opCtx = makeOperationContext();
    tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx.get());

    for (const auto& tenantId : _tenantIds) {
        auto mtab = TenantMigrationAccessBlockerRegistry::get(getServiceContext())
                        .getTenantMigrationAccessBlockerForTenantId(
                            tenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
        ASSERT(mtab);
        auto readFuture = mtab->getCanReadFuture(opCtx.get(), "dummyCmd");
        ASSERT_TRUE(readFuture.isReady());
        ASSERT_THROWS_CODE_AND_WHAT(readFuture.get(),
                                    DBException,
                                    ErrorCodes::SnapshotTooOld,
                                    "Tenant read is not allowed before migration completes");
    }
}

TEST_F(RecoverAccessBlockerTest, ShardMergeRecipientCommittedWithoutFCV) {
    _recipientDoc.setProtocol(MigrationProtocolEnum::kShardMerge);
    _recipientDoc.setTenantIds(_tenantIds);
    _recipientDoc.setState(TenantMigrationRecipientStateEnum::kCommitted);

    insertStateDocument(NamespaceString::kTenantMigrationRecipientsNamespace,
                        _recipientDoc.toBSON());

    auto opCtx = makeOperationContext();
    tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx.get());

    for (const auto& tenantId : _tenantIds) {
        auto mtab = TenantMigrationAccessBlockerRegistry::get(getServiceContext())
                        .getTenantMigrationAccessBlockerForTenantId(
                            tenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
        ASSERT(!mtab);
    }
}

TEST_F(RecoverAccessBlockerTest, ShardMergeRecipientCommittedWithFCV) {
    _recipientDoc.setProtocol(MigrationProtocolEnum::kShardMerge);
    _recipientDoc.setTenantIds(_tenantIds);
    _recipientDoc.setState(TenantMigrationRecipientStateEnum::kCommitted);
    _recipientDoc.setRecipientPrimaryStartingFCV(
        multiversion::FeatureCompatibilityVersion::kVersion_6_3);

    insertStateDocument(NamespaceString::kTenantMigrationRecipientsNamespace,
                        _recipientDoc.toBSON());

    auto opCtx = makeOperationContext();
    tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx.get());

    for (const auto& tenantId : _tenantIds) {
        auto mtab = TenantMigrationAccessBlockerRegistry::get(getServiceContext())
                        .getTenantMigrationAccessBlockerForTenantId(
                            tenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
        ASSERT(mtab);
        auto readFuture = mtab->getCanReadFuture(opCtx.get(), "dummyCmd");
        ASSERT_TRUE(readFuture.isReady());
        ASSERT_THROWS_CODE_AND_WHAT(readFuture.get(),
                                    DBException,
                                    ErrorCodes::SnapshotTooOld,
                                    "Tenant read is not allowed before migration completes");
    }
}

TEST_F(RecoverAccessBlockerTest, ShardMergeRecipientLearnedFiles) {
    _recipientDoc.setProtocol(MigrationProtocolEnum::kShardMerge);
    _recipientDoc.setTenantIds(_tenantIds);
    _recipientDoc.setState(TenantMigrationRecipientStateEnum::kLearnedFilenames);

    insertStateDocument(NamespaceString::kTenantMigrationRecipientsNamespace,
                        _recipientDoc.toBSON());

    auto opCtx = makeOperationContext();
    tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx.get());

    for (const auto& tenantId : _tenantIds) {
        auto mtab = TenantMigrationAccessBlockerRegistry::get(getServiceContext())
                        .getTenantMigrationAccessBlockerForTenantId(
                            tenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
        ASSERT(mtab);
    }
}

TEST_F(RecoverAccessBlockerTest, ShardMergeRecipientConsistent) {
    _recipientDoc.setProtocol(MigrationProtocolEnum::kShardMerge);
    _recipientDoc.setTenantIds(_tenantIds);
    _recipientDoc.setState(TenantMigrationRecipientStateEnum::kConsistent);

    insertStateDocument(NamespaceString::kTenantMigrationRecipientsNamespace,
                        _recipientDoc.toBSON());

    auto opCtx = makeOperationContext();
    tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx.get());

    for (const auto& tenantId : _tenantIds) {
        auto mtab = TenantMigrationAccessBlockerRegistry::get(getServiceContext())
                        .getTenantMigrationAccessBlockerForTenantId(
                            tenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
        ASSERT(mtab);
        auto readFuture = mtab->getCanReadFuture(opCtx.get(), "dummyCmd");
        ASSERT_TRUE(readFuture.isReady());
        ASSERT_THROWS_CODE_AND_WHAT(readFuture.get(),
                                    DBException,
                                    ErrorCodes::SnapshotTooOld,
                                    "Tenant read is not allowed before migration completes");
    }
}

TEST_F(RecoverAccessBlockerTest, ShardMergeRecipientRejectBeforeTimestamp) {
    _recipientDoc.setProtocol(MigrationProtocolEnum::kShardMerge);
    _recipientDoc.setTenantIds(_tenantIds);
    _recipientDoc.setState(TenantMigrationRecipientStateEnum::kCommitted);
    _recipientDoc.setRejectReadsBeforeTimestamp(Timestamp{20, 1});
    _recipientDoc.setRecipientPrimaryStartingFCV(
        multiversion::FeatureCompatibilityVersion::kVersion_6_3);

    insertStateDocument(NamespaceString::kTenantMigrationRecipientsNamespace,
                        _recipientDoc.toBSON());

    {
        auto opCtx = makeOperationContext();
        tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx.get());
    }

    for (const auto& tenantId : _tenantIds) {
        auto opCtx = makeOperationContext();
        auto mtab = TenantMigrationAccessBlockerRegistry::get(getServiceContext())
                        .getTenantMigrationAccessBlockerForTenantId(
                            tenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
        ASSERT(mtab);
        auto readFuture = mtab->getCanReadFuture(opCtx.get(), "dummyCmd");
        ASSERT_TRUE(readFuture.isReady());
        ASSERT_OK(readFuture.getNoThrow());

        repl::ReadConcernArgs::get(opCtx.get()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
        repl::ReadConcernArgs::get(opCtx.get()).setArgsAtClusterTimeForSnapshot(Timestamp{15, 1});
        auto readFutureAtClusterTime = mtab->getCanReadFuture(opCtx.get(), "dummyCmd");
        ASSERT_TRUE(readFuture.isReady());
        ASSERT_OK(readFuture.getNoThrow());
    }
}

TEST_F(RecoverAccessBlockerTest, ShardMergeDonorAbortingIndex) {
    _donorDoc.setProtocol(MigrationProtocolEnum::kShardMerge);
    _donorDoc.setTenantIds(_tenantIds);
    _donorDoc.setState(TenantMigrationDonorStateEnum::kAbortingIndexBuilds);

    insertStateDocument(NamespaceString::kTenantMigrationDonorsNamespace, _donorDoc.toBSON());

    {
        auto opCtx = makeOperationContext();
        tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx.get());
    }

    for (const auto& tenantId : _tenantIds) {
        auto opCtx = makeOperationContext();
        auto mtab = TenantMigrationAccessBlockerRegistry::get(getServiceContext())
                        .getTenantMigrationAccessBlockerForTenantId(
                            tenantId, TenantMigrationAccessBlocker::BlockerType::kDonor);
        ASSERT(mtab);
        auto readFuture = mtab->getCanReadFuture(opCtx.get(), "dummyCmd");
        ASSERT_TRUE(readFuture.isReady());
        ASSERT_OK(readFuture.getNoThrow());
        ASSERT_OK(mtab->checkIfCanWrite(Timestamp{10, 1}));

        auto indexStatus = mtab->checkIfCanBuildIndex();
        ASSERT_EQ(indexStatus.code(), ErrorCodes::TenantMigrationConflict);
        auto migrationConflictInfo = indexStatus.extraInfo<TenantMigrationConflictInfo>();
        ASSERT_EQ(migrationConflictInfo->getMigrationId(), _uuid);
    }
}

TEST_F(RecoverAccessBlockerTest, ShardMergeDonorBlocking) {
    _donorDoc.setProtocol(MigrationProtocolEnum::kShardMerge);
    _donorDoc.setTenantIds(_tenantIds);
    _donorDoc.setState(TenantMigrationDonorStateEnum::kBlocking);
    _donorDoc.setBlockTimestamp(Timestamp{100, 1});

    insertStateDocument(NamespaceString::kTenantMigrationDonorsNamespace, _donorDoc.toBSON());

    {
        auto opCtx = makeOperationContext();
        tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx.get());
    }

    for (const auto& tenantId : _tenantIds) {
        auto opCtx = makeOperationContext();
        auto mtab = TenantMigrationAccessBlockerRegistry::get(getServiceContext())
                        .getTenantMigrationAccessBlockerForTenantId(
                            tenantId, TenantMigrationAccessBlocker::BlockerType::kDonor);
        ASSERT(mtab);
        auto readFuture = mtab->getCanReadFuture(opCtx.get(), "dummyCmd");
        ASSERT_TRUE(readFuture.isReady());
        ASSERT_OK(readFuture.getNoThrow());
        ASSERT_EQ(mtab->checkIfCanWrite(Timestamp{101, 1}).code(),
                  ErrorCodes::TenantMigrationConflict);

        repl::ReadConcernArgs::get(opCtx.get()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
        repl::ReadConcernArgs::get(opCtx.get()).setArgsAtClusterTimeForSnapshot(Timestamp{101, 1});
        auto afterReadFuture = mtab->getCanReadFuture(opCtx.get(), "dummyCmd");
        ASSERT_FALSE(afterReadFuture.isReady());

        auto indexStatus = mtab->checkIfCanBuildIndex();
        ASSERT_EQ(indexStatus.code(), ErrorCodes::TenantMigrationConflict);
    }
}

TEST_F(RecoverAccessBlockerTest, ShardMergeDonorCommitted) {
    _donorDoc.setProtocol(MigrationProtocolEnum::kShardMerge);
    _donorDoc.setTenantIds(_tenantIds);
    _donorDoc.setState(TenantMigrationDonorStateEnum::kCommitted);
    _donorDoc.setBlockTimestamp(Timestamp{100, 1});
    _donorDoc.setCommitOrAbortOpTime(repl::OpTime{Timestamp{101, 1}, 2});

    _replMock->setCurrentCommittedSnapshotOpTime(repl::OpTime{Timestamp{101, 1}, 2});
    insertStateDocument(NamespaceString::kTenantMigrationDonorsNamespace, _donorDoc.toBSON());

    {
        auto opCtx = makeOperationContext();
        tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx.get());
    }

    for (const auto& tenantId : _tenantIds) {
        auto opCtx = makeOperationContext();
        auto mtab = TenantMigrationAccessBlockerRegistry::get(getServiceContext())
                        .getTenantMigrationAccessBlockerForTenantId(
                            tenantId, TenantMigrationAccessBlocker::BlockerType::kDonor);
        ASSERT(mtab);
        repl::ReadConcernArgs::get(opCtx.get()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
        repl::ReadConcernArgs::get(opCtx.get()).setArgsAtClusterTimeForSnapshot(Timestamp{90, 1});
        auto readFuture = mtab->getCanReadFuture(opCtx.get(), "dummyCmd");
        ASSERT_TRUE(readFuture.isReady());
        ASSERT_OK(readFuture.getNoThrow());
        ASSERT_EQ(mtab->checkIfCanWrite(Timestamp{102, 1}).code(),
                  ErrorCodes::TenantMigrationCommitted);

        repl::ReadConcernArgs::get(opCtx.get()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
        repl::ReadConcernArgs::get(opCtx.get()).setArgsAtClusterTimeForSnapshot(Timestamp{102, 1});
        auto afterReadFuture = mtab->getCanReadFuture(opCtx.get(), "dummyCmd");
        ASSERT_TRUE(afterReadFuture.isReady());
        ASSERT_EQ(afterReadFuture.getNoThrow().code(), ErrorCodes::TenantMigrationCommitted);
        auto indexStatus = mtab->checkIfCanBuildIndex();
        ASSERT_EQ(indexStatus.code(), ErrorCodes::TenantMigrationCommitted);
    }
}

TEST_F(RecoverAccessBlockerTest, ShardMergeDonorAborted) {
    _donorDoc.setProtocol(MigrationProtocolEnum::kShardMerge);
    _donorDoc.setTenantIds(_tenantIds);
    _donorDoc.setState(TenantMigrationDonorStateEnum::kAborted);
    _donorDoc.setBlockTimestamp(Timestamp{100, 1});
    _donorDoc.setCommitOrAbortOpTime(repl::OpTime{Timestamp{101, 1}, 2});

    _replMock->setCurrentCommittedSnapshotOpTime(repl::OpTime{Timestamp{101, 1}, 2});
    insertStateDocument(NamespaceString::kTenantMigrationDonorsNamespace, _donorDoc.toBSON());

    {
        auto opCtx = makeOperationContext();
        tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx.get());
    }

    for (const auto& tenantId : _tenantIds) {
        auto opCtx = makeOperationContext();
        auto mtab = TenantMigrationAccessBlockerRegistry::get(getServiceContext())
                        .getTenantMigrationAccessBlockerForTenantId(
                            tenantId, TenantMigrationAccessBlocker::BlockerType::kDonor);
        ASSERT(mtab);
        repl::ReadConcernArgs::get(opCtx.get()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
        repl::ReadConcernArgs::get(opCtx.get()).setArgsAtClusterTimeForSnapshot(Timestamp{90, 1});
        auto readFuture = mtab->getCanReadFuture(opCtx.get(), "dummyCmd");
        ASSERT_TRUE(readFuture.isReady());
        ASSERT_OK(readFuture.getNoThrow());
        ASSERT_OK(mtab->checkIfCanWrite(Timestamp{102, 1}));

        repl::ReadConcernArgs::get(opCtx.get()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
        repl::ReadConcernArgs::get(opCtx.get()).setArgsAtClusterTimeForSnapshot(Timestamp{102, 1});
        auto afterReadFuture = mtab->getCanReadFuture(opCtx.get(), "dummyCmd");
        ASSERT_TRUE(afterReadFuture.isReady());
        ASSERT_OK(afterReadFuture.getNoThrow());
        ASSERT_OK(mtab->checkIfCanBuildIndex());
    }
}

}  // namespace mongo
