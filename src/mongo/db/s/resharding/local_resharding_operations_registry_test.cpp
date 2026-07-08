/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/local_resharding_operations_registry.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/resharding/resharding_replica_set_aware_service.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using Role = LocalReshardingOperationsRegistry::Role;

const NamespaceString kNs1 = NamespaceString::createNamespaceString_forTest("db.coll1");
const NamespaceString kNs2 = NamespaceString::createNamespaceString_forTest("db.coll2");
const BSONObj kShardKeyPattern = BSON("x" << 1);

NamespaceString makeTempReshardingNss(const NamespaceString& sourceNss, const UUID& sourceUUID) {
    auto tempColl = fmt::format(
        "{}{}", NamespaceString::kTemporaryReshardingCollectionPrefix, sourceUUID.toString());
    return NamespaceString::createNamespaceString_forTest(sourceNss.db_forSharding(), tempColl);
}

CommonReshardingMetadata makeMetadata(const NamespaceString& sourceNss,
                                      const UUID& reshardingUUID = UUID::gen(),
                                      const UUID& sourceUUID = UUID::gen()) {
    auto tempNss = makeTempReshardingNss(sourceNss, sourceUUID);
    return CommonReshardingMetadata(
        reshardingUUID, sourceNss, sourceUUID, std::move(tempNss), kShardKeyPattern);
}

BSONObj makeCoordinatorStateDoc(const NamespaceString& sourceNss,
                                const UUID& reshardingUUID,
                                const UUID& sourceUUID,
                                CoordinatorStateEnum state = CoordinatorStateEnum::kInitializing) {
    ReshardingCoordinatorDocument doc(state, {}, {});
    doc.setCommonReshardingMetadata(makeMetadata(sourceNss, reshardingUUID, sourceUUID));
    return doc.toBSON();
}

BSONObj makeDonorStateDoc(const NamespaceString& sourceNss,
                          const UUID& reshardingUUID,
                          const UUID& sourceUUID) {
    ReshardingDonorDocument doc;
    doc.setCommonReshardingMetadata(makeMetadata(sourceNss, reshardingUUID, sourceUUID));
    doc.setMutableState(DonorShardContext{});
    doc.setRecipientShards({});
    return doc.toBSON();
}

BSONObj makeRecipientStateDoc(const NamespaceString& sourceNss,
                              const UUID& reshardingUUID,
                              const UUID& sourceUUID) {
    ReshardingRecipientDocument doc;
    doc.setCommonReshardingMetadata(makeMetadata(sourceNss, reshardingUUID, sourceUUID));
    doc.setMutableState(RecipientShardContext{});
    doc.setDonorShards({});
    doc.setMinimumOperationDurationMillis(0);
    return doc.toBSON();
}

void createCollectionAndInsert(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const std::vector<BSONObj>& docs) {
    DBDirectClient client(opCtx);
    ASSERT_TRUE(client.createCollection(nss));
    for (const auto& doc : docs) {
        client.insert(nss, doc);
    }
}

class LocalReshardingOperationsRegistryResyncFromDiskTest : public MockReplCoordServerFixture {};

TEST_F(LocalReshardingOperationsRegistryResyncFromDiskTest, EmptyCollectionsRegistryRemainsEmpty) {
    createCollectionAndInsert(opCtx(), NamespaceString::kConfigReshardingOperationsNamespace, {});
    createCollectionAndInsert(opCtx(), NamespaceString::kDonorReshardingOperationsNamespace, {});
    createCollectionAndInsert(
        opCtx(), NamespaceString::kRecipientReshardingOperationsNamespace, {});

    LocalReshardingOperationsRegistry registry;
    registry.resyncFromDisk(opCtx(), "test resync");

    ASSERT_FALSE(registry.getOperation(kNs1));
    ASSERT_FALSE(registry.getOperation(kNs2));
}

