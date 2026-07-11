// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/balancer/auto_merger_policy.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_config_server_parameters_gen.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
const std::string kPolicyName{"AutoMergerPolicy"};

void logAutoMergeChange(OperationContext* opCtx, const std::string& what) {
    auto catalogManager = ShardingCatalogManager::get(opCtx);
    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           what,
                                           NamespaceString::kEmpty,
                                           BSONObj(),
                                           WriteConcernOptions(),
                                           std::move(catalogManager->localConfigShard()),
                                           catalogManager->localCatalogClient());
}
}  // namespace


void AutoMergerPolicy::enable(OperationContext* opCtx) {
    std::lock_guard<std::mutex> lk(_mutex);
    if (!_enabled) {
        _enabled = true;
        logAutoMergeChange(opCtx, "autoMerge.enable");
        _init(opCtx, lk);
    }
}

void AutoMergerPolicy::disable(OperationContext* opCtx) {
    std::lock_guard<std::mutex> lk(_mutex);
    if (_enabled) {
        _enabled = false;
        _collectionsToMergePerShard.clear();
        _maxHistoryTimeCurrentRound = Timestamp(0, 0);
        _maxHistoryTimePreviousRound = Timestamp(0, 0);
        logAutoMergeChange(opCtx, "autoMerge.disable");
    }
}

bool AutoMergerPolicy::isEnabled() {
    std::lock_guard<std::mutex> lk(_mutex);
    return _enabled;
}

void AutoMergerPolicy::checkInternalUpdates(OperationContext* opCtx) {
    std::lock_guard<std::mutex> lk(_mutex);
    if (!_enabled) {
        return;
    }
    _checkInternalUpdatesWithLock(opCtx, lk);
}

std::string_view AutoMergerPolicy::getName() const {
    return kPolicyName;
}

boost::optional<BalancerStreamAction> AutoMergerPolicy::getNextStreamingAction(
    OperationContext* opCtx) {
    std::unique_lock<std::mutex> lk(_mutex);

    if (!_enabled) {
        return boost::none;
    }

    _checkInternalUpdatesWithLock(opCtx, lk);

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

    if (_withinRound) {
        // All mergeable chunks have been processed for the current round.
        _withinRound = false;
        logAutoMergeChange(opCtx, "autoMerge.end");
    }
    return boost::none;
}

