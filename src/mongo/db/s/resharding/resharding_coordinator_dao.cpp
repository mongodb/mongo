/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_coordinator_dao.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace resharding {

namespace {
BSONObj executeConfigRequest(OperationContext* opCtx, const BatchedCommandRequest& request) {
    DBDirectClient client(opCtx);
    BSONObj result;
    client.runCommand(request.getNS().dbName(), request.toBSON(), result);
    return result;
};

void verifyUpdateResult(const BatchedCommandRequest& request, const BSONObj& result) {
    auto numDocsMatched = result.getIntField("n");
    uassert(10323900,
            str::stream() << "Expected to match " << 1 << " docs, but only matched "
                          << numDocsMatched << " for write request " << request.toString(),
            1 == numDocsMatched);
}

ReshardingCoordinatorDocument buildAndExecuteRequest(OperationContext* opCtx,
                                                     std::unique_ptr<DaoStorageClient> client,
                                                     const UUID& reshardingUUID,
                                                     BSONObjBuilder& bob) {
    auto request =
        BatchedCommandRequest::buildUpdateOp(NamespaceString::kConfigReshardingOperationsNamespace,
                                             BSON("_id" << reshardingUUID),
                                             bob.obj(),
                                             false,  // upsert
                                             false   // multi
        );
    client->alterState(opCtx, request);
    return client->readState(opCtx, reshardingUUID);
}

boost::optional<StringData> getTimedPhaseFieldNameFor(CoordinatorStateEnum coordinatorPhase) {
    switch (coordinatorPhase) {
        case CoordinatorStateEnum::kCloning:
            return ReshardingCoordinatorMetrics::kDocumentCopyFieldName;
        case CoordinatorStateEnum::kApplying:
            return ReshardingCoordinatorMetrics::kOplogApplicationFieldName;
        default:
            return boost::none;
    }
    MONGO_UNREACHABLE;
}

boost::optional<std::string> getTimedPhaseStartFieldFor(CoordinatorStateEnum coordinatorPhase) {
    auto timedPhase = getTimedPhaseFieldNameFor(coordinatorPhase);
    if (!timedPhase) {
        return boost::none;
    }
    return resharding_metrics::getIntervalStartFieldName<ReshardingCoordinatorDocument>(
        *timedPhase);
}

boost::optional<std::string> getTimedPhaseEndFieldFor(CoordinatorStateEnum coordinatorPhase) {
    auto timedPhase = getTimedPhaseFieldNameFor(coordinatorPhase);
    if (!timedPhase) {
        return boost::none;
    }
    return resharding_metrics::getIntervalEndFieldName<ReshardingCoordinatorDocument>(*timedPhase);
}
}  // namespace

void DaoStorageClientImpl::alterState(OperationContext* opCtx,
                                      const BatchedCommandRequest& request) {

    auto result = executeConfigRequest(opCtx, request);
    uassertStatusOK(getStatusFromWriteCommandReply(result));
    verifyUpdateResult(request, result);
}

ReshardingCoordinatorDocument DaoStorageClientImpl::readState(OperationContext* opCtx,
                                                              const UUID& reshardingUUID) {
    return getCoordinatorDoc(opCtx, reshardingUUID);
}

void TransactionalDaoStorageClientImpl::alterState(OperationContext* opCtx,
                                                   const BatchedCommandRequest& request) {
    auto result = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, request.getNS(), request, _txnNumber);
    verifyUpdateResult(request, result);
}

ReshardingCoordinatorDocument TransactionalDaoStorageClientImpl::readState(
    OperationContext* opCtx, const UUID& reshardingUUID) {
    auto result = ShardingCatalogManager::get(opCtx)->findOneConfigDocumentInTxn(
        opCtx,
        NamespaceString::kConfigReshardingOperationsNamespace,
        _txnNumber,
        BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName << reshardingUUID));
    uassert(10323901,
            str::stream() << "Could not find the coordinator document for the resharding operation "
                          << reshardingUUID.toString(),
            result.has_value() && !result->isEmpty());
    return ReshardingCoordinatorDocument::parse(*result,
                                                IDLParserContext("ReshardingCoordinatorDocument"));
}

