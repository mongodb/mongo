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

#include <boost/filesystem/fstream.hpp>
#include <memory>
#include <string>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/shard_merge_recipient_op_observer.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/serverless/serverless_operation_lock_registry.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/log_test.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

namespace mongo::repl {

using namespace shard_merge_utils;

namespace {
const Timestamp kDefaultStartMigrationTimestamp(1, 1);
static const std::string kDefaultDonorConnStr = "donor-rs/localhost:12345";
static const std::string kDefaultRecipientConnStr = "recipient-rs/localhost:56789";
static const UUID kMigrationId = UUID::gen();

}  // namespace

class ShardMergeRecipientOpObserverTest : public ServiceContextMongoDTest {
public:
    static bool collectionExists(OperationContext* opCtx, const NamespaceString& nss) {
        return static_cast<bool>(AutoGetCollectionForRead(opCtx, nss).getCollection());
    }

    void setUp() override {
        ServiceContextMongoDTest::setUp();

        auto serviceContext = getServiceContext();

        // Need real (non-mock) storage for testing dropping marker collection.
        StorageInterface::set(serviceContext, std::make_unique<StorageInterfaceImpl>());

        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

        _opCtx = makeOperationContext();
        TenantMigrationAccessBlockerRegistry::get(getServiceContext()).startup();

        repl::createOplog(opCtx());
        ASSERT_OK(createCollection(opCtx(),
                                   CreateCommand(NamespaceString::kShardMergeRecipientsNamespace)));
    }

    void tearDown() override {
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
            FAIL(str::stream()
                 << "Collection "
                 << NamespaceString::kShardMergeRecipientsNamespace.toStringForErrorMsg()
                 << " doesn't exist");

        CollectionUpdateArgs updateArgs{preImageDoc};
        updateArgs.updatedDoc = UpdatedDoc;

        OplogUpdateEntryArgs update(&updateArgs, *collection);

        WriteUnitOfWork wuow(opCtx());
        _observer.onUpdate(opCtx(), update);
        wuow.commit();
    }

    int64_t countLogLinesWithId(int32_t id) {
        return countBSONFormatLogLinesIsSubset(BSON("id" << id));
    }

    std::vector<TenantId> _tenantIds{TenantId{OID::gen()}, TenantId{OID::gen()}};

private:
    unittest::MinimumLoggedSeverityGuard _tenantMigrationSeverityGuard{
        logv2::LogComponent::kTenantMigration, logv2::LogSeverity::Debug(1)};

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

TEST_F(ShardMergeRecipientOpObserverTest, TransitionToAbortedDropsImportedCollection) {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    recipientDoc.setState(ShardMergeRecipientStateEnum::kConsistent);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kAborted);
    recipientDoc.setAbortOpTime(OpTime(Timestamp::max(), 1));
    auto updatedDoc = recipientDoc.toBSON();

    const NamespaceString importedDonorCollNss1 =
        NamespaceString::createNamespaceString_forTest(_tenantIds[0].toString() + "_test.coll1");
    ASSERT_OK(createCollection(opCtx(), CreateCommand(importedDonorCollNss1)));

    const NamespaceString importedDonorCollNss2 =
        NamespaceString::createNamespaceString_forTest(_tenantIds[1].toString() + "_test.coll2");
    ASSERT_OK(createCollection(opCtx(), CreateCommand(importedDonorCollNss2)));

    ASSERT(collectionExists(opCtx(), importedDonorCollNss1));
    ASSERT(collectionExists(opCtx(), importedDonorCollNss2));

    performUpdates(updatedDoc, preImageDoc);

    ASSERT(!collectionExists(opCtx(), importedDonorCollNss1));
    ASSERT(!collectionExists(opCtx(), importedDonorCollNss2));
}

TEST_F(ShardMergeRecipientOpObserverTest, TransitionToCommmittedShouldNotDropImportedCollection) {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    recipientDoc.setState(ShardMergeRecipientStateEnum::kConsistent);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kCommitted);
    auto updatedDoc = recipientDoc.toBSON();