TEST_F(LocalReshardingOperationsRegistryResyncFromDiskTest,
       CoordinatorDocOnlyRegistryHasCoordinatorRole) {
    auto reshardingUUID = UUID::gen();
    auto sourceUUID = UUID::gen();
    createCollectionAndInsert(opCtx(),
                              NamespaceString::kConfigReshardingOperationsNamespace,
                              {makeCoordinatorStateDoc(kNs1, reshardingUUID, sourceUUID)});
    createCollectionAndInsert(opCtx(), NamespaceString::kDonorReshardingOperationsNamespace, {});
    createCollectionAndInsert(
        opCtx(), NamespaceString::kRecipientReshardingOperationsNamespace, {});

    LocalReshardingOperationsRegistry registry;
    registry.resyncFromDisk(opCtx(), "test resync");

    auto op = registry.getOperation(kNs1);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->metadata.getSourceNss(), kNs1);
    ASSERT_EQ(op->metadata.getReshardingUUID(), reshardingUUID);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kCoordinator));
}

TEST_F(LocalReshardingOperationsRegistryResyncFromDiskTest, DonorDocOnlyRegistryHasDonorRole) {
    auto reshardingUUID = UUID::gen();
    auto sourceUUID = UUID::gen();
    createCollectionAndInsert(opCtx(), NamespaceString::kConfigReshardingOperationsNamespace, {});
    createCollectionAndInsert(opCtx(),
                              NamespaceString::kDonorReshardingOperationsNamespace,
                              {makeDonorStateDoc(kNs1, reshardingUUID, sourceUUID)});
    createCollectionAndInsert(
        opCtx(), NamespaceString::kRecipientReshardingOperationsNamespace, {});

    LocalReshardingOperationsRegistry registry;
    registry.resyncFromDisk(opCtx(), "test resync");

    auto op = registry.getOperation(kNs1);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->metadata.getSourceNss(), kNs1);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kDonor));
}

TEST_F(LocalReshardingOperationsRegistryResyncFromDiskTest,
       RecipientDocOnlyRegistryHasRecipientRole) {
    auto reshardingUUID = UUID::gen();
    auto sourceUUID = UUID::gen();
    createCollectionAndInsert(opCtx(), NamespaceString::kConfigReshardingOperationsNamespace, {});
    createCollectionAndInsert(opCtx(), NamespaceString::kDonorReshardingOperationsNamespace, {});
    createCollectionAndInsert(opCtx(),
                              NamespaceString::kRecipientReshardingOperationsNamespace,
                              {makeRecipientStateDoc(kNs1, reshardingUUID, sourceUUID)});

    LocalReshardingOperationsRegistry registry;
    registry.resyncFromDisk(opCtx(), "test resync");

    auto op = registry.getOperation(kNs1);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->metadata.getSourceNss(), kNs1);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kRecipient));
}

TEST_F(LocalReshardingOperationsRegistryResyncFromDiskTest,
       MultipleRolesSameNssRegistryHasAllRoles) {
    auto reshardingUUID = UUID::gen();
    auto sourceUUID = UUID::gen();
    createCollectionAndInsert(opCtx(),
                              NamespaceString::kConfigReshardingOperationsNamespace,
                              {makeCoordinatorStateDoc(kNs1, reshardingUUID, sourceUUID)});
    createCollectionAndInsert(opCtx(),
                              NamespaceString::kDonorReshardingOperationsNamespace,
                              {makeDonorStateDoc(kNs1, reshardingUUID, sourceUUID)});
    createCollectionAndInsert(opCtx(),
                              NamespaceString::kRecipientReshardingOperationsNamespace,
                              {makeRecipientStateDoc(kNs1, reshardingUUID, sourceUUID)});

    LocalReshardingOperationsRegistry registry;
    registry.resyncFromDisk(opCtx(), "test resync");

    auto op = registry.getOperation(kNs1);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->metadata.getSourceNss(), kNs1);
    ASSERT_TRUE(op->roles.count(Role::kCoordinator));
    ASSERT_TRUE(op->roles.count(Role::kDonor));
    ASSERT_TRUE(op->roles.count(Role::kRecipient));
}

