/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_donor_recipient_common_test.h"

#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

using namespace fmt::literals;

TEST_F(ReshardingDonorRecipientCommonInternalsTest, ConstructDonorDocumentFromReshardingFields) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadataForOriginalCollection(opCtx, kThisShard.getShardId());

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
    appendDonorFieldsToReshardingFields(reshardingFields, kReshardingKeyPattern);

    auto donorDoc = resharding::constructDonorDocumentFromReshardingFields(
        kOriginalNss, metadata, reshardingFields);
    assertDonorDocMatchesReshardingFields(kOriginalNss, kExistingUUID, reshardingFields, donorDoc);
}

TEST_F(ReshardingDonorRecipientCommonInternalsTest,
       ConstructRecipientDocumentFromReshardingFields) {
    OperationContext* opCtx = operationContext();
    auto metadata =
        makeShardedMetadataForTemporaryReshardingCollection(opCtx, kThisShard.getShardId());

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);

    appendRecipientFieldsToReshardingFields(reshardingFields, kShards, kExistingUUID, kOriginalNss);

    auto recipientDoc = resharding::constructRecipientDocumentFromReshardingFields(
        opCtx, kTemporaryReshardingNss, metadata, reshardingFields);
    assertRecipientDocMatchesReshardingFields(metadata, reshardingFields, recipientDoc);
}

TEST_F(ReshardingDonorRecipientCommonTest, CreateDonorServiceInstance) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadataForOriginalCollection(opCtx, kThisShard.getShardId());

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
    appendDonorFieldsToReshardingFields(reshardingFields, kReshardingKeyPattern);

    resharding::processReshardingFieldsForCollection(
        opCtx, kOriginalNss, metadata, reshardingFields);

    auto donorStateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingDonorService,
                                                 ReshardingDonorService::DonorStateMachine,
                                                 ReshardingDonorDocument>(opCtx, kReshardingUUID);

    ASSERT(donorStateMachine != boost::none);

    donorStateMachine.get()->interrupt({ErrorCodes::InternalError, "Shut down for test"});
}

TEST_F(ReshardingDonorRecipientCommonTest, CreateRecipientServiceInstance) {
    OperationContext* opCtx = operationContext();
    auto metadata =
        makeShardedMetadataForTemporaryReshardingCollection(opCtx, kThisShard.getShardId());

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
    appendRecipientFieldsToReshardingFields(reshardingFields, kShards, kExistingUUID, kOriginalNss);

    resharding::processReshardingFieldsForCollection(
        opCtx, kTemporaryReshardingNss, metadata, reshardingFields);

    auto recipientStateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingRecipientService,
                                                 ReshardingRecipientService::RecipientStateMachine,
                                                 ReshardingRecipientDocument>(opCtx,
                                                                              kReshardingUUID);

    ASSERT(recipientStateMachine != boost::none);

    recipientStateMachine.get()->interrupt({ErrorCodes::InternalError, "Shut down for test"});
}

TEST_F(ReshardingDonorRecipientCommonTest,
       CreateDonorServiceInstanceWithIncorrectCoordinatorState) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadataForOriginalCollection(opCtx, kThisShard.getShardId());

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kCommitting);
    appendDonorFieldsToReshardingFields(reshardingFields, kReshardingKeyPattern);

    auto donorStateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingDonorService,
                                                 ReshardingDonorService::DonorStateMachine,
                                                 ReshardingDonorDocument>(opCtx, kReshardingUUID);
    ASSERT(donorStateMachine == boost::none);
}

TEST_F(ReshardingDonorRecipientCommonTest,
       CreateRecipientServiceInstanceWithIncorrectCoordinatorState) {
    OperationContext* opCtx = operationContext();
    auto metadata =
        makeShardedMetadataForTemporaryReshardingCollection(opCtx, kThisShard.getShardId());

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kCommitting);
    appendRecipientFieldsToReshardingFields(
        reshardingFields, kShards, kExistingUUID, kOriginalNss, kCloneTimestamp);

    ASSERT_THROWS_CODE(resharding::processReshardingFieldsForCollection(
                           opCtx, kTemporaryReshardingNss, metadata, reshardingFields),
                       DBException,
                       5274202);

    auto recipientStateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingRecipientService,
                                                 ReshardingRecipientService::RecipientStateMachine,
                                                 ReshardingRecipientDocument>(opCtx,
                                                                              kReshardingUUID);

    ASSERT(recipientStateMachine == boost::none);
}

TEST_F(ReshardingDonorRecipientCommonTest, ProcessDonorFieldsWhenShardDoesntOwnAnyChunks) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadataForOriginalCollection(opCtx, kOtherShard.getShardId());

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
    appendDonorFieldsToReshardingFields(reshardingFields, kReshardingKeyPattern);

    resharding::processReshardingFieldsForCollection(
        opCtx, kOriginalNss, metadata, reshardingFields);

    auto donorStateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingDonorService,
                                                 ReshardingDonorService::DonorStateMachine,
                                                 ReshardingDonorDocument>(opCtx, kReshardingUUID);

    ASSERT(donorStateMachine == boost::none);
}