    const NamespaceString importedDonorCollNss1 =
        NamespaceString::createNamespaceString_forTest(_tenantIds[0].toString() + "_test.coll1");
    ASSERT_OK(createCollection(opCtx(), CreateCommand(importedDonorCollNss1)));

    const NamespaceString importedDonorCollNss2 =
        NamespaceString::createNamespaceString_forTest(_tenantIds[1].toString() + "_test.coll2");
    ASSERT_OK(createCollection(opCtx(), CreateCommand(importedDonorCollNss2)));

    ASSERT(collectionExists(opCtx(), importedDonorCollNss1));
    ASSERT(collectionExists(opCtx(), importedDonorCollNss2));

    performUpdates(updatedDoc, preImageDoc);

    ASSERT(collectionExists(opCtx(), importedDonorCollNss1));
    ASSERT(collectionExists(opCtx(), importedDonorCollNss2));
}

TEST_F(ShardMergeRecipientOpObserverTest,
       TransitionToAbortedGarbageCollectableShouldDropTempFilesAndMarkerCollection) {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    ServerlessOperationLockRegistry::get(opCtx()->getServiceContext())
        .acquireLock(ServerlessOperationLockRegistry::LockType::kMergeRecipient, kMigrationId);

    recipientDoc.setState(ShardMergeRecipientStateEnum::kLearnedFilenames);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kAborted);
    recipientDoc.setAbortOpTime(OpTime(Timestamp::max(), 1));
    recipientDoc.setExpireAt(opCtx()->getServiceContext()->getFastClockSource()->now());
    auto updatedDoc = recipientDoc.toBSON();

    auto knownIdentListBeforeGC = DurableCatalog::get(opCtx())->getAllIdents(opCtx());

    // Create idents unknown to storage.
    const auto unknownIdent1 = "collection-70--88888";
    const auto unknownIdentPath1 = constructDestinationPath(unknownIdent1);
    boost::filesystem::ofstream unknownIdent1Writer(unknownIdentPath1);
    unknownIdent1Writer << "Dummy stream1 \n";
    unknownIdent1Writer.close();

    const auto unknownIdent2 = "index-71--88888";
    const auto unknownIdentPath2 = constructDestinationPath(unknownIdent2);
    boost::filesystem::ofstream unknownIdent2Writer(unknownIdentPath2);
    unknownIdent2Writer << "Dummy stream2 \n";
    unknownIdent2Writer.close();

    const auto fileClonerTempDirPath = fileClonerTempDir(kMigrationId);
    ASSERT_TRUE(boost::filesystem::create_directory(fileClonerTempDirPath));

    writeMovingFilesMarker(fileClonerTempDirPath, unknownIdent1, true);
    writeMovingFilesMarker(fileClonerTempDirPath, unknownIdent2, false);
    // GC shouldn't remove these known idents.
    for (const auto& ident : knownIdentListBeforeGC) {
        writeMovingFilesMarker(fileClonerTempDirPath, ident, false);
    }

    // Create the marker collection.
    createImportDoneMarkerLocalCollection(opCtx(), kMigrationId);

    // Verify that temp files and the marker collection exist before GC.
    ASSERT(collectionExists(opCtx(), getImportDoneMarkerNs(kMigrationId)));
    ASSERT(boost::filesystem::exists(unknownIdentPath1));
    ASSERT(boost::filesystem::exists(unknownIdentPath2));
    ASSERT(boost::filesystem::exists(fileClonerTempDirPath));

    startCapturingLogMessages();

    performUpdates(updatedDoc, preImageDoc);

    stopCapturingLogMessages();

    // Verify that temp files and the marker collection are deleted after GC.
    ASSERT(!collectionExists(opCtx(), getImportDoneMarkerNs(kMigrationId)));

    ASSERT(!boost::filesystem::exists(unknownIdentPath1));
    ASSERT(!boost::filesystem::exists(unknownIdentPath2));
    ASSERT(!boost::filesystem::exists(fileClonerTempDirPath));

    ASSERT_EQUALS(2, countLogLinesWithId(7458501));
    ASSERT_EQUALS(1, countLogLinesWithId(7458503));

    // Verify that GC didn't remove any known idents.
    const auto knownIdentListAfterGC = DurableCatalog::get(opCtx())->getAllIdents(opCtx());
    ASSERT(knownIdentListBeforeGC == knownIdentListAfterGC);
}