TEST_F(LocalReshardingOperationsRegistryResyncFromDiskTest,
       QuiescedCoordinatorDocOnlyRegistryRemainsEmpty) {
    auto reshardingUUID = UUID::gen();
    auto sourceUUID = UUID::gen();
    createCollectionAndInsert(
        opCtx(),
        NamespaceString::kConfigReshardingOperationsNamespace,
        {makeCoordinatorStateDoc(
            kNs1, reshardingUUID, sourceUUID, CoordinatorStateEnum::kQuiesced)});
    createCollectionAndInsert(opCtx(), NamespaceString::kDonorReshardingOperationsNamespace, {});
    createCollectionAndInsert(
        opCtx(), NamespaceString::kRecipientReshardingOperationsNamespace, {});

    LocalReshardingOperationsRegistry registry;
    registry.resyncFromDisk(opCtx(), "test resync");

    ASSERT_FALSE(registry.getOperation(kNs1));
}

TEST_F(LocalReshardingOperationsRegistryResyncFromDiskTest, MultipleOperationsRegistryHasAll) {
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    createCollectionAndInsert(opCtx(),
                              NamespaceString::kConfigReshardingOperationsNamespace,
                              {makeCoordinatorStateDoc(kNs1, uuid1, UUID::gen())});
    createCollectionAndInsert(opCtx(),
                              NamespaceString::kDonorReshardingOperationsNamespace,
                              {makeDonorStateDoc(kNs2, uuid2, UUID::gen())});

    LocalReshardingOperationsRegistry registry;
    registry.resyncFromDisk(opCtx(), "test resync");

    auto op1 = registry.getOperation(kNs1);
    auto op2 = registry.getOperation(kNs2);
    ASSERT_TRUE(op1);
    ASSERT_TRUE(op2);
    ASSERT_EQ(op1->metadata.getSourceNss(), kNs1);
    ASSERT_EQ(op2->metadata.getSourceNss(), kNs2);
    ASSERT_TRUE(op1->roles.count(Role::kCoordinator));
    ASSERT_TRUE(op2->roles.count(Role::kDonor));
}

TEST_F(LocalReshardingOperationsRegistryResyncFromDiskTest, ClearsExistingStateBeforeRepopulating) {
    auto reshardingUUID = UUID::gen();
    auto sourceUUID = UUID::gen();
    createCollectionAndInsert(opCtx(),
                              NamespaceString::kConfigReshardingOperationsNamespace,
                              {makeCoordinatorStateDoc(kNs1, reshardingUUID, sourceUUID)});
    createCollectionAndInsert(opCtx(), NamespaceString::kDonorReshardingOperationsNamespace, {});
    createCollectionAndInsert(
        opCtx(), NamespaceString::kRecipientReshardingOperationsNamespace, {});

    LocalReshardingOperationsRegistry registry;
    registry.registerOperation(Role::kDonor, makeMetadata(kNs2));
    ASSERT_TRUE(registry.getOperation(kNs2));

    registry.resyncFromDisk(opCtx(), "test resync");

    ASSERT_TRUE(registry.getOperation(kNs1));
    ASSERT_FALSE(registry.getOperation(kNs2));
}

TEST_F(LocalReshardingOperationsRegistryResyncFromDiskTest, ReportForServerStatusCountsResyncs) {
    createCollectionAndInsert(opCtx(), NamespaceString::kConfigReshardingOperationsNamespace, {});
    createCollectionAndInsert(opCtx(), NamespaceString::kDonorReshardingOperationsNamespace, {});
    createCollectionAndInsert(
        opCtx(), NamespaceString::kRecipientReshardingOperationsNamespace, {});

    LocalReshardingOperationsRegistry registry;
    registry.resyncFromDisk(opCtx(), "test resync");
    registry.resyncFromDisk(opCtx(), "test resync");

    BSONObjBuilder bob;
    registry.reportForServerStatus(&bob);
    auto bobObj = bob.obj();
    auto section = bobObj.getObjectField("reshardingOperationsRegistry");
    ASSERT_EQ(section.getField("resyncs").numberLong(), 2);
    ASSERT_EQ(section.getField("currentOperations").numberLong(), 0);
}

