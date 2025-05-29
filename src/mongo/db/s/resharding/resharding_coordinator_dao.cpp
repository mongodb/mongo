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
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_util.h"

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
                                                     DaoStorageClient* client,
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
    return ReshardingCoordinatorDocument::parse(IDLParserContext("ReshardingCoordinatorDocument"),
                                                *result);
}

using resharding_metrics::getIntervalEndFieldName;
using resharding_metrics::getIntervalStartFieldName;

ReshardingCoordinatorDocument ReshardingCoordinatorDao::transitionToCloningPhase(
    OperationContext* opCtx,
    DaoStorageClient* client,
    Date_t now,
    Timestamp cloneTimestamp,
    ReshardingApproxCopySize approxCopySize,
    const UUID& reshardingUUID) {

    auto doc = client->readState(opCtx, reshardingUUID);
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
        setBuilder.append(getIntervalStartFieldName<ReshardingCoordinatorDocument>(
                              ReshardingRecipientMetrics::kDocumentCopyFieldName),
                          now);
    }

    return buildAndExecuteRequest(opCtx, client, reshardingUUID, updateBuilder);
}

ReshardingCoordinatorDocument ReshardingCoordinatorDao::transitionToBlockingWritesPhase(
    OperationContext* opCtx,
    DaoStorageClient* client,
    Date_t now,
    Date_t criticalSectionExpireTime,
    const UUID& reshardingUUID) {

    auto doc = client->readState(opCtx, reshardingUUID);
    invariant(doc.getState() == CoordinatorStateEnum::kApplying);

    BSONObjBuilder updateBuilder;
    {
        BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

        setBuilder.append(ReshardingCoordinatorDocument::kStateFieldName,
                          CoordinatorState_serializer(CoordinatorStateEnum::kBlockingWrites));

        setBuilder.append(getIntervalEndFieldName<ReshardingCoordinatorDocument>(
                              ReshardingRecipientMetrics::kOplogApplicationFieldName),
                          now);

        setBuilder.append(ReshardingCoordinatorDocument::kCriticalSectionExpiresAtFieldName,
                          criticalSectionExpireTime);
    }

    return buildAndExecuteRequest(opCtx, client, reshardingUUID, updateBuilder);
}

ReshardingCoordinatorDocument ReshardingCoordinatorDao::transitionToApplyingPhase(
    OperationContext* opCtx, DaoStorageClient* client, Date_t now, const UUID& reshardingUUID) {
    auto doc = client->readState(opCtx, reshardingUUID);
    invariant(doc.getState() == CoordinatorStateEnum::kCloning);

    BSONObjBuilder updateBuilder;
    {
        BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

        // Always update the state field.
        setBuilder.append(ReshardingCoordinatorDocument::kStateFieldName,
                          CoordinatorState_serializer(CoordinatorStateEnum::kApplying));

        // Update applying metrics.
        setBuilder.append(getIntervalEndFieldName<ReshardingCoordinatorDocument>(
                              ReshardingRecipientMetrics::kDocumentCopyFieldName),
                          now);
        setBuilder.append(getIntervalStartFieldName<ReshardingCoordinatorDocument>(
                              ReshardingRecipientMetrics::kOplogApplicationFieldName),
                          now);
    }

    return buildAndExecuteRequest(opCtx, client, reshardingUUID, updateBuilder);
}

}  // namespace resharding
}  // namespace mongo