TEST_F(ReshardingDonorRecipientCommonTest, ProcessRecipientFieldsWhenShardDoesntOwnAnyChunks) {
    OperationContext* opCtx = operationContext();
    auto metadata =
        makeShardedMetadataForTemporaryReshardingCollection(opCtx, kOtherShard.getShardId());

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
    appendRecipientFieldsToReshardingFields(reshardingFields, kShards, kExistingUUID, kOriginalNss);

    resharding::processReshardingFieldsForCollection(
        opCtx, kTemporaryReshardingNss, metadata, reshardingFields);

    auto recipientStateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingRecipientService,
                                                 ReshardingRecipientService::RecipientStateMachine,
                                                 ReshardingRecipientDocument>(opCtx,
                                                                              kReshardingUUID);

    ASSERT(recipientStateMachine == boost::none);
}

TEST_F(ReshardingDonorRecipientCommonTest, ProcessReshardingFieldsWithoutDonorOrRecipientFields) {
    OperationContext* opCtx = operationContext();
    auto metadata =
        makeShardedMetadataForTemporaryReshardingCollection(opCtx, kThisShard.getShardId());

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);

    ASSERT_THROWS_CODE(resharding::processReshardingFieldsForCollection(
                           opCtx, kTemporaryReshardingNss, metadata, reshardingFields),
                       DBException,
                       5274201);
}

TEST_F(ReshardingDonorRecipientCommonInternalsTest, ClearReshardingFilteringMetaData) {
    OperationContext* opCtx = operationContext();

    const bool scheduleAsyncRefresh = false;
    auto doSetupFunc = [&] {
        // Clear out the resharding donor/recipient metadata collections.
        for (auto const& nss : {NamespaceString::kDonorReshardingOperationsNamespace,
                                NamespaceString::kRecipientReshardingOperationsNamespace}) {
            dropDatabase(opCtx, nss.db().toString()).ignore();
        }

        // Assert the prestate has no filtering metadata.
        for (auto const& nss : {kOriginalNss, kTemporaryReshardingNss}) {
            AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_IS);
            auto csr = CollectionShardingRuntime::get(opCtx, nss);
            ASSERT(csr->getCurrentMetadataIfKnown() == boost::none);
        }

        // Add filtering metadata for the collection being resharded.
        {
            AutoGetCollection autoColl(opCtx, kOriginalNss, LockMode::MODE_IS);
            auto csr = CollectionShardingRuntime::get(opCtx, kOriginalNss);
            csr->setFilteringMetadata(
                opCtx, makeShardedMetadataForOriginalCollection(opCtx, kThisShard.getShardId()));
            ASSERT(csr->getCurrentMetadataIfKnown());
        }

        // Add filtering metadata for the temporary resharding namespace.
        {
            AutoGetCollection autoColl(opCtx, kTemporaryReshardingNss, LockMode::MODE_IS);
            auto csr = CollectionShardingRuntime::get(opCtx, kTemporaryReshardingNss);
            csr->setFilteringMetadata(opCtx,
                                      makeShardedMetadataForTemporaryReshardingCollection(
                                          opCtx, kThisShard.getShardId()));
            ASSERT(csr->getCurrentMetadataIfKnown());
        }

        // Prior to adding a resharding document, assert that attempting to clear filtering does
        // nothing.
        resharding::clearFilteringMetadata(opCtx, scheduleAsyncRefresh);

        for (auto const& nss : {kOriginalNss, kTemporaryReshardingNss}) {
            AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_IS);
            auto csr = CollectionShardingRuntime::get(opCtx, nss);
            ASSERT(csr->getCurrentMetadataIfKnown());
        }
    };

    doSetupFunc();
    // Add a resharding donor document that targets the namespaces involved in resharding.
    ReshardingDonorDocument donorDoc = makeDonorStateDoc();
    ReshardingDonorService::DonorStateMachine::insertStateDocument(opCtx, donorDoc);

    // Clear the filtering metadata (without scheduling a refresh) and assert the metadata is gone.
    resharding::clearFilteringMetadata(opCtx, scheduleAsyncRefresh);

    for (auto const& nss : {kOriginalNss, kTemporaryReshardingNss}) {
        AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_IS);
        auto csr = CollectionShardingRuntime::get(opCtx, nss);
        ASSERT(csr->getCurrentMetadataIfKnown() == boost::none);
    }

    doSetupFunc();
    // Add a resharding recipient document that targets the namespaces involved in resharding.
    ReshardingRecipientDocument recipDoc = makeRecipientStateDoc();
    ReshardingRecipientService::RecipientStateMachine::insertStateDocument(opCtx, recipDoc);

    // Clear the filtering metadata (without scheduling a refresh) and assert the metadata is gone.
    resharding::clearFilteringMetadata(opCtx, scheduleAsyncRefresh);

    for (auto const& nss : {kOriginalNss, kTemporaryReshardingNss}) {
        AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_IS);
        auto csr = CollectionShardingRuntime::get(opCtx, nss);
        ASSERT(csr->getCurrentMetadataIfKnown() == boost::none);
    }
}
}  // namespace

}  // namespace mongo
