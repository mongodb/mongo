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

#include "mongo/db/s/resharding/resharding_op_observer.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/local_resharding_operations_registry.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_mock.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using Role = LocalReshardingOperationsRegistry::Role;

const NamespaceString kSourceNss =
    NamespaceString::createNamespaceString_forTest("db", "reshardingSourceColl");
const BSONObj kShardKeyPattern = BSON("x" << 1);

CommonReshardingMetadata makeMetadata(const NamespaceString& sourceNss = kSourceNss,
                                      const UUID& reshardingUUID = UUID::gen(),
                                      const UUID& sourceUUID = UUID::gen()) {
    auto tempColl = fmt::format(
        "{}{}", NamespaceString::kTemporaryReshardingCollectionPrefix, sourceUUID.toString());
    auto tempNss =
        NamespaceString::createNamespaceString_forTest(sourceNss.db_forSharding(), tempColl);
    return CommonReshardingMetadata(
        reshardingUUID, sourceNss, sourceUUID, std::move(tempNss), kShardKeyPattern);
}

BSONObj makeCoordinatorDocBson(const CommonReshardingMetadata& metadata,
                               CoordinatorStateEnum state = CoordinatorStateEnum::kInitializing) {
    ReshardingCoordinatorDocument doc(state, {}, {});
    doc.setCommonReshardingMetadata(metadata);
    return doc.toBSON();
}

BSONObj makeDonorDocBson(const CommonReshardingMetadata& metadata) {
    DonorShardContext donorCtx;
    donorCtx.setState(DonorStateEnum::kPreparingToDonate);
    ReshardingDonorDocument doc(std::move(donorCtx), {ShardId{"recipient1"}});
    doc.setCommonReshardingMetadata(metadata);
    return doc.toBSON();
}

BSONObj makeRecipientDocBson(const CommonReshardingMetadata& metadata) {
    RecipientShardContext recipientCtx;
    recipientCtx.setState(RecipientStateEnum::kAwaitingFetchTimestamp);
    ReshardingRecipientDocument doc(std::move(recipientCtx));
    doc.setDonorShards({DonorShardFetchTimestamp(ShardId{"donor1"})});
    doc.setMinimumOperationDurationMillis(5);
    doc.setCommonReshardingMetadata(metadata);
    return doc.toBSON();
}

std::pair<std::shared_ptr<Collection>, CollectionPtr> makeCollPtr(const NamespaceString& nss) {
    std::shared_ptr<Collection> mock = std::make_shared<CollectionMock>(nss);
    CollectionPtr ptr(mock.get());
    return {std::move(mock), std::move(ptr)};
}

class ReshardingOpObserverRegistryTest : public ServiceContextMongoDTest {
protected:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto service = getServiceContext();

        repl::ReplicationCoordinator::set(
            service, std::make_unique<repl::ReplicationCoordinatorMock>(service));
    }

    LocalReshardingOperationsRegistry& registry() {
        return LocalReshardingOperationsRegistry::get();
    }

    void doInsert(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& doc) {
        auto [_, collPtr] = makeCollPtr(nss);
        std::vector<InsertStatement> stmts{InsertStatement(doc)};
        _opObserver.onInserts(opCtx,
                              collPtr,
                              stmts.cbegin(),
                              stmts.cend(),
                              /*recordIds=*/{},
                              /*fromMigrate=*/{false},
                              /*defaultFromMigrate=*/false);
    }

    void doUpdate(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& updatedDoc) {
        auto [_, collPtr] = makeCollPtr(nss);
        CollectionUpdateArgs updateArgs(BSONObj{} /* preImageDoc */);
        updateArgs.updatedDoc = updatedDoc;
        OplogUpdateEntryArgs update(&updateArgs, collPtr);
        // We add a WUOW here to transition recovery unit state from Inactive to InUnitOfWork. The
        // registry changes we test are synchronous and unaffected by the WUOW abort.
        WriteUnitOfWork wuow(opCtx);
        _opObserver.onUpdate(opCtx, update);
    }

    void doDelete(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& doc) {
        auto [_, collPtr] = makeCollPtr(nss);
        DocumentKey docKey(doc["_id"].wrap(), boost::none);
        OplogDeleteEntryArgs deleteArgs;
        _opObserver.onDelete(opCtx, collPtr, /*stmtId=*/0, doc, docKey, deleteArgs);
    }

    ReshardingOpObserver _opObserver;
};

