/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/balancer/cluster_chunks_resize_policy_impl.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {

ChunkVersion getShardVersion(OperationContext* opCtx,
                             const ShardId& shardId,
                             const NamespaceString& nss) {
    auto cm = Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfo(opCtx, nss);
    return cm.getVersion(shardId);
}

bool isRetriableError(const Status& status) {
    if (ErrorCodes::isA<ErrorCategory::RetriableError>(status))
        return true;

    if (status == ErrorCodes::StaleConfig) {
        if (auto staleInfo = status.extraInfo<StaleConfigInfo>()) {
            // If the staleInfo error contains a "wanted" version, this means the donor shard which
            // returned this error has its versioning information up-to-date (as opposed to UNKNOWN)
            // and it couldn't find the chunk that the policy expected. Such a situation can only
            // arise as a result of manual split/merge/move concurrently with the policy.
            return !staleInfo->getVersionWanted();
        }
    }

    return false;
}

void applyResultToCollState(OperationContext* opCtx,
                            const Status& result,
                            CollectionState& collectionState,
                            std::function<std::vector<ActionRequestInfo>()> onSuccess,
                            std::function<std::vector<ActionRequestInfo>()> onRetriableError) {
    if (result.isOK()) {
        collectionState.actionCompleted(onSuccess());
        return;
    }

    if (result == ErrorCodes::StaleConfig) {
        if (auto staleInfo = result.extraInfo<StaleConfigInfo>()) {
            Grid::get(opCtx)
                ->catalogCache()
                ->invalidateShardOrEntireCollectionEntryForShardedCollection(
                    collectionState.getNss(),
                    staleInfo->getVersionWanted(),
                    staleInfo->getShardId());
        }
    }

    if (isRetriableError(result)) {
        collectionState.actionCompleted(onRetriableError());
    } else {
        LOGV2_WARNING(6417102,
                      "Marking collection chunks resize state as pending to be restarded",
                      "namespace"_attr = collectionState.getNss().ns(),
                      "err"_attr = result);
        collectionState.errorDetectedOnActionCompleted(opCtx);
    }
}

}  // namespace

CollectionState::CollectionState(const CollectionType& coll,
                                 std::vector<ActionRequestInfo>&& initialRequests,
                                 int defaultMaxChunksSizeBytes)
    : _nss(coll.getNss()),
      _uuid(coll.getUuid()),
      _keyPattern(coll.getKeyPattern().toBSON()),
      _epoch(coll.getEpoch()),
      _creationTime(coll.getTimestamp()),
      _maxChunkSizeBytes(coll.getMaxChunkSizeBytes().get_value_or(defaultMaxChunksSizeBytes)),
      _pendingRequests(std::move(initialRequests)),
      _numOutstandingActions(0),
      _restartRequested(false) {}

boost::optional<DefragmentationAction> CollectionState::popNextAction(OperationContext* opCtx) {
    boost::optional<DefragmentationAction> nextAction(boost::none);
    if (!_pendingRequests.empty()) {
        const auto& nextRequest = _pendingRequests.back();
        try {
            auto shardVersion = getShardVersion(opCtx, nextRequest.host, _nss);
            DefragmentationAction nextAction = nextRequest.splitPoints.empty()
                ? (DefragmentationAction)AutoSplitVectorInfo(nextRequest.host,
                                                             _nss,
                                                             _uuid,
                                                             shardVersion,
                                                             _keyPattern,
                                                             nextRequest.chunkRange.getMin(),
                                                             nextRequest.chunkRange.getMax(),
                                                             _maxChunkSizeBytes)
                : (DefragmentationAction)SplitInfoWithKeyPattern(nextRequest.host,
                                                                 _nss,
                                                                 shardVersion,
                                                                 nextRequest.chunkRange.getMin(),
                                                                 nextRequest.chunkRange.getMax(),
                                                                 nextRequest.splitPoints,
                                                                 _uuid,
                                                                 _keyPattern);
            _pendingRequests.pop_back();
            ++_numOutstandingActions;
            return nextAction;
        } catch (const DBException& e) {
            // May throw due to stepdown or collection no more available
            LOGV2(6417103,
                  "Failed to fetch collection version; marking chunks resize state as "
                  "pending to be restarted",
                  "namespace"_attr = _nss.ns(),
                  "error"_attr = redact(e));
            _requestRestart(opCtx);
        }
    }
    return boost::none;
}

