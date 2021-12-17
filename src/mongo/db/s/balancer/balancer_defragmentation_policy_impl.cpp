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

namespace {

MONGO_FAIL_POINT_DEFINE(beforeTransitioningDefragmentationPhase);

ChunkVersion getShardVersion(OperationContext* opCtx, const ShardId& shardId, const UUID& uuid) {
    auto coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, uuid);

    auto chunkVector = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getChunks(
        opCtx,
        BSON(ChunkType::collectionUUID()
             << coll.getUuid() << ChunkType::shard(shardId.toString())) /*query*/,
        BSON(ChunkType::lastmod << -1) /*sort*/,
        1 /*limit*/,
        nullptr /*opTime*/,
        coll.getEpoch(),
        coll.getTimestamp(),
        repl::ReadConcernLevel::kLocalReadConcern,
        boost::none));
    return chunkVector.front().getVersion();
}

class MergeChunksPhase : public DefragmentationPhase {
public:
    static std::unique_ptr<MergeChunksPhase> build(OperationContext* opCtx,
                                                   const CollectionType& coll) {
        auto collectionChunks = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getChunks(
            opCtx,
            BSON(ChunkType::collectionUUID() << coll.getUuid()) /*query*/,
            BSON(ChunkType::min() << 1) /*sort*/,
            boost::none /*limit*/,
            nullptr /*opTime*/,
            coll.getEpoch(),
            coll.getTimestamp(),
            repl::ReadConcernLevel::kLocalReadConcern,
            boost::none));

        const auto collectionZones = [&] {
            ZoneInfo zones;
            uassertStatusOK(
                ZoneInfo::addTagsFromCatalog(opCtx, coll.getNss(), coll.getKeyPattern(), zones));
            return zones;
        }();

        auto areConsecutive = [&](const ChunkType& firstChunk,
                                  const ChunkType& secondChunk) -> bool {
            return firstChunk.getShard() == secondChunk.getShard() &&
                collectionZones.getZoneForChunk(firstChunk.getRange()) ==
                collectionZones.getZoneForChunk(secondChunk.getRange()) &&
                SimpleBSONObjComparator::kInstance.evaluate(firstChunk.getMax() ==
                                                            secondChunk.getMin());
        };

        std::map<ShardId, PendingActions> pendingActionsByShards;
        // Find ranges of chunks; for single-chunk ranges, request DataSize; for multi-range, issue
        // merge
        while (!collectionChunks.empty()) {
            auto upperRangeBound = std::prev(collectionChunks.cend());
            auto lowerRangeBound = upperRangeBound;
            while (lowerRangeBound != collectionChunks.cbegin() &&
                   areConsecutive(*std::prev(lowerRangeBound), *lowerRangeBound)) {
                --lowerRangeBound;
            }
            if (lowerRangeBound != upperRangeBound) {
                pendingActionsByShards[upperRangeBound->getShard()].rangesToMerge.emplace_back(
                    lowerRangeBound->getMin(), upperRangeBound->getMax());
            } else {
                if (!upperRangeBound->getEstimatedSizeBytes().has_value()) {
                    pendingActionsByShards[upperRangeBound->getShard()]
                        .rangesWithoutDataSize.emplace_back(upperRangeBound->getMin(),
                                                            upperRangeBound->getMax());
                }
            }
            collectionChunks.erase(lowerRangeBound, std::next(upperRangeBound));
        }
        return std::unique_ptr<MergeChunksPhase>(
            new MergeChunksPhase(coll.getNss(),
                                 coll.getUuid(),
                                 coll.getKeyPattern().toBSON(),
                                 std::move(pendingActionsByShards)));
    }

    DefragmentationPhaseEnum getType() const override {
        return DefragmentationPhaseEnum::kMergeChunks;
    }

    boost::optional<DefragmentationAction> popNextStreamableAction(
        OperationContext* opCtx) override {
        boost::optional<DefragmentationAction> nextAction = boost::none;
        if (!_pendingActionsByShards.empty()) {
            // TODO (SERVER-61635) improve fairness if needed
            auto& [shardId, pendingActions] = *_pendingActionsByShards.begin();
            auto shardVersion = getShardVersion(opCtx, shardId, _uuid);

            if (pendingActions.rangesWithoutDataSize.size() > pendingActions.rangesToMerge.size()) {
                const auto& rangeToMeasure = pendingActions.rangesWithoutDataSize.back();
                nextAction = boost::optional<DefragmentationAction>(DataSizeInfo(
                    shardId, _nss, _uuid, rangeToMeasure, shardVersion, _shardKey, false));
                pendingActions.rangesWithoutDataSize.pop_back();
            } else if (!pendingActions.rangesToMerge.empty()) {
                const auto& rangeToMerge = pendingActions.rangesToMerge.back();
                nextAction = boost::optional<DefragmentationAction>(
                    MergeInfo(shardId, _nss, _uuid, shardVersion, rangeToMerge));
                pendingActions.rangesToMerge.pop_back();
            }
            if (nextAction.has_value()) {
                ++_outstandingActions;
                if (pendingActions.rangesToMerge.empty() &&
                    pendingActions.rangesWithoutDataSize.empty()) {
                    _pendingActionsByShards.erase(shardId);
                }
            }
        }
        return nextAction;
    }

    boost::optional<MigrateInfo> popNextMigration(
        const stdx::unordered_set<ShardId>& unavailableShards) override {
        return boost::none;
    }

    void applyActionResult(OperationContext* opCtx,
                           const DefragmentationAction& action,
                           const DefragmentationActionResponse& response) override {
        stdx::visit(
            visit_helper::Overloaded{
                [&](const MergeInfo& mergeAction) {
                    auto& mergeResponse = stdx::get<Status>(response);
                    auto& shardingPendingActions = _pendingActionsByShards[mergeAction.shardId];
                    if (mergeResponse.isOK()) {
                        shardingPendingActions.rangesWithoutDataSize.emplace_back(
                            mergeAction.chunkRange);
                    } else {
                        shardingPendingActions.rangesToMerge.emplace_back(mergeAction.chunkRange);
                    }
                },
                [&](const DataSizeInfo& dataSizeAction) {
                    auto& dataSizeResponse = stdx::get<StatusWith<DataSizeResponse>>(response);
                    if (dataSizeResponse.isOK()) {
                        ChunkType chunk(dataSizeAction.uuid,
                                        dataSizeAction.chunkRange,
                                        dataSizeAction.version,
                                        dataSizeAction.shardId);
                        auto catalogManager = ShardingCatalogManager::get(opCtx);
                        catalogManager->setChunkEstimatedSize(
                            opCtx,
                            chunk,
                            dataSizeResponse.getValue().sizeBytes,
                            ShardingCatalogClient::kMajorityWriteConcern);
                    } else {
                        auto& shardingPendingActions =
                            _pendingActionsByShards[dataSizeAction.shardId];
                        shardingPendingActions.rangesWithoutDataSize.emplace_back(
                            dataSizeAction.chunkRange);
                    }
                },
                [&](const AutoSplitVectorInfo& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const SplitInfoWithKeyPattern& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const EndOfActionStream& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                }},
            action);
        --_outstandingActions;
    }

    bool isComplete() const override {
        return _pendingActionsByShards.empty() && _outstandingActions == 0;
    }