TEST_F(ShardMergeRecipientOpObserverTest,
       TransitionToCommittedGarbageCollectableShouldDropTempFilesAndMarkerCollection) {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    auto& registry = TenantMigrationAccessBlockerRegistry::get(getGlobalServiceContext());
    for (const auto& tenantId : _tenantIds) {
        registry.add(tenantId,
                     std::make_shared<TenantMigrationRecipientAccessBlocker>(
                         opCtx()->getServiceContext(), kMigrationId));
    }

    ServerlessOperationLockRegistry::get(opCtx()->getServiceContext())
        .acquireLock(ServerlessOperationLockRegistry::LockType::kMergeRecipient, kMigrationId);

    recipientDoc.setState(ShardMergeRecipientStateEnum::kConsistent);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kCommitted);
    recipientDoc.setExpireAt(opCtx()->getServiceContext()->getFastClockSource()->now());
    auto updatedDoc = recipientDoc.toBSON();

    auto knownIdentListBeforeGC = DurableCatalog::get(opCtx())->getAllIdents(opCtx());

    // Create idents unknown to storage.
    const auto unknownIdent1 = "collection-70--88888";
    const auto unknownIdentPath1 = constructDestinationPath(unknownIdent1);
    boost::filesystem::ofstream unknownIdent1Writer(unknownIdentPath1);
    unknownIdent1Writer << "Dummy stream1 \n";
    unknownIdent1Writer.close();

    const auto unknownIdent2 = "index-71--88888";
    const auto unknownIdentPath2 = constructDestinationPath(unknownIdent2);
    boost::filesystem::ofstream unknownIdent2Writer(unknownIdentPath2);
    unknownIdent2Writer << "Dummy stream2 \n";
    unknownIdent2Writer.close();

    const auto fileClonerTempDirPath = fileClonerTempDir(kMigrationId);
    ASSERT_TRUE(boost::filesystem::create_directory(fileClonerTempDirPath));

    writeMovingFilesMarker(fileClonerTempDirPath, unknownIdent1, true);
    writeMovingFilesMarker(fileClonerTempDirPath, unknownIdent2, false);
    // GC shouldn't remove these known idents.
    for (const auto& ident : knownIdentListBeforeGC) {
        writeMovingFilesMarker(fileClonerTempDirPath, ident, false);
    }

    // Create the marker collection.
    createImportDoneMarkerLocalCollection(opCtx(), kMigrationId);

    // Verify that temp files and the marker collection exist before GC.
    ASSERT(collectionExists(opCtx(), getImportDoneMarkerNs(kMigrationId)));
    ASSERT(boost::filesystem::exists(unknownIdentPath1));
    ASSERT(boost::filesystem::exists(unknownIdentPath2));
    ASSERT(boost::filesystem::exists(fileClonerTempDirPath));

    startCapturingLogMessages();

    performUpdates(updatedDoc, preImageDoc);

    stopCapturingLogMessages();

    // Verify that temp files and the marker collection are deleted after GC.
    ASSERT(!collectionExists(opCtx(), getImportDoneMarkerNs(kMigrationId)));

    ASSERT(!boost::filesystem::exists(unknownIdentPath1));
    ASSERT(!boost::filesystem::exists(unknownIdentPath2));
    ASSERT(!boost::filesystem::exists(fileClonerTempDirPath));

    ASSERT_EQUALS(2, countLogLinesWithId(7458501));
    ASSERT_EQUALS(1, countLogLinesWithId(7458503));

    // Verify that GC didn't remove any known idents.
    const auto knownIdentListAfterGC = DurableCatalog::get(opCtx())->getAllIdents(opCtx());
    ASSERT(knownIdentListBeforeGC == knownIdentListAfterGC);
}