ActionRequestInfo CollectionState::composeAutoSplitVectorRequest(const BSONObj& minKey,
                                                                 const BSONObj& maxKey,
                                                                 const ShardId& shard) {
    return ActionRequestInfo(ChunkRange(minKey, maxKey), shard, {} /*splitPoints*/);
}

ActionRequestInfo CollectionState::composeSplitChunkRequest(const BSONObj& minKey,
                                                            const BSONObj& maxKey,
                                                            const ShardId& shard,
                                                            SplitPoints splitPoints) {
    return ActionRequestInfo(ChunkRange(minKey, maxKey), shard, std::move(splitPoints));
}

void CollectionState::actionCompleted(std::vector<ActionRequestInfo>&& followUpRequests) {
    --_numOutstandingActions;
    for (auto& req : followUpRequests) {
        _pendingRequests.push_back(std::move(req));
    }
}

void CollectionState::errorDetectedOnActionCompleted(OperationContext* opCtx) {
    _requestRestart(opCtx);
    --_numOutstandingActions;
    _pendingRequests.clear();
}

bool CollectionState::restartNeeded() const {
    return _restartRequested && _numOutstandingActions == 0;
}

bool CollectionState::fullyProcessed() const {
    return _numOutstandingActions == 0 && _pendingRequests.empty() && !_restartRequested;
}

const NamespaceString& CollectionState::getNss() const {
    return _nss;
}

const UUID& CollectionState::getUuid() const {
    return _uuid;
}

void CollectionState::_requestRestart(OperationContext* opCtx) {
    // Invalidate the CollectionState object and the related entry in the cache.
    _restartRequested = true;
    Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(_nss);
}

ClusterChunksResizePolicyImpl::ClusterChunksResizePolicyImpl(
    const std::function<void()>& onStateUpdated)
    : _onStateUpdated(onStateUpdated) {}

ClusterChunksResizePolicyImpl::~ClusterChunksResizePolicyImpl() {
    stop();
}

SharedSemiFuture<void> ClusterChunksResizePolicyImpl::activate(OperationContext* opCtx,
                                                               int64_t defaultMaxChunksSizeBytes) {
    LOGV2(6417101,
          "Starting to split all oversized chunks in the cluster",
          "maxChunkSizeBytes"_attr = defaultMaxChunksSizeBytes);

    stdx::lock_guard<Latch> lk(_stateMutex);
    if (!_activeRequestPromise.is_initialized()) {
        invariant(!_unprocessedCollections && _collectionsBeingProcessed.empty());
        _defaultMaxChunksSizeBytes = defaultMaxChunksSizeBytes;
        invariant(_defaultMaxChunksSizeBytes > 0);

        DBDirectClient dbClient(opCtx);
        FindCommandRequest findCollectionsRequest{CollectionType::ConfigNS};
        findCollectionsRequest.setFilter(
            BSON(CollectionTypeBase::kChunksAlreadySplitForDowngradeFieldName
                 << BSON("$not" << BSON("$eq" << true))));
        _unprocessedCollections = dbClient.find(std::move(findCollectionsRequest));
        uassert(ErrorCodes::OperationFailed,
                "Failed to establish a cursor for accessing config.collections",
                _unprocessedCollections);

        _activeRequestPromise.emplace();
    }

    return _activeRequestPromise->getFuture();
}  // namespace mongo

bool ClusterChunksResizePolicyImpl::isActive() {
    stdx::lock_guard<Latch> lk(_stateMutex);
    return _activeRequestPromise.is_initialized();
}