void AutoMergerPolicy::applyActionResult(OperationContext* opCtx,
                                         const BalancerStreamAction& action,
                                         const BalancerStreamActionResponse& response) {
    std::unique_lock<std::mutex> lk(_mutex);

    const ScopeGuard decrementNumberOfOutstandingActions([&] {
        --_outstandingActions;
        if (_outstandingActions < MAX_NUMBER_OF_CONCURRENT_MERGE_ACTIONS) {
            _onStateUpdated();
        }
    });

    const auto& mergeAction = get<MergeAllChunksOnShardInfo>(action);

    const auto& swResponse = get<StatusWith<NumMergedChunks>>(response);
    if (swResponse.isOK()) {
        auto numMergedChunks = swResponse.getValue();
        if (numMergedChunks > 0) {
            // Reschedule auto-merge for <shard, nss> until no merge has been performed
            _rescheduledCollectionsToMergePerShard[mergeAction.shardId].push_back(mergeAction.nss);
        }
        return;
    }

    const auto status = swResponse.getStatus();
    if (status.code() == ErrorCodes::ConflictingOperationInProgress) {
        // Reschedule auto-merge for <shard, nss> because commit overlapped with other chunk ops
        _rescheduledCollectionsToMergePerShard[mergeAction.shardId].push_back(mergeAction.nss);
    } else if (status.code() == ErrorCodes::KeyPatternShorterThanBound || status.code() == 16634) {
        LOGV2_WARNING(7805201,
                      "Auto-merger skipping namespace due to misconfigured zones",
                      "namespace"_attr = mergeAction.nss,
                      "error"_attr = redact(status));
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

void AutoMergerPolicy::_init(OperationContext* opCtx, WithLock lk) {
    _maxHistoryTimePreviousRound = _maxHistoryTimeCurrentRound;
    _intervalTimer.reset();
    _collectionsToMergePerShard.clear();
    _firstAction = true;
    _withinRound = true;
    _outstandingActions = 0;
    _onStateUpdated();
    logAutoMergeChange(opCtx, "autoMerge.start");
}

void AutoMergerPolicy::_checkInternalUpdatesWithLock(OperationContext* opCtx, WithLock lk) {
    if (!_enabled || !_collectionsToMergePerShard.empty() || _outstandingActions) {
        return;
    }

    // Trigger Automerger every `autoMergerIntervalSecs` seconds
    if (_intervalTimer.seconds() > autoMergerIntervalSecs.load()) {
        _init(opCtx, lk);
    }
}

std::map<ShardId, std::vector<NamespaceString>>
AutoMergerPolicy::_getNamespacesWithMergeableChunksPerShard(OperationContext* opCtx) {
    std::map<ShardId, std::vector<NamespaceString>> collectionsToMerge;
    DBDirectClient client(opCtx);

    // First, get the list of all the shards of the cluster.
    // Using the DbClient to avoid accessing the ShardRegistry while holding the mutex.
    std::vector<ShardId> shardIds;
    {
        auto cursor = client.find(FindCommandRequest(NamespaceString::kConfigsvrShardsNamespace));
        while (cursor->more()) {
            const auto& doc = cursor->nextSafe();
            shardIds.push_back(doc.getField(ShardType::name()).str());
        }
    }

    for (const auto& shard : shardIds) {
        // Build an aggregation pipeline to get the collections with mergeable chunks placed on a
        // specific shard

        ResolvedNamespaceMap resolvedNamespaces;
        resolvedNamespaces[NamespaceString::kConfigsvrChunksNamespace] = {
            NamespaceString::kConfigsvrChunksNamespace, std::vector<BSONObj>()};
        resolvedNamespaces[NamespaceString::kConfigsvrCollectionsNamespace] = {
            NamespaceString::kConfigsvrCollectionsNamespace, std::vector<BSONObj>()};

        DocumentSourceContainer stages;
        auto expCtx = ExpressionContextBuilder{}
                          .opCtx(opCtx)
                          .ns(NamespaceString::kConfigsvrChunksNamespace)
                          .resolvedNamespace(std::move(resolvedNamespaces))
                          .build();

        // 1. Match all collections where `automerge` is enabled and `defragmentation` is disabled
        // {
        //     $match : {
        //         enableAutoMerge : { $ne : false },
        //         defragmentCollection : { $ne : true },
        //         unsplittable : { $ne : true }
        //     }
        // }
        stages.emplace_back(DocumentSourceMatch::create(
            BSON(CollectionType::kEnableAutoMergeFieldName
                 << BSON("$ne" << false) << CollectionType::kDefragmentCollectionFieldName
                 << BSON("$ne" << true) << CollectionType::kUnsplittableFieldName
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
                     "from" << NamespaceString::kConfigsvrChunksNamespace.coll() << "localField"
                            << CollectionType::kUuidFieldName << "foreignField"
                            << ChunkType::collectionUUID() << "pipeline"
                            << BSON_ARRAY(
                                   BSON("$match" << BSON(
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
        stages.emplace_back(DocumentSourceUnwind::createFromBson(
            BSON("$unwind" << BSON("path" << "$chunks")).firstElement(), expCtx));

        auto pipeline = Pipeline::create(std::move(stages), expCtx);
        auto aggRequest = AggregateCommandRequest(NamespaceString::kConfigsvrCollectionsNamespace,
                                                  pipeline->serializeToBson());
        aggRequest.setReadConcern(repl::ReadConcernArgs::kMajority);

        auto cursor = uassertStatusOKWithContext(
            DBClientCursor::fromAggregationRequest(
                &client, aggRequest, true /* secondaryOk */, true /* useExhaust */),
            "Failed to establish a cursor for aggregation");

        while (cursor->more()) {
            const auto doc = cursor->nextSafe();
            const auto nss =
                NamespaceStringUtil::deserialize(boost::none,
                                                 doc.getStringField(CollectionType::kNssFieldName),
                                                 SerializationContext::stateDefault());
            collectionsToMerge[shard].push_back(nss);
        }
    }

    return collectionsToMerge;
}

}  // namespace mongo