TEST_F(ShardMergeRecipientOpObserverTest,
       TransitionToAbortedDropsImportedCollectionInStartupRecovery) {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    recipientDoc.setState(ShardMergeRecipientStateEnum::kConsistent);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kAborted);
    recipientDoc.setAbortOpTime(OpTime(Timestamp::max(), 1));
    auto updatedDoc = recipientDoc.toBSON();

    const NamespaceString importedDonorCollNss1 =
        NamespaceString::createNamespaceString_forTest(_tenantIds[0].toString() + "_test.coll1");
    ASSERT_OK(createCollection(opCtx(), CreateCommand(importedDonorCollNss1)));

    const NamespaceString importedDonorCollNss2 =
        NamespaceString::createNamespaceString_forTest(_tenantIds[1].toString() + "_test.coll2");
    ASSERT_OK(createCollection(opCtx(), CreateCommand(importedDonorCollNss2)));

    ASSERT(collectionExists(opCtx(), importedDonorCollNss1));
    ASSERT(collectionExists(opCtx(), importedDonorCollNss2));

    // Simulate the node is in startup repl state.
    ASSERT_OK(
        repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(repl::MemberState::RS_STARTUP));

    performUpdates(updatedDoc, preImageDoc);

    ASSERT(!collectionExists(opCtx(), importedDonorCollNss1));
    ASSERT(!collectionExists(opCtx(), importedDonorCollNss2));
}

TEST_F(ShardMergeRecipientOpObserverTest,
       TransitionToCommmittedShouldNotDropImportedCollectionInStartupRecovery) {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    recipientDoc.setState(ShardMergeRecipientStateEnum::kConsistent);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kCommitted);
    auto updatedDoc = recipientDoc.toBSON();

    const NamespaceString importedDonorCollNss1 =
        NamespaceString::createNamespaceString_forTest(_tenantIds[0].toString() + "_test.coll1");
    ASSERT_OK(createCollection(opCtx(), CreateCommand(importedDonorCollNss1)));

    const NamespaceString importedDonorCollNss2 =
        NamespaceString::createNamespaceString_forTest(_tenantIds[1].toString() + "_test.coll2");
    ASSERT_OK(createCollection(opCtx(), CreateCommand(importedDonorCollNss2)));

    ASSERT(collectionExists(opCtx(), importedDonorCollNss1));
    ASSERT(collectionExists(opCtx(), importedDonorCollNss2));

    // Simulate the node is in startup repl state.
    ASSERT_OK(
        repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(repl::MemberState::RS_STARTUP));
    performUpdates(updatedDoc, preImageDoc);

    ASSERT(collectionExists(opCtx(), importedDonorCollNss1));
    ASSERT(collectionExists(opCtx(), importedDonorCollNss2));
}