void ClusterChunksResizePolicyImpl::stop() {
    {
        stdx::lock_guard<Latch> lk(_stateMutex);
        if (_activeRequestPromise.is_initialized()) {
            _collectionsBeingProcessed.clear();
            _unprocessedCollections = nullptr;
            _activeRequestPromise->setFrom(
                Status(ErrorCodes::Interrupted, "Chunk resizing task has been interrupted"));
            _activeRequestPromise = boost::none;
        }
    }
    _onStateUpdated();
}

StringData ClusterChunksResizePolicyImpl::getName() const {
    return StringData(kPolicyName);
}


boost::optional<DefragmentationAction> ClusterChunksResizePolicyImpl::getNextStreamingAction(
    OperationContext* opCtx) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    if (!_activeRequestPromise.is_initialized()) {
        return boost::none;
    }

    bool stateInspectionCompleted = false;
    while (!stateInspectionCompleted) {
        // Try to get the next action from the current subset of collections being processed.
        for (auto it = _collectionsBeingProcessed.begin();
             it != _collectionsBeingProcessed.end();) {
            auto& [collUuid, collState] = *it;
            if (collState.restartNeeded()) {
                auto refreshedCollState = _buildInitialStateFor(opCtx, collState.getNss());
                if (!refreshedCollState) {
                    // The collection does not longer exist - discard it
                    auto entryToErase = it;
                    it = std::next(it);
                    _collectionsBeingProcessed.erase(entryToErase);
                    continue;
                }
                if (refreshedCollState->getUuid() != collUuid) {
                    // the collection has been dropped and re-created since the creation of the
                    // element; re-insert the entry with an updated UUID (this operation will
                    // invalidate the iterator).
                    _collectionsBeingProcessed.erase(it);
                    _collectionsBeingProcessed.emplace(refreshedCollState->getUuid(),
                                                       std::move(*refreshedCollState));
                    it = _collectionsBeingProcessed.begin();
                    continue;
                }

                // update the current element
                collState = std::move(*refreshedCollState);
            }

            auto nextAction = collState.popNextAction(opCtx);
            if (nextAction.is_initialized()) {
                return nextAction;
            }

            // If an exception interrupted popNextAction() for the current collection and its state
            // may be immediately restarted, do not advance the iterator and use the next loop to do
            // it.
            if (!collState.restartNeeded()) {
                ++it;
            }
        }

        if (_collectionsBeingProcessed.size() < kMaxCollectionsBeingProcessed &&
            _unprocessedCollections->more()) {
            // Start processing a new collection
            auto nextDoc = _unprocessedCollections->next();
            CollectionType coll(nextDoc);
            auto initialCollState = _buildInitialStateFor(opCtx, coll);
            if (initialCollState) {
                _collectionsBeingProcessed.emplace(coll.getUuid(), std::move(*initialCollState));
            }
        } else {
            // The process is either completed, or the policy needs to receive new actions results
            // before continuing.
            stateInspectionCompleted = true;
        }
    }

    if (_collectionsBeingProcessed.empty() && !_unprocessedCollections->more()) {
        LOGV2(6417104, "Cluster chunks resize process completed. Clearing up internal state");
        PersistentTaskStore<CollectionType> store(CollectionType::ConfigNS);
        try {
            BSONObj allDocsQuery;
            store.update(opCtx,
                         allDocsQuery,
                         BSON("$unset" << BSON(
                                  CollectionType::kChunksAlreadySplitForDowngradeFieldName << "")),
                         WriteConcerns::kMajorityWriteConcernNoTimeout);
        } catch (const ExceptionFor<ErrorCodes::NoMatchingDocument>&) {
            // ignore
        } catch (const DBException& e) {
            LOGV2_WARNING(
                6417105,
                "Failed to clear persisted state while ending cluster chunks resize process",
                "err"_attr = redact(e));
        }
        _unprocessedCollections = nullptr;
        _activeRequestPromise->setFrom(Status::OK());
        _activeRequestPromise = boost::none;
    }

    return boost::none;
}  // namespace mongo