TEST_F(LocalReshardingOperationsRegistryResyncFromDiskTest,
       CurrentOperationsReflectsOnDiskStateWhileRegistrationCountersDriftAcrossRollbackAndRetry) {
    auto reshardingUUID = UUID::gen();
    auto sourceUUID = UUID::gen();
    createCollectionAndInsert(opCtx(), NamespaceString::kConfigReshardingOperationsNamespace, {});
    createCollectionAndInsert(opCtx(), NamespaceString::kDonorReshardingOperationsNamespace, {});
    createCollectionAndInsert(
        opCtx(), NamespaceString::kRecipientReshardingOperationsNamespace, {});

    LocalReshardingOperationsRegistry registry;
    auto meta = makeMetadata(kNs1, reshardingUUID, sourceUUID);

    auto section = [&] {
        BSONObjBuilder bob;
        registry.reportForServerStatus(&bob);
        return bob.obj().getObjectField("reshardingOperationsRegistry").getOwned();
    };

    // On startup the registry resyncs from an empty disk, so nothing is tracked.
    registry.resyncFromDisk(opCtx(), "startup");
    ASSERT_EQ(section().getField("resyncs").numberLong(), 1);
    ASSERT_EQ(section().getField("currentOperations").numberLong(), 0);

    // On stepup the operation is registered in memory and its state document is persisted to disk.
    registry.registerOperation(Role::kDonor, meta);
    DBDirectClient(opCtx()).insert(NamespaceString::kDonorReshardingOperationsNamespace,
                                   makeDonorStateDoc(kNs1, reshardingUUID, sourceUUID));
    ASSERT_EQ(section().getField("registrations").numberLong(), 1);
    ASSERT_EQ(section().getField("currentOperations").numberLong(), 1);

    // A rollback triggers another resync. The operation is still on disk, so the gauge is correctly
    // repopulated to 1 even though the resync does not touch the monotonic registration counter.
    registry.resyncFromDisk(opCtx(), "rollback");
    ASSERT_EQ(section().getField("resyncs").numberLong(), 2);
    ASSERT_EQ(section().getField("registrations").numberLong(), 1);
    ASSERT_EQ(section().getField("currentOperations").numberLong(), 1);

    // The operation is retried and re-registers, which double-counts the monotonic registrations
    // counter but is idempotent for the gauge.
    registry.registerOperation(Role::kDonor, meta);
    ASSERT_EQ(section().getField("registrations").numberLong(), 2);
    ASSERT_EQ(section().getField("currentOperations").numberLong(), 1);

    // When the operation finally completes it unregisters once.
    registry.unregisterOperation(Role::kDonor, meta);
    auto finalSection = section();
    ASSERT_EQ(finalSection.getField("registrations").numberLong(), 2);
    ASSERT_EQ(finalSection.getField("unregistrations").numberLong(), 1);
    ASSERT_EQ(finalSection.getField("resyncs").numberLong(), 2);
    ASSERT_EQ(finalSection.getField("currentOperations").numberLong(), 0);
}

class ReshardingReplicaSetAwareServiceTest : public MockReplCoordServerFixture {};

TEST_F(ReshardingReplicaSetAwareServiceTest,
       OnConsistentDataAvailableResyncsGlobalRegistryFromDisk) {
    auto reshardingUUID = UUID::gen();
    auto sourceUUID = UUID::gen();
    createCollectionAndInsert(opCtx(),
                              NamespaceString::kDonorReshardingOperationsNamespace,
                              {makeDonorStateDoc(kNs1, reshardingUUID, sourceUUID)});

    // The global registry does not reflect the on-disk operation until data is made consistent.
    ASSERT_FALSE(LocalReshardingOperationsRegistry::get().getOperation(kNs1));

    ReshardingReplicaSetAwareService::get(getServiceContext())
        ->onConsistentDataAvailable(opCtx(), /*isMajority=*/true, /*isRollback=*/false);

    auto op = LocalReshardingOperationsRegistry::get().getOperation(kNs1);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->metadata.getReshardingUUID(), reshardingUUID);
    ASSERT_TRUE(op->roles.count(Role::kDonor));
}