TEST_F(ReshardingOpObserverRegistryTest, InsertCoordinatorDocRegistersCoordinatorRole) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();
    auto doc = makeCoordinatorDocBson(metadata);

    doInsert(opCtx.get(), NamespaceString::kConfigReshardingOperationsNamespace, doc);

    auto op = registry().getOperation(kSourceNss);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kCoordinator));
    ASSERT_EQ(op->metadata.getReshardingUUID(), metadata.getReshardingUUID());
    ASSERT_EQ(op->metadata.getSourceNss(), kSourceNss);
}

TEST_F(ReshardingOpObserverRegistryTest, InsertDonorDocRegistersDonorRole) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();
    auto doc = makeDonorDocBson(metadata);

    doInsert(opCtx.get(), NamespaceString::kDonorReshardingOperationsNamespace, doc);

    auto op = registry().getOperation(kSourceNss);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kDonor));
    ASSERT_EQ(op->metadata.getReshardingUUID(), metadata.getReshardingUUID());
}

TEST_F(ReshardingOpObserverRegistryTest, InsertRecipientDocRegistersRecipientRole) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();
    auto doc = makeRecipientDocBson(metadata);

    doInsert(opCtx.get(), NamespaceString::kRecipientReshardingOperationsNamespace, doc);

    auto op = registry().getOperation(kSourceNss);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kRecipient));
    ASSERT_EQ(op->metadata.getReshardingUUID(), metadata.getReshardingUUID());
}

TEST_F(ReshardingOpObserverRegistryTest, InsertUnrelatedCollectionDoesNotRegister) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();
    auto doc = makeCoordinatorDocBson(metadata);

    auto unrelatedNss = NamespaceString::createNamespaceString_forTest("config", "otherCollection");
    doInsert(opCtx.get(), unrelatedNss, doc);

    ASSERT_FALSE(registry().getOperation(kSourceNss));
}

TEST_F(ReshardingOpObserverRegistryTest, InsertMultipleRolesRegistersAll) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();

    doInsert(opCtx.get(),
             NamespaceString::kConfigReshardingOperationsNamespace,
             makeCoordinatorDocBson(metadata));
    doInsert(opCtx.get(),
             NamespaceString::kDonorReshardingOperationsNamespace,
             makeDonorDocBson(metadata));
    doInsert(opCtx.get(),
             NamespaceString::kRecipientReshardingOperationsNamespace,
             makeRecipientDocBson(metadata));

    auto op = registry().getOperation(kSourceNss);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 3u);
    ASSERT_TRUE(op->roles.count(Role::kCoordinator));
    ASSERT_TRUE(op->roles.count(Role::kDonor));
    ASSERT_TRUE(op->roles.count(Role::kRecipient));
}

TEST_F(ReshardingOpObserverRegistryTest, InsertDuplicateIsNoOp) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();
    auto doc = makeCoordinatorDocBson(metadata);

    doInsert(opCtx.get(), NamespaceString::kConfigReshardingOperationsNamespace, doc);
    doInsert(opCtx.get(), NamespaceString::kConfigReshardingOperationsNamespace, doc);

    auto op = registry().getOperation(kSourceNss);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kCoordinator));
}

TEST_F(ReshardingOpObserverRegistryTest, DeleteCoordinatorDocUnregistersCoordinatorRole) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();

    registry().registerOperation(Role::kCoordinator, metadata);
    ASSERT_TRUE(registry().getOperation(kSourceNss));

    doDelete(opCtx.get(),
             NamespaceString::kConfigReshardingOperationsNamespace,
             makeCoordinatorDocBson(metadata));

    ASSERT_FALSE(registry().getOperation(kSourceNss));
}

TEST_F(ReshardingOpObserverRegistryTest, DeleteRecipientDocUnregistersRecipientRole) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();

    registry().registerOperation(Role::kRecipient, metadata);
    ASSERT_TRUE(registry().getOperation(kSourceNss));

    doDelete(opCtx.get(),
             NamespaceString::kRecipientReshardingOperationsNamespace,
             makeRecipientDocBson(metadata));

    ASSERT_FALSE(registry().getOperation(kSourceNss));
}