TEST_F(
    ShardMergeRecipientOpObserverTest,
    TransitionToAbortedGarbageCollectableShouldDropTempFilesAndMarkerCollectionInStartupRecovery) {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    recipientDoc.setState(ShardMergeRecipientStateEnum::kLearnedFilenames);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kAborted);
    recipientDoc.setAbortOpTime(OpTime(Timestamp::max(), 1));
    recipientDoc.setExpireAt(opCtx()->getServiceContext()->getFastClockSource()->now());
    auto updatedDoc = recipientDoc.toBSON();

    auto knownIdentListBeforeGC = DurableCatalog::get(opCtx())->getAllIdents(opCtx());

    // Create idents unknown to storage.
    const auto unknownIdent1 = "collection-70--88888";
    const auto unknownIdentPath1 = constructDestinationPath(unknownIdent1);
    boost::filesystem::ofstream unknownIdent1Writer(unknownIdentPath1);
    unknownIdent1Writer << "Dummy stream1 \n";
    unknownIdent1Writer.close();

    const auto unknownIdent2 = "index-71--88888";
    const auto unknownIdentPath2 = constructDestinationPath(unknownIdent2);
    boost::filesystem::ofstream unknownIdent2Writer(unknownIdentPath2);
    unknownIdent2Writer << "Dummy stream2 \n";
    unknownIdent2Writer.close();

    const auto fileClonerTempDirPath = fileClonerTempDir(kMigrationId);
    ASSERT_TRUE(boost::filesystem::create_directory(fileClonerTempDirPath));

    writeMovingFilesMarker(fileClonerTempDirPath, unknownIdent1, true);
    writeMovingFilesMarker(fileClonerTempDirPath, unknownIdent2, false);
    // GC shouldn't remove these known idents.
    for (const auto& ident : knownIdentListBeforeGC) {
        writeMovingFilesMarker(fileClonerTempDirPath, ident, false);
    }

    // Create the marker collection.
    createImportDoneMarkerLocalCollection(opCtx(), kMigrationId);

    // Verify that temp files and the marker collection exist before GC.
    ASSERT(collectionExists(opCtx(), getImportDoneMarkerNs(kMigrationId)));
    ASSERT(boost::filesystem::exists(unknownIdentPath1));
    ASSERT(boost::filesystem::exists(unknownIdentPath2));
    ASSERT(boost::filesystem::exists(fileClonerTempDirPath));

    startCapturingLogMessages();

    // Simulate the node is in startup repl state.
    ASSERT_OK(
        repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(repl::MemberState::RS_STARTUP));
    performUpdates(updatedDoc, preImageDoc);

    stopCapturingLogMessages();

    // Verify that temp files and the marker collection are deleted after GC.
    ASSERT(!collectionExists(opCtx(), getImportDoneMarkerNs(kMigrationId)));

    ASSERT(!boost::filesystem::exists(unknownIdentPath1));
    ASSERT(!boost::filesystem::exists(unknownIdentPath2));
    ASSERT(!boost::filesystem::exists(fileClonerTempDirPath));

    ASSERT_EQUALS(2, countLogLinesWithId(7458501));
    ASSERT_EQUALS(1, countLogLinesWithId(7458503));

    // Verify that GC didn't remove any known idents.
    const auto knownIdentListAfterGC = DurableCatalog::get(opCtx())->getAllIdents(opCtx());
    ASSERT(knownIdentListBeforeGC == knownIdentListAfterGC);
}