TEST_F(ReshardingReplicaSetAwareServiceTest,
       OnConsistentDataAvailableIsNoOpWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagReshardingRegistry", false};

    createCollectionAndInsert(opCtx(),
                              NamespaceString::kDonorReshardingOperationsNamespace,
                              {makeDonorStateDoc(kNs1, UUID::gen(), UUID::gen())});

    ReshardingReplicaSetAwareService::get(getServiceContext())
        ->onConsistentDataAvailable(opCtx(), /*isMajority=*/true, /*isRollback=*/false);

    ASSERT_FALSE(LocalReshardingOperationsRegistry::get().getOperation(kNs1));
}

TEST(LocalReshardingOperationsRegistryTest, GetUnknownNamespace) {
    LocalReshardingOperationsRegistry registry;
    ASSERT_FALSE(registry.getOperation(kNs1));
}

TEST(LocalReshardingOperationsRegistryTest, RegisterOneRole) {
    LocalReshardingOperationsRegistry registry;
    auto meta = makeMetadata(kNs1);
    registry.registerOperation(Role::kCoordinator, meta);

    auto op = registry.getOperation(kNs1);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kCoordinator));
    ASSERT_EQ(op->metadata.getSourceNss(), kNs1);
}

TEST(LocalReshardingOperationsRegistryTest, RegisterMultipleRolesSameNamespace) {
    LocalReshardingOperationsRegistry registry;
    auto meta = makeMetadata(kNs1);
    registry.registerOperation(Role::kCoordinator, meta);
    registry.registerOperation(Role::kDonor, meta);
    registry.registerOperation(Role::kRecipient, meta);

    auto op = registry.getOperation(kNs1);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 3u);
    ASSERT_TRUE(op->roles.count(Role::kCoordinator));
    ASSERT_TRUE(op->roles.count(Role::kDonor));
    ASSERT_TRUE(op->roles.count(Role::kRecipient));
    ASSERT_EQ(op->metadata.getSourceNss(), kNs1);
}

TEST(LocalReshardingOperationsRegistryTest, RegisterMultipleNamespaces) {
    LocalReshardingOperationsRegistry registry;
    auto meta1 = makeMetadata(kNs1);
    auto meta2 = makeMetadata(kNs2);
    registry.registerOperation(Role::kCoordinator, meta1);
    registry.registerOperation(Role::kDonor, meta2);

    auto op1 = registry.getOperation(kNs1);
    auto op2 = registry.getOperation(kNs2);
    ASSERT_TRUE(op1);
    ASSERT_TRUE(op2);
    ASSERT_EQ(op1->roles.size(), 1u);
    ASSERT_EQ(op2->roles.size(), 1u);
    ASSERT_EQ(op1->metadata.getSourceNss(), kNs1);
    ASSERT_EQ(op2->metadata.getSourceNss(), kNs2);
}

TEST(LocalReshardingOperationsRegistryTest, UnregisterOnlyRoleRemovesEntry) {
    LocalReshardingOperationsRegistry registry;
    auto meta = makeMetadata(kNs1);
    registry.registerOperation(Role::kCoordinator, meta);
    registry.unregisterOperation(Role::kCoordinator, meta);

    ASSERT_FALSE(registry.getOperation(kNs1));
}

TEST(LocalReshardingOperationsRegistryTest, UnregisterOneOfMultipleRoles) {
    LocalReshardingOperationsRegistry registry;
    auto meta = makeMetadata(kNs1);
    registry.registerOperation(Role::kCoordinator, meta);
    registry.registerOperation(Role::kDonor, meta);
    registry.unregisterOperation(Role::kCoordinator, meta);

    auto op = registry.getOperation(kNs1);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kDonor));
}

TEST(LocalReshardingOperationsRegistryTest, UnregisterLastRoleRemovesEntry) {
    LocalReshardingOperationsRegistry registry;
    auto meta = makeMetadata(kNs1);
    registry.registerOperation(Role::kCoordinator, meta);
    registry.registerOperation(Role::kDonor, meta);
    registry.unregisterOperation(Role::kCoordinator, meta);
    registry.unregisterOperation(Role::kDonor, meta);

    ASSERT_FALSE(registry.getOperation(kNs1));
}