private:
    struct PendingActions {
        std::vector<ChunkRange> rangesToMerge;
        std::vector<ChunkRange> rangesWithoutDataSize;
    };
    MergeChunksPhase(const NamespaceString& nss,
                     const UUID& uuid,
                     const BSONObj& shardKey,
                     std::map<ShardId, PendingActions>&& pendingActionsByShards)
        : _nss(nss),
          _uuid(uuid),
          _shardKey(shardKey),
          _pendingActionsByShards(std::move(pendingActionsByShards)) {}

    const NamespaceString _nss;
    const UUID _uuid;
    const BSONObj _shardKey;
    std::map<ShardId, PendingActions> _pendingActionsByShards;
    size_t _outstandingActions{0};
};

}  // namespace

void BalancerDefragmentationPolicyImpl::refreshCollectionDefragmentationStatus(
    OperationContext* opCtx, const CollectionType& coll) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    const auto& uuid = coll.getUuid();
    if (coll.getBalancerShouldMergeChunks() && !_defragmentationStates.contains(uuid)) {
        _initializeCollectionState(lk, opCtx, coll);
        // Fulfill pending promise of actionable operation if needed
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
        _persistPhaseUpdate(opCtx, DefragmentationPhaseEnum::kFinished, uuid);
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
    for (auto it = _defragmentationStates.begin(); it != _defragmentationStates.end();) {
        auto& currentCollectionDefragmentationState = it->second;
        // Phase transition if needed
        auto coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, it->first);
        while (currentCollectionDefragmentationState &&
               currentCollectionDefragmentationState->isComplete()) {
            currentCollectionDefragmentationState = _transitionPhases(
                opCtx, coll, _getNextPhase(currentCollectionDefragmentationState->getType()));
        }
        if (!currentCollectionDefragmentationState) {
            it = _defragmentationStates.erase(it, std::next(it));
            continue;
        }
        // Get next action
        auto nextAction = currentCollectionDefragmentationState->popNextStreamableAction(opCtx);
        if (nextAction) {
            return nextAction;
        }
        ++it;
    }

    boost::optional<DefragmentationAction> noAction;
    if (_streamClosed) {
        noAction = boost::optional<EndOfActionStream>();
    }
    return noAction;
}

void BalancerDefragmentationPolicyImpl::acknowledgeMergeResult(OperationContext* opCtx,
                                                               MergeInfo action,
                                                               const Status& result) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    // Check if collection defragmentation has been canceled
    if (!_defragmentationStates.contains(action.uuid)) {
        return;
    }

    _defragmentationStates[action.uuid]->applyActionResult(opCtx, action, result);
    _processEndOfAction(lk, opCtx);
}

void BalancerDefragmentationPolicyImpl::acknowledgeDataSizeResult(
    OperationContext* opCtx, DataSizeInfo action, const StatusWith<DataSizeResponse>& result) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    // Check if collection defragmentation has been canceled
    if (!_defragmentationStates.contains(action.uuid)) {
        return;
    }

    _defragmentationStates[action.uuid]->applyActionResult(opCtx, action, result);
    _processEndOfAction(lk, opCtx);
}

