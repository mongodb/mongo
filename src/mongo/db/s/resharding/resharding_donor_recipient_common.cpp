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

#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"

namespace mongo {
namespace resharding {

using DonorStateMachine = ReshardingDonorService::DonorStateMachine;
using RecipientStateMachine = ReshardingRecipientService::RecipientStateMachine;

namespace {
std::vector<DonorShardMirroringEntry> createDonorShardMirroringEntriesFromDonorShardIds(
    const std::vector<ShardId>& shardIds) {
    std::vector<DonorShardMirroringEntry> donorShards(shardIds.size());
    for (size_t i = 0; i < shardIds.size(); ++i) {
        donorShards[i] = {shardIds[i], false /* mirroring */};
    }
    return donorShards;
}

/*
 * Creates a ReshardingStateMachine with the assumption that the state machine does not already
 * exist.
 */
template <class Service, class StateMachine, class ReshardingDocument>
void createReshardingStateMachine(OperationContext* opCtx, const ReshardingDocument& doc) {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    auto service = registry->lookupServiceByName(Service::kServiceName);
    StateMachine::getOrCreate(opCtx, service, doc.toBSON());
}

/*
 * Either constructs a new ReshardingDonorStateMachine with 'reshardingFields' or passes
 * 'reshardingFields' to an already-existing ReshardingDonorStateMachine.
 */
void processReshardingFieldsForDonorCollection(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const CollectionMetadata& metadata,
                                               const ReshardingFields& reshardingFields) {
    if (auto donorStateMachine = tryGetReshardingStateMachine<ReshardingDonorService,
                                                              DonorStateMachine,
                                                              ReshardingDonorDocument>(
            opCtx, reshardingFields.getUuid())) {
        donorStateMachine->get()->onReshardingFieldsChanges(reshardingFields);
        return;
    }

    // If a resharding operation is past state kPreparingToDonate but does not currently have a
    // donor document in-memory, this means that the document will be recovered by the
    // ReshardingDonorService, and at that time the latest instance of 'reshardingFields' will be
    // read. Return no-op.
    if (reshardingFields.getState() > CoordinatorStateEnum::kPreparingToDonate) {
        return;
    }

    auto donorDoc = constructDonorDocumentFromReshardingFields(nss, metadata, reshardingFields);
    createReshardingStateMachine<ReshardingDonorService,
                                 DonorStateMachine,
                                 ReshardingDonorDocument>(opCtx, donorDoc);
}

/*
 * Either constructs a new ReshardingRecipientStateMachine with 'reshardingFields' or passes
 * 'reshardingFields' to an already-existing ReshardingRecipientStateMachine.
 */
void processReshardingFieldsForRecipientCollection(OperationContext* opCtx,
                                                   const CollectionMetadata& metadata,
                                                   const ReshardingFields& reshardingFields) {
    if (auto recipientStateMachine = tryGetReshardingStateMachine<ReshardingRecipientService,
                                                                  RecipientStateMachine,
                                                                  ReshardingRecipientDocument>(
            opCtx, reshardingFields.getUuid())) {
        recipientStateMachine->get()->onReshardingFieldsChanges(reshardingFields);
        return;
    }

    // If a resharding operation is past state kCloning but does not currently have a recipient
    // document in-memory, this means that the document will be recovered by the
    // ReshardingRecipientService, and at that time the latest instance of 'reshardingFields'
    // will be read. Return no-op.
    if (reshardingFields.getState() > CoordinatorStateEnum::kCloning) {
        return;
    }

    auto recipientDoc =
        constructRecipientDocumentFromReshardingFields(opCtx, metadata, reshardingFields);
    createReshardingStateMachine<ReshardingRecipientService,
                                 RecipientStateMachine,
                                 ReshardingRecipientDocument>(opCtx, recipientDoc);
}

}  // namespace

ReshardingDonorDocument constructDonorDocumentFromReshardingFields(
    const NamespaceString& nss,
    const CollectionMetadata& metadata,
    const ReshardingFields& reshardingFields) {
    auto donorDoc = ReshardingDonorDocument(DonorStateEnum::kPreparingToDonate);

    auto commonMetadata =
        CommonReshardingMetadata(reshardingFields.getUuid(),
                                 nss,
                                 getCollectionUUIDFromChunkManger(nss, *metadata.getChunkManager()),
                                 reshardingFields.getDonorFields()->getReshardingKey().toBSON());
    donorDoc.setCommonReshardingMetadata(std::move(commonMetadata));

    return donorDoc;
}

ReshardingRecipientDocument constructRecipientDocumentFromReshardingFields(
    OperationContext* opCtx,
    const CollectionMetadata& metadata,
    const ReshardingFields& reshardingFields) {
    std::vector<DonorShardMirroringEntry> donorShards =
        createDonorShardMirroringEntriesFromDonorShardIds(
            reshardingFields.getRecipientFields()->getDonorShardIds());

    auto recipientDoc =
        ReshardingRecipientDocument(RecipientStateEnum::kCloning, std::move(donorShards));

    auto commonMetadata =
        CommonReshardingMetadata(reshardingFields.getUuid(),
                                 reshardingFields.getRecipientFields()->getOriginalNamespace(),
                                 reshardingFields.getRecipientFields()->getExistingUUID(),
                                 metadata.getShardKeyPattern().toBSON());
    recipientDoc.setCommonReshardingMetadata(std::move(commonMetadata));

    emplaceFetchTimestampIfExists(recipientDoc,
                                  reshardingFields.getRecipientFields()->getFetchTimestamp());

    return recipientDoc;
}

void processReshardingFieldsForCollection(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const CollectionMetadata& metadata,
                                          const ReshardingFields& reshardingFields) {
    if (reshardingFields.getDonorFields()) {
        invariant(!reshardingFields.getRecipientFields());
        processReshardingFieldsForDonorCollection(opCtx, nss, metadata, reshardingFields);
        return;
    }

    if (reshardingFields.getRecipientFields()) {
        invariant(!reshardingFields.getDonorFields());
        processReshardingFieldsForRecipientCollection(opCtx, metadata, reshardingFields);
        return;
    }

    MONGO_UNREACHABLE
}

}  // namespace resharding

}  // namespace mongo