TEST(LocalReshardingOperationsRegistryTest, UnregisterNonExistentRoleNamespaceIsNoOp) {
    LocalReshardingOperationsRegistry registry;
    auto meta = makeMetadata(kNs1);
    registry.unregisterOperation(Role::kCoordinator, meta);
    ASSERT_FALSE(registry.getOperation(kNs1));

    registry.registerOperation(Role::kCoordinator, meta);
    registry.unregisterOperation(Role::kDonor, meta);

    auto op = registry.getOperation(kNs1);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kCoordinator));
}

TEST(LocalReshardingOperationsRegistryTest, UnregisterWithNonMatchingMetadataIsNoOp) {
    LocalReshardingOperationsRegistry registry;
    auto meta1 = makeMetadata(kNs1, UUID::gen(), UUID::gen());
    auto meta2 = makeMetadata(kNs1, UUID::gen(), UUID::gen());
    registry.registerOperation(Role::kCoordinator, meta1);
    registry.unregisterOperation(Role::kCoordinator, meta2);

    auto op = registry.getOperation(kNs1);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kCoordinator));
}

TEST(LocalReshardingOperationsRegistryTest, RegisterDuplicateRoleNamespaceSameMetadataIsNoOp) {
    LocalReshardingOperationsRegistry registry;
    auto meta = makeMetadata(kNs1);
    registry.registerOperation(Role::kCoordinator, meta);
    registry.registerOperation(Role::kCoordinator, meta);

    auto op = registry.getOperation(kNs1);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kCoordinator));
    ASSERT_EQ(op->metadata.getSourceNss(), kNs1);
}

TEST(LocalReshardingOperationsRegistryTest,
     ReportForServerStatusCountsRegistrationsAndUnregistrations) {
    LocalReshardingOperationsRegistry registry;
    auto meta = makeMetadata(kNs1);

    {
        BSONObjBuilder bob;
        registry.reportForServerStatus(&bob);
        auto bobObj = bob.obj();
        auto section = bobObj.getObjectField("reshardingOperationsRegistry");
        ASSERT_EQ(section.getField("registrations").numberLong(), 0);
        ASSERT_EQ(section.getField("unregistrations").numberLong(), 0);
        ASSERT_EQ(section.getField("currentOperations").numberLong(), 0);
    }

    registry.registerOperation(Role::kCoordinator, meta);
    registry.registerOperation(Role::kDonor, meta);
    registry.unregisterOperation(Role::kCoordinator, meta);

    {
        BSONObjBuilder bob;
        registry.reportForServerStatus(&bob);
        auto bobObj = bob.obj();
        auto section = bobObj.getObjectField("reshardingOperationsRegistry");
        ASSERT_EQ(section.getField("registrations").numberLong(), 2);
        ASSERT_EQ(section.getField("unregistrations").numberLong(), 1);
        // The coordinator role was unregistered but the donor role remains, so the operation is
        // still tracked and the gauge reflects one live operation.
        ASSERT_EQ(section.getField("currentOperations").numberLong(), 1);
    }
}

TEST(LocalReshardingOperationsRegistryTest, ReportForServerStatusCountsCurrentOperations) {
    LocalReshardingOperationsRegistry registry;
    auto meta1 = makeMetadata(kNs1);
    auto meta2 = makeMetadata(kNs2);

    registry.registerOperation(Role::kCoordinator, meta1);
    registry.registerOperation(Role::kDonor, meta1);
    registry.registerOperation(Role::kDonor, meta2);

    auto currentOperations = [&] {
        BSONObjBuilder bob;
        registry.reportForServerStatus(&bob);
        return bob.obj()
            .getObjectField("reshardingOperationsRegistry")
            .getField("currentOperations")
            .numberLong();
    };

    // Two distinct namespaces are tracked; multiple roles on the same operation count once.
    ASSERT_EQ(currentOperations(), 2);

    // Removing one of meta1's two roles leaves the operation tracked, so the gauge is unchanged.
    registry.unregisterOperation(Role::kCoordinator, meta1);
    ASSERT_EQ(currentOperations(), 2);
    // Removing meta1's last role drops the operation.
    registry.unregisterOperation(Role::kDonor, meta1);
    ASSERT_EQ(currentOperations(), 1);

    registry.unregisterOperation(Role::kDonor, meta2);
    ASSERT_EQ(currentOperations(), 0);
}