TEST_F(
    ShardMergeRecipientOpObserverTest,
    TransitionToCommittedGarbageCollectableShouldDropTempFilesAndMarkerCollectionInStartupRecovery) {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    recipientDoc.setState(ShardMergeRecipientStateEnum::kConsistent);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kCommitted);
    recipientDoc.setExpireAt(opCtx()->getServiceContext()->getFastClockSource()->now());
    auto updatedDoc = recipientDoc.toBSON();

    auto knownIdentListBeforeGC = DurableCatalog::get(opCtx())->getAllIdents(opCtx());

    // Create idents unknown to storage.
    const auto unknownIdent1 = "collection-70--88888";
    const auto unknownIdentPath1 = constructDestinationPath(unknownIdent1);
    boost::filesystem::ofstream unknownIdent1Writer(unknownIdentPath1);
    unknownIdent1Writer << "Dummy stream1 \n";
    unknownIdent1Writer.close();

    const auto unknownIdent2 = "index-71--88888";
    const auto unknownIdentPath2 = constructDestinationPath(unknownIdent2);
    boost::filesystem::ofstream unknownIdent2Writer(unknownIdentPath2);
    unknownIdent2Writer << "Dummy stream2 \n";
    unknownIdent2Writer.close();

    const auto fileClonerTempDirPath = fileClonerTempDir(kMigrationId);
    ASSERT_TRUE(boost::filesystem::create_directory(fileClonerTempDirPath));

    writeMovingFilesMarker(fileClonerTempDirPath, unknownIdent1, true);
    writeMovingFilesMarker(fileClonerTempDirPath, unknownIdent2, false);
    // GC shouldn't remove these known idents.
    for (const auto& ident : knownIdentListBeforeGC) {
        writeMovingFilesMarker(fileClonerTempDirPath, ident, false);
    }

    // Create the marker collection.
    createImportDoneMarkerLocalCollection(opCtx(), kMigrationId);

    // Verify that temp files and the marker collection exist before GC.
    ASSERT(collectionExists(opCtx(), getImportDoneMarkerNs(kMigrationId)));
    ASSERT(boost::filesystem::exists(unknownIdentPath1));
    ASSERT(boost::filesystem::exists(unknownIdentPath2));
    ASSERT(boost::filesystem::exists(fileClonerTempDirPath));

    startCapturingLogMessages();

    // Simulate the node is in startup repl state.
    ASSERT_OK(
        repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(repl::MemberState::RS_STARTUP));
    performUpdates(updatedDoc, preImageDoc);

    stopCapturingLogMessages();

    // Verify that temp files and the marker collection are deleted after GC.
    ASSERT(!collectionExists(opCtx(), getImportDoneMarkerNs(kMigrationId)));

    ASSERT(!boost::filesystem::exists(unknownIdentPath1));
    ASSERT(!boost::filesystem::exists(unknownIdentPath2));
    ASSERT(!boost::filesystem::exists(fileClonerTempDirPath));

    ASSERT_EQUALS(2, countLogLinesWithId(7458501));
    ASSERT_EQUALS(1, countLogLinesWithId(7458503));

    // Verify that GC didn't remove any known idents.
    const auto knownIdentListAfterGC = DurableCatalog::get(opCtx())->getAllIdents(opCtx());
    ASSERT(knownIdentListBeforeGC == knownIdentListAfterGC);
}

TEST_F(ShardMergeRecipientOpObserverTest,
       TransitionToAbortedDropsImportedCollectionInRollbackRecovery) {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    recipientDoc.setState(ShardMergeRecipientStateEnum::kConsistent);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kAborted);
    recipientDoc.setAbortOpTime(OpTime(Timestamp::max(), 1));
    auto updatedDoc = recipientDoc.toBSON();

    const NamespaceString importedDonorCollNss1 =
        NamespaceString::createNamespaceString_forTest(_tenantIds[0].toString() + "_test.coll1");
    ASSERT_OK(createCollection(opCtx(), CreateCommand(importedDonorCollNss1)));

    const NamespaceString importedDonorCollNss2 =
        NamespaceString::createNamespaceString_forTest(_tenantIds[1].toString() + "_test.coll2");
    ASSERT_OK(createCollection(opCtx(), CreateCommand(importedDonorCollNss2)));

    ASSERT(collectionExists(opCtx(), importedDonorCollNss1));
    ASSERT(collectionExists(opCtx(), importedDonorCollNss2));

    // Simulate the node is in rollback repl state.
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_ROLLBACK));

    performUpdates(updatedDoc, preImageDoc);

    ASSERT(!collectionExists(opCtx(), importedDonorCollNss1));
    ASSERT(!collectionExists(opCtx(), importedDonorCollNss2));
}

