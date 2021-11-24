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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/db/s/balancer/balancer_defragmentation_policy_impl.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/grid.h"

namespace mongo {

void BalancerDefragmentationPolicyImpl::beginNewCollection(OperationContext* opCtx,
                                                           const UUID& uuid) {
    _persistPhaseUpdate(opCtx, DefragmentationPhaseEnum::kNotStarted, uuid);
    _initializeCollectionState(opCtx, uuid);
}

void BalancerDefragmentationPolicyImpl::removeCollection(OperationContext* opCtx,
                                                         const UUID& uuid) {
    _persistPhaseUpdate(opCtx, boost::none, uuid);
    _clearDataSizeInformation(opCtx, uuid);
    _defragmentationStates.erase(uuid);
}

SemiFuture<DefragmentationAction> BalancerDefragmentationPolicyImpl::getNextStreamingAction() {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    if (_concurrentStreamingOps < kMaxConcurrentOperations) {
        _concurrentStreamingOps++;
        if (auto action = _nextStreamingAction()) {
            return SemiFuture<DefragmentationAction>::makeReady(*action);
        }
    }
    auto&& [promise, future] = makePromiseFuture<DefragmentationAction>();
    _nextStreamingActionPromise = std::move(promise);
    return std::move(future).semi();
}

boost::optional<DefragmentationAction> BalancerDefragmentationPolicyImpl::_nextStreamingAction() {
    // TODO (SERVER-61635) validate fairness through collections
    for (auto& [uuid, collectionData] : _defragmentationStates) {
        if (!collectionData.queuedActions.empty()) {
            auto action = collectionData.queuedActions.front();
            collectionData.queuedActions.pop();
            return action;
        }
        if (collectionData.phase == DefragmentationPhaseEnum::kMergeChunks) {
            if (auto mergeAction = _getCollectionMergeAction(collectionData)) {
                return boost::optional<DefragmentationAction>(*mergeAction);
            }
        }
        if (collectionData.phase == DefragmentationPhaseEnum::kSplitChunks) {
            if (auto splitAction = _getCollectionSplitAction(collectionData)) {
                return boost::optional<DefragmentationAction>(*splitAction);
            }
        }
    }
    boost::optional<DefragmentationAction> noAction = boost::none;
    if (_streamClosed) {
        noAction = boost::optional<EndOfActionStream>();
    }
    return noAction;
}

void BalancerDefragmentationPolicyImpl::acknowledgeMergeResult(MergeInfo action,
                                                               const Status& result) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    boost::optional<DefragmentationAction> nextActionOnNamespace = result.isOK()
        ? boost::optional<DefragmentationAction>(
              DataSizeInfo(action.shardId,
                           action.nss,
                           action.uuid,
                           action.chunkRange,
                           action.collectionVersion,
                           _defragmentationStates.at(action.uuid).collectionShardKey,
                           false))
        : boost::optional<DefragmentationAction>(action);
    _processEndOfAction(lk, action.uuid, nextActionOnNamespace);
}

void BalancerDefragmentationPolicyImpl::acknowledgeDataSizeResult(
    OperationContext* opCtx, DataSizeInfo action, const StatusWith<DataSizeResponse>& result) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    if (result.isOK()) {
        ChunkType chunk(action.uuid, action.chunkRange, action.version, action.shardId);
        ShardingCatalogManager* catalogManager = ShardingCatalogManager::get(opCtx);
        catalogManager->setChunkEstimatedSize(opCtx,
                                              chunk,
                                              result.getValue().sizeBytes,
                                              ShardingCatalogClient::kMajorityWriteConcern);
    }
    boost::optional<DefragmentationAction> nextActionOnNamespace =
        result.isOK() ? boost::none : boost::optional<DefragmentationAction>(action);
    _processEndOfAction(lk, action.uuid, nextActionOnNamespace);
}