void BalancerDefragmentationPolicyImpl::acknowledgeAutoSplitVectorResult(
    OperationContext* opCtx, AutoSplitVectorInfo action, const StatusWith<SplitPoints>& result) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    // Check if collection defragmentation has been canceled
    if (!_defragmentationStates.contains(action.uuid)) {
        return;
    }

    _defragmentationStates[action.uuid]->applyActionResult(opCtx, action, result);
    _processEndOfAction(lk, opCtx);
}

void BalancerDefragmentationPolicyImpl::acknowledgeSplitResult(OperationContext* opCtx,
                                                               SplitInfoWithKeyPattern action,
                                                               const Status& result) {
    stdx::lock_guard<Latch> lk(_streamingMutex);
    // Check if collection defragmentation has been canceled
    if (!_defragmentationStates.contains(action.uuid)) {
        return;
    }

    _defragmentationStates[action.uuid]->applyActionResult(opCtx, action, result);
    _processEndOfAction(lk, opCtx);
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

void BalancerDefragmentationPolicyImpl::_processEndOfAction(WithLock, OperationContext* opCtx) {
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

std::unique_ptr<DefragmentationPhase> BalancerDefragmentationPolicyImpl::_transitionPhases(
    OperationContext* opCtx,
    const CollectionType& coll,
    DefragmentationPhaseEnum nextPhase,
    bool shouldPersistPhase) {
    beforeTransitioningDefragmentationPhase.pauseWhileSet();
    std::unique_ptr<DefragmentationPhase> nextPhaseObject(nullptr);
    try {
        switch (nextPhase) {
            case DefragmentationPhaseEnum::kMergeChunks:
                nextPhaseObject = MergeChunksPhase::build(opCtx, coll);
                break;
            case DefragmentationPhaseEnum::kMoveAndMergeChunks:
                // TODO (SERVER-60459) build phase 2
                break;
            case DefragmentationPhaseEnum::kSplitChunks:
                // TODO (SERVER-60479) build phase 3
                break;
            case DefragmentationPhaseEnum::kFinished:
                _clearDataSizeInformation(opCtx, coll.getUuid());
                break;
        }
        if (shouldPersistPhase) {
            _persistPhaseUpdate(opCtx, nextPhase, coll.getUuid());
        }
    } catch (const DBException& e) {
        LOGV2_ERROR(6153101,
                    "Error while building defragmentation phase on collection",
                    "namespace"_attr = coll.getNss(),
                    "uuid"_attr = coll.getUuid(),
                    "phase"_attr = nextPhase,
                    "error"_attr = e);
    }
    return nextPhaseObject;
}

void BalancerDefragmentationPolicyImpl::_initializeCollectionState(WithLock,
                                                                   OperationContext* opCtx,
                                                                   const CollectionType& coll) {
    auto phaseToBuild = coll.getDefragmentationPhase() ? coll.getDefragmentationPhase().get()
                                                       : DefragmentationPhaseEnum::kMergeChunks;
    auto collectionPhase = _transitionPhases(
        opCtx, coll, phaseToBuild, !coll.getDefragmentationPhase().is_initialized());
    while (collectionPhase && collectionPhase->isComplete()) {
        collectionPhase = _transitionPhases(opCtx, coll, _getNextPhase(collectionPhase->getType()));
    }
    if (collectionPhase) {
        auto [_, inserted] =
            _defragmentationStates.insert_or_assign(coll.getUuid(), std::move(collectionPhase));
        dassert(inserted);
    }
}

DefragmentationPhaseEnum BalancerDefragmentationPolicyImpl::_getNextPhase(
    DefragmentationPhaseEnum currentPhase) {
    switch (currentPhase) {
        case DefragmentationPhaseEnum::kMergeChunks:
            // TODO (SERVER-60459) change to kMoveAndMergeChunks
            return DefragmentationPhaseEnum::kFinished;
        case DefragmentationPhaseEnum::kMoveAndMergeChunks:
            // TODO (SERVER-60479) change to kSplitChunks
            return DefragmentationPhaseEnum::kFinished;
        case DefragmentationPhaseEnum::kSplitChunks:
            return DefragmentationPhaseEnum::kFinished;
        default:
            uasserted(ErrorCodes::BadValue, "Invalid phase transition");
    }
}

void BalancerDefragmentationPolicyImpl::_persistPhaseUpdate(OperationContext* opCtx,
                                                            DefragmentationPhaseEnum phase,
                                                            const UUID& uuid) {
    DBDirectClient dbClient(opCtx);
    write_ops::UpdateCommandRequest updateOp(CollectionType::ConfigNS);
    updateOp.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON(CollectionType::kUuidFieldName << uuid));
        if (phase != DefragmentationPhaseEnum::kFinished) {
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                BSON("$set" << BSON(CollectionType::kDefragmentationPhaseFieldName
                                    << DefragmentationPhase_serializer(phase)))));
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
