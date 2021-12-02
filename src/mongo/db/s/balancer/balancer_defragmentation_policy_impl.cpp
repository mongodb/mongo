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
MONGO_FAIL_POINT_DEFINE(skipPhaseTransition);

void BalancerDefragmentationPolicyImpl::refreshCollectionDefragmentationStatus(
    OperationContext* opCtx, const CollectionType& coll) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    const auto& uuid = coll.getUuid();
    if (coll.getBalancerShouldMergeChunks() && !_defragmentationStates.contains(uuid)) {
        _initializeCollectionState(lk, opCtx, coll);

        // Load first action, this will trigger move to phase 2 if there are no phase 1 actions
        _queueNextAction(opCtx, uuid, _defragmentationStates[uuid]);
        // Fulfill promise if needed
        if (_nextStreamingActionPromise) {
            auto nextStreamingAction = _nextStreamingAction(opCtx);
            if (nextStreamingAction) {
                _concurrentStreamingOps++;
                _nextStreamingActionPromise.get().setWith([&] { return *nextStreamingAction; });
                _nextStreamingActionPromise = boost::none;
                return;
            }
        }
    } else if (!coll.getBalancerShouldMergeChunks() && _defragmentationStates.contains(uuid)) {
        _clearDataSizeInformation(opCtx, uuid);
        _defragmentationStates.erase(uuid);
        _persistPhaseUpdate(opCtx, boost::none, uuid);
    }
}

SemiFuture<DefragmentationAction> BalancerDefragmentationPolicyImpl::getNextStreamingAction(
    OperationContext* opCtx) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    if (_concurrentStreamingOps < kMaxConcurrentOperations) {
        if (auto action = _nextStreamingAction(opCtx)) {
            _concurrentStreamingOps++;
            return SemiFuture<DefragmentationAction>::makeReady(*action);
        }
    }
    auto [promise, future] = makePromiseFuture<DefragmentationAction>();
    _nextStreamingActionPromise = std::move(promise);
    return std::move(future).semi();
}

boost::optional<DefragmentationAction> BalancerDefragmentationPolicyImpl::_nextStreamingAction(
    OperationContext* opCtx) {
    // TODO (SERVER-61635) validate fairness through collections
    for (auto& [uuid, collectionData] : _defragmentationStates) {
        if (!collectionData.queuedActions.empty() ||
            _queueNextAction(opCtx, uuid, collectionData)) {
            return collectionData.popFromActionQueue();
        }
    }
    boost::optional<DefragmentationAction> noAction = boost::none;
    if (_streamClosed) {
        noAction = boost::optional<EndOfActionStream>();
    }
    return noAction;
}

bool BalancerDefragmentationPolicyImpl::_queueNextAction(
    OperationContext* opCtx, const UUID& uuid, CollectionDefragmentationState& collectionData) {
    // get next action within the current phase
    switch (collectionData.phase) {
        case DefragmentationPhaseEnum::kMergeChunks:
            if (auto mergeAction = _getCollectionMergeAction(collectionData)) {
                collectionData.queuedActions.push(*mergeAction);
                return true;
            }
            break;
        case DefragmentationPhaseEnum::kSplitChunks:
            if (auto splitAction = _getCollectionSplitAction(collectionData)) {
                collectionData.queuedActions.push(*splitAction);
                return true;
            }
            break;
        default:
            uasserted(ErrorCodes::BadValue, "Unsupported phase type");
    }
    // If no action for the current phase is available, check the conditions for transitioning to
    // the next phase
    if (collectionData.queuedActions.empty() && collectionData.outstandingActions == 0) {
        _transitionPhases(opCtx, uuid, collectionData);
    }
    return false;
}

void BalancerDefragmentationPolicyImpl::acknowledgeMergeResult(OperationContext* opCtx,
                                                               MergeInfo action,
                                                               const Status& result) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    // Check if collection defragmentation has been canceled
    if (!_defragmentationStates.contains(action.uuid)) {
        return;
    }
    if (result.isOK())
        _defragmentationStates[action.uuid].outstandingActions--;
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
    _processEndOfAction(lk, opCtx, action.uuid, nextActionOnNamespace);
}

void BalancerDefragmentationPolicyImpl::acknowledgeDataSizeResult(
    OperationContext* opCtx, DataSizeInfo action, const StatusWith<DataSizeResponse>& result) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    // Check if collection defragmentation has been canceled
    if (!_defragmentationStates.contains(action.uuid)) {
        return;
    }
    if (result.isOK()) {
        _defragmentationStates[action.uuid].outstandingActions--;
        ChunkType chunk(action.uuid, action.chunkRange, action.version, action.shardId);
        ShardingCatalogManager* catalogManager = ShardingCatalogManager::get(opCtx);
        catalogManager->setChunkEstimatedSize(opCtx,
                                              chunk,
                                              result.getValue().sizeBytes,
                                              ShardingCatalogClient::kMajorityWriteConcern);
    }
    boost::optional<DefragmentationAction> nextActionOnNamespace =
        result.isOK() ? boost::none : boost::optional<DefragmentationAction>(action);
    _processEndOfAction(lk, opCtx, action.uuid, nextActionOnNamespace);
}