TEST_F(ReshardingOpObserverRegistryTest, DeleteOneRoleLeavesOtherRoles) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();

    registry().registerOperation(Role::kCoordinator, metadata);
    registry().registerOperation(Role::kDonor, metadata);
    doDelete(opCtx.get(),
             NamespaceString::kConfigReshardingOperationsNamespace,
             makeCoordinatorDocBson(metadata));

    auto op = registry().getOperation(kSourceNss);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kDonor));
}

TEST_F(ReshardingOpObserverRegistryTest, DeleteUnrelatedCollectionDoesNotUnregister) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();

    registry().registerOperation(Role::kCoordinator, metadata);

    auto unrelatedNss = NamespaceString::createNamespaceString_forTest("config", "otherCollection");
    doDelete(opCtx.get(), unrelatedNss, makeCoordinatorDocBson(metadata));

    auto op = registry().getOperation(kSourceNss);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kCoordinator));
}

TEST_F(ReshardingOpObserverRegistryTest, InsertThenDeleteLifecycle) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();
    auto doc = makeCoordinatorDocBson(metadata);

    doInsert(opCtx.get(), NamespaceString::kConfigReshardingOperationsNamespace, doc);
    ASSERT_TRUE(registry().getOperation(kSourceNss));

    doDelete(opCtx.get(), NamespaceString::kConfigReshardingOperationsNamespace, doc);
    ASSERT_FALSE(registry().getOperation(kSourceNss));
}

TEST_F(ReshardingOpObserverRegistryTest, DeleteNonExistentOperationIsNoOp) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();
    auto doc = makeCoordinatorDocBson(metadata);

    doDelete(opCtx.get(), NamespaceString::kConfigReshardingOperationsNamespace, doc);
    ASSERT_FALSE(registry().getOperation(kSourceNss));
}

TEST_F(ReshardingOpObserverRegistryTest, UpdateCoordinatorDocToQuiescedUnregistersCoordinator) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();

    doInsert(opCtx.get(),
             NamespaceString::kConfigReshardingOperationsNamespace,
             makeCoordinatorDocBson(metadata));
    ASSERT_TRUE(registry().getOperation(kSourceNss));

    doUpdate(opCtx.get(),
             NamespaceString::kConfigReshardingOperationsNamespace,
             makeCoordinatorDocBson(metadata, CoordinatorStateEnum::kQuiesced));

    ASSERT_FALSE(registry().getOperation(kSourceNss));
}

TEST_F(ReshardingOpObserverRegistryTest, UpdateCoordinatorDocToNonQuiescedDoesNotUnregister) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();
    auto metadata = makeMetadata();

    doInsert(opCtx.get(),
             NamespaceString::kConfigReshardingOperationsNamespace,
             makeCoordinatorDocBson(metadata));

    doUpdate(opCtx.get(),
             NamespaceString::kConfigReshardingOperationsNamespace,
             makeCoordinatorDocBson(metadata, CoordinatorStateEnum::kCloning));

    auto op = registry().getOperation(kSourceNss);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->roles.size(), 1u);
    ASSERT_TRUE(op->roles.count(Role::kCoordinator));
}

TEST_F(ReshardingOpObserverRegistryTest, QuiescedCoordinatorThenNewOperationLifecycle) {
    RAIIServerParameterControllerForTest featureFlagScope{"featureFlagReshardingRegistry", true};
    auto opCtx = makeOperationContext();

    auto metadata1 = makeMetadata();
    doInsert(opCtx.get(),
             NamespaceString::kConfigReshardingOperationsNamespace,
             makeCoordinatorDocBson(metadata1));
    ASSERT_TRUE(registry().getOperation(kSourceNss));

    doUpdate(opCtx.get(),
             NamespaceString::kConfigReshardingOperationsNamespace,
             makeCoordinatorDocBson(metadata1, CoordinatorStateEnum::kQuiesced));
    ASSERT_FALSE(registry().getOperation(kSourceNss));

    // A new resharding operation for the same namespace with a different UUID should succeed.
    auto metadata2 = makeMetadata();
    ASSERT_NE(metadata1.getReshardingUUID(), metadata2.getReshardingUUID());
    doInsert(opCtx.get(),
             NamespaceString::kConfigReshardingOperationsNamespace,
             makeCoordinatorDocBson(metadata2));

    auto op = registry().getOperation(kSourceNss);
    ASSERT_TRUE(op);
    ASSERT_EQ(op->metadata.getReshardingUUID(), metadata2.getReshardingUUID());
}

}  // namespace
}  // namespace mongo