void ClusterChunksResizePolicyImpl::applyActionResult(OperationContext* opCtx,
                                                      const DefragmentationAction& action,
                                                      const DefragmentationActionResponse& result) {
    std::string completedNss;
    ScopeGuard onExitGuard([&] {
        if (!completedNss.empty()) {
            PersistentTaskStore<CollectionType> store(CollectionType::ConfigNS);
            try {
                store.update(
                    opCtx,
                    BSON(CollectionType::kNssFieldName << completedNss),
                    BSON("$set" << BSON(CollectionType::kChunksAlreadySplitForDowngradeFieldName
                                        << true)),
                    WriteConcerns::kMajorityWriteConcernNoTimeout);
            } catch (const DBException& e) {
                LOGV2(6417111,
                      "Could not mark collection as already processed by ClusterChunksResizePolicy",
                      "namespace"_attr = completedNss,
                      "error"_attr = redact(e));
            }
        }

        // Notify the reception of an operation outcome, even if it did not lead to a change of the
        // internal state.
        _onStateUpdated();
    });

    stdx::lock_guard<Latch> lk(_stateMutex);
    if (!_activeRequestPromise.is_initialized()) {
        return;
    }

    auto updatedEntryIt = stdx::visit(
        OverloadedVisitor{
            [&](const AutoSplitVectorInfo& act) mutable {
                auto& swSplitVectorResult = stdx::get<StatusWith<AutoSplitVectorResponse>>(result);
                auto match = _collectionsBeingProcessed.find(act.uuid);
                if (match != _collectionsBeingProcessed.end()) {
                    auto onSuccess = [&act, &swSplitVectorResult]() {
                        auto& splitVectorResult = swSplitVectorResult.getValue();
                        std::vector<ActionRequestInfo> followUpRequests;
                        auto& splitPoints = splitVectorResult.getSplitKeys();
                        if (!splitPoints.empty()) {
                            followUpRequests.push_back(CollectionState::composeSplitChunkRequest(
                                act.minKey, act.maxKey, act.shardId, splitPoints));
                            if (splitVectorResult.getContinuation()) {
                                followUpRequests.push_back(
                                    CollectionState::composeAutoSplitVectorRequest(
                                        splitPoints.back(), act.maxKey, act.shardId));
                            }
                        }
                        return followUpRequests;
                    };

                    auto onRetriableError = [&act]() {
                        std::vector<ActionRequestInfo> followUpRequests;
                        followUpRequests.push_back(CollectionState::composeAutoSplitVectorRequest(
                            act.minKey, act.maxKey, act.shardId));
                        return followUpRequests;
                    };

                    applyResultToCollState(opCtx,
                                           swSplitVectorResult.getStatus(),
                                           match->second,
                                           onSuccess,
                                           onRetriableError);
                }
                return match;
            },
            [&](const SplitInfoWithKeyPattern& act) {
                auto& splitResult = stdx::get<Status>(result);
                auto match = _collectionsBeingProcessed.find(act.uuid);
                if (match != _collectionsBeingProcessed.end()) {
                    auto onSuccess = []() { return std::vector<ActionRequestInfo>(); };

                    auto onRetriableError = [&act]() {
                        auto& splitInfo = act.info;
                        std::vector<ActionRequestInfo> followUpRequests;
                        followUpRequests.push_back(CollectionState::composeSplitChunkRequest(
                            splitInfo.minKey,
                            splitInfo.maxKey,
                            splitInfo.shardId,
                            std::move(splitInfo.splitKeys)));
                        return followUpRequests;
                    };

                    applyResultToCollState(
                        opCtx, splitResult, match->second, onSuccess, onRetriableError);
                }
                return match;
            },
            [this](const MergeInfo& _) {
                uasserted(ErrorCodes::BadValue, "Unexpected MergeInfo action type");
                return _collectionsBeingProcessed.end();
            },
            [this](const DataSizeInfo& _) {
                uasserted(ErrorCodes::BadValue, "Unexpected DataSizeInfo action type");
                return _collectionsBeingProcessed.end();
            },
            [this](const MigrateInfo& _) {
                uasserted(ErrorCodes::BadValue, "Unexpected MigrateInfo action type");
                return _collectionsBeingProcessed.end();
            },
        },
        action);

    if (updatedEntryIt == _collectionsBeingProcessed.end()) {
        return;
    }

    auto& [uuid, collectionState] = *updatedEntryIt;
    if (collectionState.fullyProcessed()) {
        LOGV2(6417107,
              "Collection chunks resize process completed",
              "namespace"_attr = collectionState.getNss().ns());
        completedNss = collectionState.getNss().ns();
        _collectionsBeingProcessed.erase(updatedEntryIt);

    } else if (collectionState.restartNeeded()) {
        auto nssToRefresh = collectionState.getNss();
        _collectionsBeingProcessed.erase(updatedEntryIt);
        auto refreshedState = _buildInitialStateFor(opCtx, nssToRefresh);
        if (refreshedState) {
            _collectionsBeingProcessed.emplace(refreshedState->getUuid(),
                                               std::move(*refreshedState));
        }
    }
}