CoordinatorStateEnum ReshardingCoordinatorDao::getPhase(OperationContext* opCtx,
                                                        boost::optional<TxnNumber> txnNumber) {
    auto client = _clientFactory->createDaoStorageClient(txnNumber);
    return client->readState(opCtx, _reshardingUUID).getState();
}

ReshardingCoordinatorDocument ReshardingCoordinatorDao::transitionToPreparingToDonatePhase(
    OperationContext* opCtx,
    resharding::ParticipantShardsAndChunks shardsAndChunks,
    boost::optional<TxnNumber> txnNumber) {

    auto client = _clientFactory->createDaoStorageClient(txnNumber);
    auto doc = client->readState(opCtx, _reshardingUUID);
    invariant(doc.getState() == CoordinatorStateEnum::kInitializing);

    BSONObjBuilder updateBuilder;
    {
        BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));
        setBuilder.append(ReshardingCoordinatorDocument::kStateFieldName,
                          CoordinatorState_serializer(CoordinatorStateEnum::kPreparingToDonate));

        BSONArrayBuilder donorShards(
            setBuilder.subarrayStart(ReshardingCoordinatorDocument::kDonorShardsFieldName));
        for (const auto& donorShard : shardsAndChunks.donorShards) {
            donorShards.append(donorShard.toBSON());
        }
        donorShards.doneFast();

        BSONArrayBuilder recipientShards(
            setBuilder.subarrayStart(ReshardingCoordinatorDocument::kRecipientShardsFieldName));
        for (const auto& recipientShard : shardsAndChunks.recipientShards) {
            recipientShards.append(recipientShard.toBSON());
        }
        recipientShards.doneFast();

        setBuilder.doneFast();

        // Remove the presetReshardedChunks and zones from the coordinator document to reduce
        // the possibility of the document reaching the BSONObj size constraint.
        BSONObjBuilder unsetBuilder(updateBuilder.subobjStart("$unset"));
        unsetBuilder.append(ReshardingCoordinatorDocument::kPresetReshardedChunksFieldName, "");
        unsetBuilder.append(ReshardingCoordinatorDocument::kZonesFieldName, "");
        unsetBuilder.doneFast();
    }

    return buildAndExecuteRequest(opCtx, std::move(client), _reshardingUUID, updateBuilder);
}

ReshardingCoordinatorDocument ReshardingCoordinatorDao::transitionToCloningPhase(
    OperationContext* opCtx,
    Date_t now,
    Timestamp cloneTimestamp,
    ReshardingApproxCopySize approxCopySize,
    boost::optional<TxnNumber> txnNumber) {

    auto client = _clientFactory->createDaoStorageClient(txnNumber);
    auto doc = client->readState(opCtx, _reshardingUUID);
    invariant(doc.getState() == CoordinatorStateEnum::kPreparingToDonate);

    // We know these values exist, so doc will contain these fields after the function calls.
    resharding::emplaceApproxBytesToCopyIfExists(doc, std::move(approxCopySize));
    resharding::emplaceCloneTimestampIfExists(doc, std::move(cloneTimestamp));

    BSONObjBuilder updateBuilder;
    {
        BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

        // Always update the state field.
        setBuilder.append(ReshardingCoordinatorDocument::kStateFieldName,
                          CoordinatorState_serializer(CoordinatorStateEnum::kCloning));
        setBuilder.append(ReshardingCoordinatorDocument::kCloneTimestampFieldName,
                          *doc.getCloneTimestamp());
        setBuilder.append(ReshardingCoordinatorDocument::kApproxBytesToCopyFieldName,
                          *doc.getApproxBytesToCopy());
        setBuilder.append(ReshardingCoordinatorDocument::kApproxDocumentsToCopyFieldName,
                          *doc.getApproxDocumentsToCopy());

        // Update cloning metrics.
        setBuilder.append(*getTimedPhaseStartFieldFor(CoordinatorStateEnum::kCloning), now);
    }

    return buildAndExecuteRequest(opCtx, std::move(client), _reshardingUUID, updateBuilder);
}

