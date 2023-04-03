/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/balancer/auto_merger_policy.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_config_server_parameters_gen.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_feature_flags_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
const std::string kPolicyName{"AutoMergerPolicy"};
}  // namespace


void AutoMergerPolicy::enable() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_enabled) {
        _enabled = true;
        _init(lk);
    }
}

void AutoMergerPolicy::disable() {
    stdx::lock_guard<Latch> lk(_mutex);
    _enabled = false;
    _collectionsToMergePerShard.clear();
    _maxHistoryTimeCurrentRound = Timestamp(0, 0);
    _maxHistoryTimePreviousRound = Timestamp(0, 0);
}

bool AutoMergerPolicy::isEnabled() {
    return _enabled;
}

void AutoMergerPolicy::checkInternalUpdates() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!feature_flags::gAutoMerger.isEnabled(serverGlobalParams.featureCompatibility) ||
        !_enabled) {
        return;
    }
    _checkInternalUpdatesWithLock(lk);
}

StringData AutoMergerPolicy::getName() const {
    return kPolicyName;
}

boost::optional<BalancerStreamAction> AutoMergerPolicy::getNextStreamingAction(
    OperationContext* opCtx) {
    stdx::unique_lock<Latch> lk(_mutex);

    if (!feature_flags::gAutoMerger.isEnabled(serverGlobalParams.featureCompatibility) ||
        !_enabled) {
        return boost::none;
    }

    _checkInternalUpdatesWithLock(lk);

    bool applyThrottling = false;

    if (_firstAction) {
        try {
            _collectionsToMergePerShard = _getNamespacesWithMergeableChunksPerShard(opCtx);
        } catch (DBException& e) {
            e.addContext("Failed to fetch collections with mergeable chunks");
            throw;
        }
        applyThrottling = true;
        _firstAction = false;
    }

    // Issue at most 10 concurrent auto merge requests
    if (_outstandingActions >= MAX_NUMBER_OF_CONCURRENT_MERGE_ACTIONS) {
        return boost::none;
    }

    while (!_collectionsToMergePerShard.empty() ||
           !_rescheduledCollectionsToMergePerShard.empty()) {

        if (_collectionsToMergePerShard.empty()) {
            std::swap(_rescheduledCollectionsToMergePerShard, _collectionsToMergePerShard);
        }

        // Get next <shardId, collection> pair to merge
        for (auto it = _collectionsToMergePerShard.begin();
             it != _collectionsToMergePerShard.end();) {
            auto& [shardId, collections] = *it;
            if (collections.empty()) {
                it = _collectionsToMergePerShard.erase(it);
                applyThrottling = true;
                continue;
            }

            auto mergeAction = MergeAllChunksOnShardInfo{shardId, collections.back()};
            mergeAction.applyThrottling = applyThrottling;

            collections.pop_back();
            ++_outstandingActions;

            return boost::optional<BalancerStreamAction>(mergeAction);
        }
    }

    return boost::none;
}

void AutoMergerPolicy::applyActionResult(OperationContext* opCtx,
                                         const BalancerStreamAction& action,
                                         const BalancerStreamActionResponse& response) {
    stdx::unique_lock<Latch> lk(_mutex);

    const ScopeGuard decrementNumberOfOutstandingActions([&] {
        --_outstandingActions;
        if (_outstandingActions < MAX_NUMBER_OF_CONCURRENT_MERGE_ACTIONS) {
            _onStateUpdated();
        }
    });

    const auto& mergeAction = stdx::get<MergeAllChunksOnShardInfo>(action);

    const auto& swResponse = stdx::get<StatusWith<NumMergedChunks>>(response);
    if (swResponse.isOK()) {
        auto numMergedChunks = swResponse.getValue();
        if (numMergedChunks > 0) {
            // Reschedule auto-merge for <shard, nss> until no merge has been performed
            _rescheduledCollectionsToMergePerShard[mergeAction.shardId].push_back(mergeAction.nss);
        }
        return;
    }

    const auto status = std::move(swResponse.getStatus());
    if (status.code() == ErrorCodes::ConflictingOperationInProgress) {
        // Reschedule auto-merge for <shard, nss> because commit overlapped with other chunk ops
        _rescheduledCollectionsToMergePerShard[mergeAction.shardId].push_back(mergeAction.nss);
    } else {
        // Reset the history window to consider during next round because chunk merges may have
        // been potentially missed due to an unexpected error
        _maxHistoryTimeCurrentRound = _maxHistoryTimePreviousRound;
        LOGV2_DEBUG(7312600,
                    1,
                    "Hit unexpected error while automerging chunks",
                    "shard"_attr = mergeAction.shardId,
                    "nss"_attr = mergeAction.nss,
                    "error"_attr = redact(status));
    }
}