boost::optional<CollectionState> ClusterChunksResizePolicyImpl::_buildInitialStateFor(
    OperationContext* opCtx, const NamespaceString& nss) {
    try {
        DBDirectClient dbClient(opCtx);
        FindCommandRequest findCollectionsRequest{CollectionType::ConfigNS};
        findCollectionsRequest.setFilter(BSON(CollectionType::kNssFieldName << nss.ns()));
        auto collObj = dbClient.findOne(std::move(findCollectionsRequest));
        CollectionType refreshedColl(std::move(collObj));
        return _buildInitialStateFor(opCtx, refreshedColl);
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // do nothing
    } catch (const DBException& e) {
        LOGV2_WARNING(6417106,
                      "Failed to initialize collection state in chunks resize policy",
                      "namespace"_attr = nss.ns(),
                      "error"_attr = redact(e));
    }

    return boost::none;
}

boost::optional<CollectionState> ClusterChunksResizePolicyImpl::_buildInitialStateFor(
    OperationContext* opCtx, const CollectionType& coll) {
    boost::optional<CollectionState> initialCollState(boost::none);
    try {
        DBDirectClient dbClient(opCtx);
        FindCommandRequest findDataRangesRequest{ChunkType::ConfigNS};
        findDataRangesRequest.setFilter(BSON(ChunkType::collectionUUID << coll.getUuid()));
        auto collDataRanges = dbClient.find(std::move(findDataRangesRequest));
        uassert(ErrorCodes::OperationFailed,
                "Failed to establish a cursor for accessing config.chunks",
                collDataRanges);
        std::vector<ActionRequestInfo> pendingActionsAtStart;
        while (collDataRanges->more()) {
            auto nextChunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
                collDataRanges->next(), coll.getEpoch(), coll.getTimestamp()));
            pendingActionsAtStart.push_back(CollectionState::composeAutoSplitVectorRequest(
                nextChunk.getMin(), nextChunk.getMax(), nextChunk.getShard()));
        }
        if (!pendingActionsAtStart.empty()) {
            initialCollState.emplace(
                coll, std::move(pendingActionsAtStart), _defaultMaxChunksSizeBytes);
        }
    } catch (const DBException& e) {
        LOGV2_WARNING(6417100,
                      "Failed to initialize collection state in chunks resize policy",
                      "namespace"_attr = coll.getNss().ns(),
                      "uuid"_attr = coll.getUuid(),
                      "error"_attr = redact(e));
    }
    return initialCollState;
}

}  // namespace mongo