TEST(LocalReshardingOperationsRegistryTest, ClearOperationsForRoleUpdatesCurrentOperations) {
    LocalReshardingOperationsRegistry registry;
    auto meta1 = makeMetadata(kNs1);
    auto meta2 = makeMetadata(kNs2);

    // kNs1 holds two roles; kNs2 holds only the donor role.
    registry.registerOperation(Role::kCoordinator, meta1);
    registry.registerOperation(Role::kDonor, meta1);
    registry.registerOperation(Role::kDonor, meta2);

    auto currentOperations = [&] {
        BSONObjBuilder bob;
        registry.reportForServerStatus(&bob);
        return bob.obj()
            .getObjectField("reshardingOperationsRegistry")
            .getField("currentOperations")
            .numberLong();
    };

    ASSERT_EQ(currentOperations(), 2);

    // Clearing the donor role drops kNs2 entirely (its only role) but leaves kNs1 tracked because
    // it still holds the coordinator role, so the gauge only decreases by one.
    registry.clearOperationsForRole(Role::kDonor);
    ASSERT_EQ(currentOperations(), 1);
    auto op = registry.getOperation(kNs1);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kCoordinator));
    ASSERT_FALSE(registry.getOperation(kNs2));

    // Clearing the coordinator role removes kNs1's last role, so the gauge reaches zero.
    registry.clearOperationsForRole(Role::kCoordinator);
    ASSERT_EQ(currentOperations(), 0);
    ASSERT_FALSE(registry.getOperation(kNs1));

    // Clearing a role with nothing left to remove is a no-op.
    registry.clearOperationsForRole(Role::kRecipient);
    ASSERT_EQ(currentOperations(), 0);
}

TEST(LocalReshardingOperationsRegistryTest,
     GetOperationWithMultipleOpsForSameNssThrowsTransientInconsistency) {
    LocalReshardingOperationsRegistry registry;
    auto meta1 = makeMetadata(kNs1, UUID::gen(), UUID::gen());
    auto meta2 = makeMetadata(kNs1, UUID::gen(), UUID::gen());
    registry.registerOperation(Role::kCoordinator, meta1);
    registry.registerOperation(Role::kCoordinator, meta2);

    ASSERT_THROWS_CODE(registry.getOperation(kNs1), DBException, ErrorCodes::PrimarySteppedDown);
}

class ThrowIfReshardingInProgressTest : public ServiceContextTest {};

TEST_F(ThrowIfReshardingInProgressTest, DoesNotThrowWhenNoOperationRegistered) {
    ASSERT_DOES_NOT_THROW(resharding::throwIfReshardingInProgress(kNs1));
}

TEST_F(ThrowIfReshardingInProgressTest, ThrowsWhenOperationIsRegistered) {
    auto& registry = LocalReshardingOperationsRegistry::get();
    registry.registerOperation(Role::kCoordinator, makeMetadata(kNs1));

    ASSERT_THROWS_CODE(resharding::throwIfReshardingInProgress(kNs1),
                       DBException,
                       ErrorCodes::ReshardCollectionInProgress);
}

TEST_F(ThrowIfReshardingInProgressTest, DoesNotThrowAfterOperationUnregistered) {
    auto& registry = LocalReshardingOperationsRegistry::get();
    auto meta = makeMetadata(kNs1);
    registry.registerOperation(Role::kCoordinator, meta);
    registry.unregisterOperation(Role::kCoordinator, meta);

    ASSERT_DOES_NOT_THROW(resharding::throwIfReshardingInProgress(kNs1));
}

TEST_F(ThrowIfReshardingInProgressTest, DoesNotThrowForDifferentNamespace) {
    auto& registry = LocalReshardingOperationsRegistry::get();
    registry.registerOperation(Role::kCoordinator, makeMetadata(kNs1));

    ASSERT_DOES_NOT_THROW(resharding::throwIfReshardingInProgress(kNs2));
}

}  // namespace

}  // namespace mongo