TEST_F(ShardMergeRecipientOpObserverTest,
       TransitionToCommmittedShouldNotDropImportedCollectionInRollbackRecovery) {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    recipientDoc.setState(ShardMergeRecipientStateEnum::kConsistent);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kCommitted);
    auto updatedDoc = recipientDoc.toBSON();

    const NamespaceString importedDonorCollNss1 =
        NamespaceString::createNamespaceString_forTest(_tenantIds[0].toString() + "_test.coll1");
    ASSERT_OK(createCollection(opCtx(), CreateCommand(importedDonorCollNss1)));

    const NamespaceString importedDonorCollNss2 =
        NamespaceString::createNamespaceString_forTest(_tenantIds[1].toString() + "_test.coll2");
    ASSERT_OK(createCollection(opCtx(), CreateCommand(importedDonorCollNss2)));

    ASSERT(collectionExists(opCtx(), importedDonorCollNss1));
    ASSERT(collectionExists(opCtx(), importedDonorCollNss2));

    // Simulate the node is in rollback repl state.
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_ROLLBACK));
    performUpdates(updatedDoc, preImageDoc);

    ASSERT(collectionExists(opCtx(), importedDonorCollNss1));
    ASSERT(collectionExists(opCtx(), importedDonorCollNss2));
}

TEST_F(
    ShardMergeRecipientOpObserverTest,
    TransitionToAbortedGarbageCollectableShouldDropTempFilesAndMarkerCollectionInRollbackRecovery) {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    recipientDoc.setState(ShardMergeRecipientStateEnum::kLearnedFilenames);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kAborted);
    recipientDoc.setAbortOpTime(OpTime(Timestamp::max(), 1));
    recipientDoc.setExpireAt(opCtx()->getServiceContext()->getFastClockSource()->now());
    auto updatedDoc = recipientDoc.toBSON();

    auto knownIdentListBeforeGC = DurableCatalog::get(opCtx())->getAllIdents(opCtx());

    // Create idents unknown to storage.
    const auto unknownIdent1 = "collection-70--88888";
    const auto unknownIdentPath1 = constructDestinationPath(unknownIdent1);
    boost::filesystem::ofstream unknownIdent1Writer(unknownIdentPath1);
    unknownIdent1Writer << "Dummy stream1 \n";
    unknownIdent1Writer.close();

    const auto unknownIdent2 = "index-71--88888";
    const auto unknownIdentPath2 = constructDestinationPath(unknownIdent2);
    boost::filesystem::ofstream unknownIdent2Writer(unknownIdentPath2);
    unknownIdent2Writer << "Dummy stream2 \n";
    unknownIdent2Writer.close();

    const auto fileClonerTempDirPath = fileClonerTempDir(kMigrationId);
    ASSERT_TRUE(boost::filesystem::create_directory(fileClonerTempDirPath));

    writeMovingFilesMarker(fileClonerTempDirPath, unknownIdent1, true);
    writeMovingFilesMarker(fileClonerTempDirPath, unknownIdent2, false);
    // GC shouldn't remove these known idents.
    for (const auto& ident : knownIdentListBeforeGC) {
        writeMovingFilesMarker(fileClonerTempDirPath, ident, false);
    }

    // Create the marker collection.
    createImportDoneMarkerLocalCollection(opCtx(), kMigrationId);

    // Verify that temp files and the marker collection exist before GC.
    ASSERT(collectionExists(opCtx(), getImportDoneMarkerNs(kMigrationId)));
    ASSERT(boost::filesystem::exists(unknownIdentPath1));
    ASSERT(boost::filesystem::exists(unknownIdentPath2));
    ASSERT(boost::filesystem::exists(fileClonerTempDirPath));

    startCapturingLogMessages();

    // Simulate the node is in rollback repl state.
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_ROLLBACK));
    performUpdates(updatedDoc, preImageDoc);

    stopCapturingLogMessages();

    // Verify that temp files and the marker collection are deleted after GC.
    ASSERT(!collectionExists(opCtx(), getImportDoneMarkerNs(kMigrationId)));

    ASSERT(!boost::filesystem::exists(unknownIdentPath1));
    ASSERT(!boost::filesystem::exists(unknownIdentPath2));
    ASSERT(!boost::filesystem::exists(fileClonerTempDirPath));

    ASSERT_EQUALS(2, countLogLinesWithId(7458501));
    ASSERT_EQUALS(1, countLogLinesWithId(7458503));

    // Verify that GC didn't remove any known idents.
    const auto knownIdentListAfterGC = DurableCatalog::get(opCtx())->getAllIdents(opCtx());
    ASSERT(knownIdentListBeforeGC == knownIdentListAfterGC);
}