ReshardingCoordinatorDocument ReshardingCoordinatorDao::transitionToBlockingWritesPhase(
    OperationContext* opCtx,
    Date_t now,
    Date_t criticalSectionExpireTime,
    boost::optional<TxnNumber> txnNumber) {

    auto client = _clientFactory->createDaoStorageClient(txnNumber);
    auto doc = client->readState(opCtx, _reshardingUUID);
    invariant(doc.getState() == CoordinatorStateEnum::kApplying);

    BSONObjBuilder updateBuilder;
    {
        BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

        setBuilder.append(ReshardingCoordinatorDocument::kStateFieldName,
                          CoordinatorState_serializer(CoordinatorStateEnum::kBlockingWrites));

        setBuilder.append(*getTimedPhaseEndFieldFor(CoordinatorStateEnum::kApplying), now);

        setBuilder.append(ReshardingCoordinatorDocument::kCriticalSectionExpiresAtFieldName,
                          criticalSectionExpireTime);
    }

    return buildAndExecuteRequest(opCtx, std::move(client), _reshardingUUID, updateBuilder);
}

ReshardingCoordinatorDocument ReshardingCoordinatorDao::transitionToApplyingPhase(
    OperationContext* opCtx, Date_t now, boost::optional<TxnNumber> txnNumber) {

    auto client = _clientFactory->createDaoStorageClient(txnNumber);
    auto doc = client->readState(opCtx, _reshardingUUID);
    invariant(doc.getState() == CoordinatorStateEnum::kCloning);

    BSONObjBuilder updateBuilder;
    {
        BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

        // Always update the state field.
        setBuilder.append(ReshardingCoordinatorDocument::kStateFieldName,
                          CoordinatorState_serializer(CoordinatorStateEnum::kApplying));

        // Update applying metrics.
        setBuilder.append(*getTimedPhaseEndFieldFor(CoordinatorStateEnum::kCloning), now);
        setBuilder.append(*getTimedPhaseStartFieldFor(CoordinatorStateEnum::kApplying), now);
    }

    return buildAndExecuteRequest(opCtx, std::move(client), _reshardingUUID, updateBuilder);
}

ReshardingCoordinatorDocument ReshardingCoordinatorDao::transitionToAbortingPhase(
    OperationContext* opCtx, Date_t now, Status abortReason, boost::optional<TxnNumber> txnNumber) {

    auto client = _clientFactory->createDaoStorageClient(txnNumber);
    auto doc = client->readState(opCtx, _reshardingUUID);
    // Participants are not aware of resharding until after the Initializing phase, so the
    // coordinator will clean itself up and immediately move to Done instead of going through
    // Aborting.
    invariant(doc.getState() > CoordinatorStateEnum::kInitializing &&
              doc.getState() < CoordinatorStateEnum::kAborting);

    BSONObjBuilder updateBuilder;
    {
        BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

        setBuilder.append(ReshardingCoordinatorDocument::kStateFieldName,
                          CoordinatorState_serializer(CoordinatorStateEnum::kAborting));

        setBuilder.append(ReshardingCoordinatorDocument::kAbortReasonFieldName,
                          resharding::serializeAndTruncateReshardingErrorIfNeeded(abortReason));

        if (auto endingTimedPhaseFieldName = getTimedPhaseEndFieldFor(doc.getState())) {
            setBuilder.append(*endingTimedPhaseFieldName, now);
        }
    }

    return buildAndExecuteRequest(opCtx, std::move(client), _reshardingUUID, updateBuilder);
}


}  // namespace resharding
}  // namespace mongo