void BalancerDefragmentationPolicyImpl::acknowledgeAutoSplitVectorResult(
    AutoSplitVectorInfo action, const StatusWith<std::vector<BSONObj>>& result) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    boost::optional<DefragmentationAction> nextActionOnNamespace = result.isOK()
        ? boost::optional<DefragmentationAction>(SplitInfoWithKeyPattern(action.shardId,
                                                                         action.nss,
                                                                         action.collectionVersion,
                                                                         action.minKey,
                                                                         action.maxKey,
                                                                         result.getValue(),
                                                                         action.uuid,
                                                                         action.keyPattern))
        : boost::optional<DefragmentationAction>(action);
    _processEndOfAction(lk, action.uuid, nextActionOnNamespace);
}

void BalancerDefragmentationPolicyImpl::acknowledgeSplitResult(SplitInfoWithKeyPattern action,
                                                               const Status& result) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    boost::optional<DefragmentationAction> nextActionOnNamespace =
        result.isOK() ? boost::none : boost::optional<DefragmentationAction>(action);
    _processEndOfAction(lk, action.uuid, nextActionOnNamespace);
}

void BalancerDefragmentationPolicyImpl::closeActionStream() {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    _defragmentationStates.clear();
    if (_nextStreamingActionPromise) {
        _nextStreamingActionPromise.get().setFrom(EndOfActionStream());
        _nextStreamingActionPromise = boost::none;
    }
    _streamClosed = true;
}

void BalancerDefragmentationPolicyImpl::_processEndOfAction(
    WithLock,
    const UUID& uuid,
    const boost::optional<DefragmentationAction>& nextActionOnNamespace) {
    // If the end of the current action implies a next step and the related collection is still
    // being defragmented, store it
    if (nextActionOnNamespace) {
        auto collectionDefragmentationStateIt = _defragmentationStates.find(uuid);
        if (collectionDefragmentationStateIt != _defragmentationStates.end()) {
            collectionDefragmentationStateIt->second.queuedActions.push(*nextActionOnNamespace);
        }
    }

    // If there is a client blocked on the stream, serve it now with a new action...
    if (_nextStreamingActionPromise) {
        auto nextStreamingAction = _nextStreamingAction();
        if (nextStreamingAction) {
            _nextStreamingActionPromise.get().setWith([&] { return *nextStreamingAction; });
            _nextStreamingActionPromise = boost::none;
            return;
        }
    }
    // ... otherwise, just lower the counter
    --_concurrentStreamingOps;
}

void BalancerDefragmentationPolicyImpl::_initializeCollectionState(OperationContext* opCtx,
                                                                   const UUID& uuid) {
    try {
        _defragmentationStates[uuid].phase = DefragmentationPhaseEnum::kNotStarted;
    } catch (const DBException& e) {
        LOGV2_ERROR(6153101,
                    "Error while starting defragmentation on collection",
                    "uuid"_attr = uuid,
                    "error"_attr = e);
    }
}

void BalancerDefragmentationPolicyImpl::_persistPhaseUpdate(
    OperationContext* opCtx, boost::optional<DefragmentationPhaseEnum> phase, const UUID& uuid) {
    DBDirectClient dbClient(opCtx);
    write_ops::UpdateCommandRequest updateOp(CollectionType::ConfigNS);
    updateOp.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON(CollectionType::kUuidFieldName << uuid));
        if (phase) {
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                BSON("$set" << BSON(CollectionType::kDefragmentationPhaseFieldName
                                    << DefragmentationPhase_serializer(*phase)))));
        } else {
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                BSON("$unset" << BSON(CollectionType::kDefragmentationPhaseFieldName << ""))));
        }
        return entry;
    }()});
    dbClient.update(updateOp);
}

void BalancerDefragmentationPolicyImpl::_clearDataSizeInformation(OperationContext* opCtx,
                                                                  const UUID& uuid) {
    DBDirectClient dbClient(opCtx);
    write_ops::UpdateCommandRequest updateOp(ChunkType::ConfigNS);
    updateOp.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON(CollectionType::kUuidFieldName << uuid));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$unset" << BSON(ChunkType::estimatedSizeBytes.name() << ""))));
        entry.setMulti(true);
        return entry;
    }()});
    dbClient.update(updateOp);
}

}  // namespace mongo