TEST_F(
    ShardMergeRecipientOpObserverTest,
    TransitionToCommittedGarbageCollectableShouldDropTempFilesAndMarkerCollectionInRollbackRecovery) {
    ShardMergeRecipientDocument recipientDoc(kMigrationId,
                                             kDefaultDonorConnStr,
                                             _tenantIds,
                                             kDefaultStartMigrationTimestamp,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    recipientDoc.setState(ShardMergeRecipientStateEnum::kConsistent);
    auto preImageDoc = recipientDoc.toBSON();

    recipientDoc.setState(ShardMergeRecipientStateEnum::kCommitted);
    recipientDoc.setExpireAt(opCtx()->getServiceContext()->getFastClockSource()->now());
    auto updatedDoc = recipientDoc.toBSON();

    auto knownIdentListBeforeGC = DurableCatalog::get(opCtx())->getAllIdents(opCtx());

    // Create idents unknown to storage.
    const auto unknownIdent1 = "collection-70--88888";
    const auto unknownIdentPath1 = constructDestinationPath(unknownIdent1);
    boost::filesystem::ofstream unknownIdent1Writer(unknownIdentPath1);
    unknownIdent1Writer << "Dummy stream1 \n";
    unknownIdent1Writer.close();

    const auto unknownIdent2 = "index-71--88888";
    const auto unknownIdentPath2 = constructDestinationPath(unknownIdent2);
    boost::filesystem::ofstream unknownIdent2Writer(unknownIdentPath2);
    unknownIdent2Writer << "Dummy stream2 \n";
    unknownIdent2Writer.close();

    const auto fileClonerTempDirPath = fileClonerTempDir(kMigrationId);
    ASSERT_TRUE(boost::filesystem::create_directory(fileClonerTempDirPath));

    writeMovingFilesMarker(fileClonerTempDirPath, unknownIdent1, true);
    writeMovingFilesMarker(fileClonerTempDirPath, unknownIdent2, false);
    // GC shouldn't remove these known idents.
    for (const auto& ident : knownIdentListBeforeGC) {
        writeMovingFilesMarker(fileClonerTempDirPath, ident, false);
    }

    // Create the marker collection.
    createImportDoneMarkerLocalCollection(opCtx(), kMigrationId);

    // Verify that temp files and the marker collection exist before GC.
    ASSERT(collectionExists(opCtx(), getImportDoneMarkerNs(kMigrationId)));
    ASSERT(boost::filesystem::exists(unknownIdentPath1));
    ASSERT(boost::filesystem::exists(unknownIdentPath2));
    ASSERT(boost::filesystem::exists(fileClonerTempDirPath));

    startCapturingLogMessages();

    // Simulate the node is in rollback repl state.
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_ROLLBACK));
    performUpdates(updatedDoc, preImageDoc);

    stopCapturingLogMessages();

    // Verify that temp files and the marker collection are deleted after GC.
    ASSERT(!collectionExists(opCtx(), getImportDoneMarkerNs(kMigrationId)));

    ASSERT(!boost::filesystem::exists(unknownIdentPath1));
    ASSERT(!boost::filesystem::exists(unknownIdentPath2));
    ASSERT(!boost::filesystem::exists(fileClonerTempDirPath));

    ASSERT_EQUALS(2, countLogLinesWithId(7458501));
    ASSERT_EQUALS(1, countLogLinesWithId(7458503));

    // Verify that GC didn't remove any known idents.
    const auto knownIdentListAfterGC = DurableCatalog::get(opCtx())->getAllIdents(opCtx());
    ASSERT(knownIdentListBeforeGC == knownIdentListAfterGC);
}
}  // namespace mongo::repl