void AutoMergerPolicy::_init(WithLock lk) {
    _maxHistoryTimePreviousRound = _maxHistoryTimeCurrentRound;
    _intervalTimer.reset();
    _collectionsToMergePerShard.clear();
    _firstAction = true;
    _onStateUpdated();
}

void AutoMergerPolicy::_checkInternalUpdatesWithLock(WithLock lk) {
    if (!_enabled || !_collectionsToMergePerShard.empty() || _outstandingActions) {
        return;
    }

    // Trigger Automerger every `autoMergerIntervalSecs` seconds
    if (_intervalTimer.seconds() > autoMergerIntervalSecs) {
        _init(lk);
    }
}

std::map<ShardId, std::vector<NamespaceString>>
AutoMergerPolicy::_getNamespacesWithMergeableChunksPerShard(OperationContext* opCtx) {
    std::map<ShardId, std::vector<NamespaceString>> collectionsToMerge;

    const auto& shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    for (const auto& shard : shardIds) {
        // Build an aggregation pipeline to get the collections with mergeable chunks placed on a
        // specific shard
        Pipeline::SourceContainer stages;

        auto expCtx = make_intrusive<ExpressionContext>(opCtx, nullptr, ChunkType::ConfigNS);
        StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
        resolvedNamespaces[ChunkType::ConfigNS.coll()] = {ChunkType::ConfigNS,
                                                          std::vector<BSONObj>()};
        resolvedNamespaces[CollectionType::ConfigNS.coll()] = {CollectionType::ConfigNS,
                                                               std::vector<BSONObj>()};
        expCtx->setResolvedNamespaces(resolvedNamespaces);

        // 1. Match all collections where `automerge` is enabled and `defragmentation` is disabled
        // {
        //     $match : {
        //         enableAutoMerge : { $ne : false },
        //         defragmentCollection : { $ne : true }
        //     }
        // }
        stages.emplace_back(DocumentSourceMatch::create(
            BSON(CollectionType::kEnableAutoMergeFieldName
                 << BSON("$ne" << false) << CollectionType::kDefragmentCollectionFieldName
                 << BSON("$ne" << true)),
            expCtx));

        // 2. Lookup stage to get at most 2 mergeable chunk per collection
        // {
        //     $lookup : {
        //         from : "chunks",
        //         localField : "uuid",
        //         foreignField : "collectionUUID",
        //         pipeline : [
        //             {
        //                 $match : {
        //                     shard : <shard>,
        //                     onCurrentShardSince : { $lt : <_maxHistoryTimeCurrentRound>,
        //                                             $gte : <_maxHistoryTimePreviousRound> }
        //                 }
        //             },
        //             {
        //                 $limit : 1
        //             }
        //         ],
        //         as : "chunks"
        //     }
        // }
        const auto _maxHistoryTimeCurrentRound =
            ShardingCatalogManager::getOldestTimestampSupportedForSnapshotHistory(opCtx);

        stages.emplace_back(DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON(
                     "from"
                     << ChunkType::ConfigNS.coll() << "localField" << CollectionType::kUuidFieldName
                     << "foreignField" << ChunkType::collectionUUID() << "pipeline"
                     << BSON_ARRAY(BSON("$match" << BSON(
                                            ChunkType::shard(shard.toString())
                                            << ChunkType::onCurrentShardSince()
                                            << BSON("$lt" << _maxHistoryTimeCurrentRound << "$gte"
                                                          << _maxHistoryTimePreviousRound)))
                                   << BSON("$limit" << 1))
                     << "as"
                     << "chunks"))
                .firstElement(),
            expCtx));

        // 3. Unwind stage to get the list of collections with mergeable chunks
        stages.emplace_back(
            DocumentSourceUnwind::createFromBson(BSON("$unwind" << BSON("path"
                                                                        << "$chunks"))
                                                     .firstElement(),
                                                 expCtx));

        auto pipeline = Pipeline::create(std::move(stages), expCtx);
        auto aggRequest =
            AggregateCommandRequest(CollectionType::ConfigNS, pipeline->serializeToBson());
        aggRequest.setReadConcern(
            repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern).toBSONInner());

        DBDirectClient client(opCtx);
        auto cursor = uassertStatusOKWithContext(
            DBClientCursor::fromAggregationRequest(
                &client, aggRequest, true /* secondaryOk */, true /* useExhaust */),
            "Failed to establish a cursor for aggregation");

        while (cursor->more()) {
            const auto doc = cursor->nextSafe();
            const auto nss = NamespaceString(doc.getStringField(CollectionType::kNssFieldName));
            collectionsToMerge[shard].push_back(nss);
        }
    }

    return collectionsToMerge;
}

}  // namespace mongo