void BalancerDefragmentationPolicyImpl::acknowledgeAutoSplitVectorResult(
    OperationContext* opCtx,
    AutoSplitVectorInfo action,
    const StatusWith<std::vector<BSONObj>>& result) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    // Check if collection defragmentation has been canceled
    if (!_defragmentationStates.contains(action.uuid)) {
        return;
    }
    if (result.isOK())
        _defragmentationStates[action.uuid].outstandingActions--;
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
    _processEndOfAction(lk, opCtx, action.uuid, nextActionOnNamespace);
}

void BalancerDefragmentationPolicyImpl::acknowledgeSplitResult(OperationContext* opCtx,
                                                               SplitInfoWithKeyPattern action,
                                                               const Status& result) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    // Check if collection defragmentation has been canceled
    if (!_defragmentationStates.contains(action.uuid)) {
        return;
    }
    if (result.isOK())
        _defragmentationStates[action.uuid].outstandingActions--;
    boost::optional<DefragmentationAction> nextActionOnNamespace =
        result.isOK() ? boost::none : boost::optional<DefragmentationAction>(action);
    _processEndOfAction(lk, opCtx, action.uuid, nextActionOnNamespace);
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
    OperationContext* opCtx,
    const UUID& uuid,
    const boost::optional<DefragmentationAction>& nextActionOnNamespace) {

    // If the end of the current action implies a next step, store it
    if (nextActionOnNamespace) {
        _defragmentationStates.at(uuid).queuedActions.push(*nextActionOnNamespace);
    }

    // Load next action, this will trigger phase change if needed
    _queueNextAction(opCtx, uuid, _defragmentationStates[uuid]);

    // Fulfill promise if needed
    if (_nextStreamingActionPromise) {
        auto nextStreamingAction = _nextStreamingAction(opCtx);
        if (nextStreamingAction) {
            _nextStreamingActionPromise.get().setWith([&] { return *nextStreamingAction; });
            _nextStreamingActionPromise = boost::none;
            return;
        }
    }
    // ... otherwise, just lower the counter
    --_concurrentStreamingOps;
}

void BalancerDefragmentationPolicyImpl::_transitionPhases(
    OperationContext* opCtx, const UUID& uuid, CollectionDefragmentationState& collectionInfo) {
    boost::optional<DefragmentationPhaseEnum> nextPhase;
    switch (collectionInfo.phase) {
        case DefragmentationPhaseEnum::kMergeChunks:
            if (MONGO_unlikely(skipPhaseTransition.shouldFail())) {
                nextPhase = DefragmentationPhaseEnum::kMergeChunks;
                break;
            }
            // TODO (SERVER-60459) Change to kMoveAndMergeChunks
            nextPhase = boost::none;
            break;
        case DefragmentationPhaseEnum::kMoveAndMergeChunks:
            // TODO (SERVER-60479) Change to kSplitChunks
            nextPhase = boost::none;
            break;
        case DefragmentationPhaseEnum::kSplitChunks:
            nextPhase = boost::none;
            break;
    }
    if (nextPhase) {
        collectionInfo.phase = nextPhase.get();
    } else {
        _clearDataSizeInformation(opCtx, uuid);
        _defragmentationStates.erase(uuid);
    }
    _persistPhaseUpdate(opCtx, nextPhase, uuid);
}

void BalancerDefragmentationPolicyImpl::_initializeCollectionState(WithLock,
                                                                   OperationContext* opCtx,
                                                                   const CollectionType& coll) {
    try {
        CollectionDefragmentationState newState;
        newState.nss = coll.getNss();
        newState.phase = coll.getDefragmentationPhase() ? coll.getDefragmentationPhase().get()
                                                        : DefragmentationPhaseEnum::kMergeChunks;
        newState.collectionShardKey = coll.getKeyPattern().toBSON();
        _persistPhaseUpdate(opCtx, newState.phase, coll.getUuid());
        auto [_, inserted] =
            _defragmentationStates.insert_or_assign(coll.getUuid(), std::move(newState));
        dassert(inserted);
    } catch (const DBException& e) {
        LOGV2_ERROR(6153101,
                    "Error while starting defragmentation on collection",
                    "namespace"_attr = coll.getNss(),
                    "uuid"_attr = coll.getUuid(),
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
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSON(
                "$unset" << BSON(CollectionType::kBalancerShouldMergeChunksFieldName
                                 << "" << CollectionType::kDefragmentationPhaseFieldName << ""))));
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
