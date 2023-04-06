/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/shard_merge_recipient_op_observer.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::repl {

namespace {
const Timestamp kDefaultStartMigrationTimestamp(1, 1);
static const std::string kDefaultDonorConnStr = "donor-rs/localhost:12345";
static const std::string kDefaultRecipientConnStr = "recipient-rs/localhost:56789";
static const UUID kMigrationId = UUID::gen();

}  // namespace

class ShardMergeRecipientOpObserverTest : public ServiceContextMongoDTest {
public:
    void setUp() {
        ServiceContextMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        repl::StorageInterface::set(serviceContext, std::make_unique<repl::StorageInterfaceMock>());

        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

        _opCtx = makeOperationContext();
        TenantMigrationAccessBlockerRegistry::get(getServiceContext()).startup();

        repl::createOplog(opCtx());
        ASSERT_OK(createCollection(opCtx(),
                                   CreateCommand(NamespaceString::kShardMergeRecipientsNamespace)));
    }

    void tearDown() {
        TenantMigrationAccessBlockerRegistry::get(getServiceContext()).shutDown();
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

protected:
    void performUpdates(const BSONObj& UpdatedDoc, const BSONObj& preImageDoc) {
        AutoGetCollection collection(
            opCtx(), NamespaceString::kShardMergeRecipientsNamespace, MODE_IX);
        if (!collection)
            FAIL(str::stream() << "Collection " << NamespaceString::kShardMergeRecipientsNamespace
                               << " doesn't exist");

        CollectionUpdateArgs updateArgs{preImageDoc};
        updateArgs.updatedDoc = UpdatedDoc;

        OplogUpdateEntryArgs update(&updateArgs, *collection);

        WriteUnitOfWork wuow(opCtx());
        _observer.onUpdate(opCtx(), update);
        wuow.commit();
    }

    std::vector<TenantId> _tenantIds{TenantId{OID::gen()}};

private:
    ShardMergeRecipientOpObserver _observer;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(ShardMergeRecipientOpObserverTest, TransitionToConsistentWithImportDoneMarkerCollection) {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    recipientDoc.setState(ShardMergeRecipientStateEnum::kLearnedFilenames);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kConsistent);
    auto updatedDoc = recipientDoc.toBSON();

    // Create the import done marker collection.
    ASSERT_OK(createCollection(
        opCtx(), CreateCommand(shard_merge_utils::getImportDoneMarkerNs(kMigrationId))));

    performUpdates(updatedDoc, preImageDoc);
}

DEATH_TEST_REGEX_F(ShardMergeRecipientOpObserverTest,
                   TransitionToConsistentWithoutImportDoneMarkerCollection,
                   "Fatal assertion.*7219902") {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    recipientDoc.setState(ShardMergeRecipientStateEnum::kLearnedFilenames);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kConsistent);
    auto updatedDoc = recipientDoc.toBSON();

    performUpdates(updatedDoc, preImageDoc);
}

}  // namespace mongo::repl
