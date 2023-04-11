/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include <set>

#include "mongo/client/read_preference.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_constraints.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangRefineCollectionShardKeyBeforeUpdatingChunks);
MONGO_FAIL_POINT_DEFINE(hangRefineCollectionShardKeyBeforeCommit);

void triggerFireAndForgetShardRefreshes(OperationContext* opCtx,
                                        Shard* configShard,
                                        ShardingCatalogClient* catalogClient,
                                        const CollectionType& coll) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto allShards = uassertStatusOK(catalogClient->getAllShards(
                                               opCtx, repl::ReadConcernLevel::kLocalReadConcern))
                               .value;
    for (const auto& shardEntry : allShards) {
        const auto query = BSON(ChunkType::collectionUUID
                                << coll.getUuid() << ChunkType::shard(shardEntry.getName()));

        const auto chunk = uassertStatusOK(configShard->exhaustiveFindOnConfig(
                                               opCtx,
                                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                               repl::ReadConcernLevel::kLocalReadConcern,
                                               ChunkType::ConfigNS,
                                               query,
                                               BSONObj(),
                                               1LL))
                               .docs;

        invariant(chunk.size() == 0 || chunk.size() == 1);

        if (chunk.size() == 1) {
            const auto shard =
                uassertStatusOK(shardRegistry->getShard(opCtx, shardEntry.getName()));

            // This is a best-effort attempt to refresh the shard 'shardEntry'. Fire and forget an
            // asynchronous '_flushRoutingTableCacheUpdates' request.
            shard->runFireAndForgetCommand(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                DatabaseName::kAdmin.toString(),
                BSON("_flushRoutingTableCacheUpdates" << coll.getNss().ns()));
        }
    }
}

}  // namespace

// Returns the pipeline updates to be used for updating a refined collection's chunk and tag
// documents.
//
// The chunk updates:
// [{$set: {
//    min: {$arrayToObject: {$concatArrays: [
//      {$objectToArray: "$min"},
//      {$literal: [{k: <new_sk_suffix_1>, v: MinKey}, ...]},
//    ]}},
//    max: {$let: {
//      vars: {maxAsArray: {$objectToArray: "$max"}},
//      in: {
//        {$arrayToObject: {$concatArrays: [
//          "$$maxAsArray",
//          {$cond: {
//            if: {$allElementsTrue: [{$map: {
//              input: "$$maxAsArray",
//              in: {$eq: [{$type: "$$this.v"}, "maxKey"]},
//            }}]},
//            then: {$literal: [{k: <new_sk_suffix_1>, v: MaxKey}, ...]},
//            else: {$literal: [{k: <new_sk_suffix_1>, v: MinKey}, ...]},
//          }}
//        ]}}
//      }
//    }}
//  }},
//  {$unset: "jumbo"}]
//
// The tag update:
// [{$set: {
//    min: {$arrayToObject: {$concatArrays: [
//      {$objectToArray: "$min"},
//      {$literal: [{k: <new_sk_suffix_1>, v: MinKey}, ...]},
//    ]}},
//    max: {$let: {
//      vars: {maxAsArray: {$objectToArray: "$max"}},
//      in: {
//        {$arrayToObject: {$concatArrays: [
//          "$$maxAsArray",
//          {$cond: {
//            if: {$allElementsTrue: [{$map: {
//              input: "$$maxAsArray",
//              in: {$eq: [{$type: "$$this.v"}, "maxKey"]},
//            }}]},
//            then: {$literal: [{k: <new_sk_suffix_1>, v: MaxKey}, ...]},
//            else: {$literal: [{k: <new_sk_suffix_1>, v: MinKey}, ...]},
//          }}
//        ]}}
//      }
//    }}
//  }}]
std::pair<std::vector<BSONObj>, std::vector<BSONObj>> makeChunkAndTagUpdatesForRefine(
    const BSONObj& newShardKeyFields) {
    // Make the $literal objects used in the $set below to add new fields to the boundaries of the
    // existing chunks and tags that may include "." characters.
    //
    // Example: oldKeyDoc = {a: 1}
    //          newKeyDoc = {a: 1, b: 1, "c.d": 1}
    //          literalMinObject = {$literal: [{k: "b", v: MinKey}, {k: "c.d", v: MinKey}]}
    //          literalMaxObject = {$literal: [{k: "b", v: MaxKey}, {k: "c.d", v: MaxKey}]}
    BSONArrayBuilder literalMinObjectBuilder, literalMaxObjectBuilder;
    for (const auto& fieldElem : newShardKeyFields) {
        literalMinObjectBuilder.append(
            BSON("k" << fieldElem.fieldNameStringData() << "v" << MINKEY));
        literalMaxObjectBuilder.append(
            BSON("k" << fieldElem.fieldNameStringData() << "v" << MAXKEY));
    }
    auto literalMinObject = BSON("$literal" << literalMinObjectBuilder.arr());
    auto literalMaxObject = BSON("$literal" << literalMaxObjectBuilder.arr());

    // Both the chunks and tags updates share the base of this $set modifier.
    auto extendMinAndMaxModifier = BSON(
        "min"
        << BSON("$arrayToObject" << BSON("$concatArrays" << BSON_ARRAY(BSON("$objectToArray"
                                                                            << "$min")
                                                                       << literalMinObject)))
        << "max"
        << BSON("$let" << BSON(
                    "vars"
                    << BSON("maxAsArray" << BSON("$objectToArray"
                                                 << "$max"))
                    << "in"
                    << BSON("$arrayToObject" << BSON(
                                "$concatArrays" << BSON_ARRAY(
                                    "$$maxAsArray"
                                    << BSON("$cond" << BSON(
                                                "if" << BSON("$allElementsTrue" << BSON_ARRAY(BSON(
                                                                 "$map" << BSON(
                                                                     "input"
                                                                     << "$$maxAsArray"
                                                                     << "in"
                                                                     << BSON("$eq" << BSON_ARRAY(
                                                                                 BSON("$type"
                                                                                      << "$$this.v")
                                                                                 << "maxKey"))))))
                                                     << "then" << literalMaxObject << "else"
                                                     << literalMinObject))))))));

    // The chunk updates change the min and max fields and unset the jumbo field.
    std::vector<BSONObj> chunkUpdates;
    chunkUpdates.emplace_back(BSON("$set" << extendMinAndMaxModifier.getOwned()));
    chunkUpdates.emplace_back(BSON("$unset" << ChunkType::jumbo()));

    // The tag updates only change the min and max fields.
    std::vector<BSONObj> tagUpdates;
    tagUpdates.emplace_back(BSON("$set" << extendMinAndMaxModifier.getOwned()));

    return std::make_pair(std::move(chunkUpdates), std::move(tagUpdates));
}

void ShardingCatalogManager::refineCollectionShardKey(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const ShardKeyPattern& newShardKeyPattern) {
    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications and generate
    // strictly monotonously increasing collection placement versions
    Lock::ExclusiveLock chunkLk(opCtx, _kChunkOpLock);
    Lock::ExclusiveLock zoneLk(opCtx, _kZoneOpLock);

    struct RefineTimers {
        Timer executionTimer;
        Timer totalTimer;
    };
    auto timers = std::make_shared<RefineTimers>();

    const auto newEpoch = OID::gen();

    auto collType = _localCatalogClient->getCollection(opCtx, nss);
    const auto oldShardKeyPattern = ShardKeyPattern(collType.getKeyPattern());

    uassertStatusOK(ShardingLogging::get(opCtx)->logChangeChecked(
        opCtx,
        "refineCollectionShardKey.start",
        nss.ns(),
        BSON("oldKey" << oldShardKeyPattern.toBSON() << "newKey" << newShardKeyPattern.toBSON()
                      << "oldEpoch" << collType.getEpoch() << "newEpoch" << newEpoch),
        ShardingCatalogClient::kLocalWriteConcern,
        _localConfigShard,
        _localCatalogClient.get()));

    const auto oldFields = oldShardKeyPattern.toBSON();
    const auto newFields =
        newShardKeyPattern.toBSON().filterFieldsUndotted(oldFields, false /* inFilter */);

    collType.setEpoch(newEpoch);
    collType.setKeyPattern(newShardKeyPattern.getKeyPattern());

    auto now = VectorClock::get(opCtx)->getTime();
    Timestamp newTimestamp = now.clusterTime().asTimestamp();
    collType.setTimestamp(newTimestamp);

    auto updateCollectionAndChunksWithAPIFn =
        [collType, newFields, nss, timers](const txn_api::TransactionClient& txnClient,
                                           ExecutorPtr txnExec) -> SemiFuture<void> {
        auto [chunkUpdates, tagUpdates] = makeChunkAndTagUpdatesForRefine(newFields);

        // Update the config.collections entry for the given namespace.
        auto catalogUpdateRequest =
            BatchedCommandRequest::buildUpdateOp(CollectionType::ConfigNS,
                                                 BSON(CollectionType::kNssFieldName << nss.ns()),
                                                 collType.toBSON(),
                                                 false /* upsert */,
                                                 false /* multi */);
        return txnClient.runCRUDOp(catalogUpdateRequest, {})
            .thenRunOn(txnExec)
            .then([&txnClient, timers, collType, nss, chunkUpdates = std::move(chunkUpdates)](
                      auto catalogResponse) {
                uassertStatusOK(catalogResponse.toStatus());

                LOGV2(5875906,
                      "refineCollectionShardKey updated collection entry for {namespace}: took "
                      "{durationMillis} ms. Total time taken: {totalTimeMillis} ms.",
                      "refineCollectionShardKey updated collection entry",
                      logAttrs(nss),
                      "durationMillis"_attr = timers->executionTimer.millis(),
                      "totalTimeMillis"_attr = timers->totalTimer.millis());
                timers->executionTimer.reset();

                if (MONGO_unlikely(hangRefineCollectionShardKeyBeforeUpdatingChunks.shouldFail())) {
                    LOGV2(5875907,
                          "Hit hangRefineCollectionShardKeyBeforeUpdatingChunks failpoint");
                    hangRefineCollectionShardKeyBeforeUpdatingChunks.pauseWhileSet();
                }

                // Update all config.chunks entries for the given namespace by setting (i) their
                // bounds for each new field in the refined key to MinKey (except for the global max
                // chunk where the max bounds are set to MaxKey), and unsetting (ii) their jumbo
                // field.
                const auto chunksQuery = BSON(ChunkType::collectionUUID << collType.getUuid());
                auto chunkUpdateRequest =
                    BatchedCommandRequest::buildPipelineUpdateOp(ChunkType::ConfigNS,
                                                                 chunksQuery,
                                                                 chunkUpdates,
                                                                 false /* upsert */,
                                                                 true /* useMultiUpdate */);

                return txnClient.runCRUDOp(chunkUpdateRequest, {});
            })
            .thenRunOn(txnExec)
            .then(
                [&txnClient, timers, nss, tagUpdates = std::move(tagUpdates)](auto chunksResponse) {
                    uassertStatusOK(chunksResponse.toStatus());

                    LOGV2(5875908,
                          "refineCollectionShardKey: updated chunk entries for {namespace}: took "
                          "{durationMillis} ms. Total time taken: {totalTimeMillis} ms.",
                          "refineCollectionShardKey: updated chunk entries",
                          logAttrs(nss),
                          "durationMillis"_attr = timers->executionTimer.millis(),
                          "totalTimeMillis"_attr = timers->totalTimer.millis());
                    timers->executionTimer.reset();

                    // Update all config.tags entries for the given namespace by setting their
                    // bounds for each new field in the refined key to MinKey (except for the global
                    // max tag where the max bounds are set to MaxKey).
                    auto tagUpdateRequest =
                        BatchedCommandRequest::buildPipelineUpdateOp(TagsType::ConfigNS,
                                                                     BSON("ns" << nss.ns()),
                                                                     tagUpdates,
                                                                     false /* upsert */,
                                                                     true /* useMultiUpdate */);
                    return txnClient.runCRUDOp(tagUpdateRequest, {});
                })
            .thenRunOn(txnExec)
            .then([&txnClient, timers, nss](auto tagsResponse) {
                uassertStatusOK(tagsResponse.toStatus());

                LOGV2(5875909,
                      "refineCollectionShardKey: updated zone entries for {namespace}: took "
                      "{durationMillis} ms. Total time taken: {totalTimeMillis} ms.",
                      "refineCollectionShardKey: updated zone entries",
                      logAttrs(nss),
                      "durationMillis"_attr = timers->executionTimer.millis(),
                      "totalTimeMillis"_attr = timers->totalTimer.millis());

                if (MONGO_unlikely(hangRefineCollectionShardKeyBeforeCommit.shouldFail())) {
                    LOGV2(5875910, "Hit hangRefineCollectionShardKeyBeforeCommit failpoint");
                    hangRefineCollectionShardKeyBeforeCommit.pauseWhileSet();
                }
            })
            .semi();
    };

    // The transaction API will use the write concern on the opCtx, which will have the default
    // sharding wTimeout of 60 seconds. Refining a shard key may involve writing many more
    // documents than a normal operation, so we override the write concern to not use a
    // wTimeout, matching the behavior before the API was introduced.
    WriteConcernOptions originalWC = opCtx->getWriteConcern();
    opCtx->setWriteConcern(WriteConcernOptions{WriteConcernOptions::kMajority,
                                               WriteConcernOptions::SyncMode::UNSET,
                                               WriteConcernOptions::kNoTimeout});
    ON_BLOCK_EXIT([opCtx, originalWC] { opCtx->setWriteConcern(originalWC); });

    withTransactionAPI(opCtx, nss, std::move(updateCollectionAndChunksWithAPIFn));

    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           "refineCollectionShardKey.end",
                                           nss.ns(),
                                           BSONObj(),
                                           ShardingCatalogClient::kLocalWriteConcern,
                                           _localConfigShard,
                                           _localCatalogClient.get());

    // Trigger refreshes on each shard containing chunks in the namespace 'nss'. Since this isn't
    // necessary for correctness, all refreshes are best-effort.
    try {
        triggerFireAndForgetShardRefreshes(
            opCtx, _localConfigShard.get(), _localCatalogClient.get(), collType);
    } catch (const DBException& ex) {
        LOGV2(
            51798,
            "refineCollectionShardKey: failed to best-effort refresh all shards containing chunks "
            "in {namespace}",
            "refineCollectionShardKey: failed to best-effort refresh all shards containing chunks",
            "error"_attr = ex.toStatus(),
            logAttrs(nss));
    }
}

void ShardingCatalogManager::configureCollectionBalancing(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<int32_t> chunkSizeMB,
    boost::optional<bool> defragmentCollection,
    boost::optional<bool> enableAutoMerger) {

    if (!chunkSizeMB && !defragmentCollection && !enableAutoMerger) {
        // No-op in case no supported parameter has been specified.
        // This allows not breaking backwards compatibility as command
        // options may be added/removed over time.
        return;
    }

    // utility lambda to log the change
    auto logConfigureCollectionBalancing = [&]() {
        BSONObjBuilder logChangeDetail;
        if (chunkSizeMB) {
            logChangeDetail.append("chunkSizeMB", chunkSizeMB.get());
        }

        if (defragmentCollection) {
            logChangeDetail.append("defragmentCollection", defragmentCollection.get());
        }

        ShardingLogging::get(opCtx)->logChange(opCtx,
                                               "configureCollectionBalancing",
                                               nss.ns(),
                                               logChangeDetail.obj(),
                                               ShardingCatalogClient::kMajorityWriteConcern,
                                               _localConfigShard,
                                               _localCatalogClient.get());
    };

    short updatedFields = 0;
    BSONObjBuilder updateCmd;
    BSONObjBuilder setClauseBuilder;
    {
        if (chunkSizeMB && *chunkSizeMB != 0) {
            auto chunkSizeBytes = static_cast<int64_t>(*chunkSizeMB) * 1024 * 1024;
            bool withinRange = nss == NamespaceString::kLogicalSessionsNamespace
                ? (chunkSizeBytes > 0 && chunkSizeBytes <= 1024 * 1024 * 1024)
                : ChunkSizeSettingsType::checkMaxChunkSizeValid(chunkSizeBytes);
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "Chunk size '" << *chunkSizeMB << "' out of range [1MB, 1GB]",
                    withinRange);
            setClauseBuilder.append(CollectionType::kMaxChunkSizeBytesFieldName, chunkSizeBytes);
            updatedFields++;
        }
        if (defragmentCollection) {
            bool doDefragmentation = defragmentCollection.value();
            if (doDefragmentation) {
                setClauseBuilder.append(CollectionType::kDefragmentCollectionFieldName,
                                        doDefragmentation);
                updatedFields++;
            } else {
                Balancer::get(opCtx)->abortCollectionDefragmentation(opCtx, nss);
            }
        }
        if (enableAutoMerger) {
            setClauseBuilder.append(CollectionType::kEnableAutoMergeFieldName,
                                    enableAutoMerger.value());
            updatedFields++;
        }
    }
    if (chunkSizeMB && *chunkSizeMB == 0) {
        // Logic to reset the 'maxChunkSizeBytes' field to its default value
        if (nss == NamespaceString::kLogicalSessionsNamespace) {
            setClauseBuilder.append(CollectionType::kMaxChunkSizeBytesFieldName,
                                    logical_sessions::kMaxChunkSizeBytes);
        } else {
            BSONObjBuilder unsetClauseBuilder;
            unsetClauseBuilder.append(CollectionType::kMaxChunkSizeBytesFieldName, 0);
            updateCmd.append("$unset", unsetClauseBuilder.obj());
        }
        updatedFields++;
    }

    if (updatedFields == 0) {
        logConfigureCollectionBalancing();
        return;
    }

    updateCmd.append("$set", setClauseBuilder.obj());
    const auto update = updateCmd.obj();
    {
        // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
        // migrations
        Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

        withTransaction(opCtx,
                        CollectionType::ConfigNS,
                        [this, &nss, &update](OperationContext* opCtx, TxnNumber txnNumber) {
                            const auto query = BSON(CollectionType::kNssFieldName << nss.ns());
                            const auto res = writeToConfigDocumentInTxn(
                                opCtx,
                                CollectionType::ConfigNS,
                                BatchedCommandRequest::buildUpdateOp(CollectionType::ConfigNS,
                                                                     query,
                                                                     update /* update */,
                                                                     false /* upsert */,
                                                                     false /* multi */),
                                txnNumber);
                            const auto numDocsModified = UpdateOp::parseResponse(res).getN();
                            uassert(ErrorCodes::NamespaceNotSharded,
                                    str::stream() << "Expected to match one doc for query " << query
                                                  << " but matched " << numDocsModified,
                                    numDocsModified == 1);

                            bumpCollectionMinorVersionInTxn(opCtx, nss, txnNumber);
                        });
        // Now any migrations that change the list of shards will see the results of the transaction
        // during refresh, so it is safe to release the chunk lock.
    }

    const auto [cm, _] = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithPlacementRefresh(opCtx,
                                                                                              nss));
    std::set<ShardId> shardsIds;
    cm.getAllShardIds(&shardsIds);

    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    sharding_util::tellShardsToRefreshCollection(
        opCtx,
        {std::make_move_iterator(shardsIds.begin()), std::make_move_iterator(shardsIds.end())},
        nss,
        executor);

    Balancer::get(opCtx)->notifyPersistedBalancerSettingsChanged(opCtx);

    logConfigureCollectionBalancing();
}

void ShardingCatalogManager::renameShardedMetadata(
    OperationContext* opCtx,
    const NamespaceString& from,
    const NamespaceString& to,
    const WriteConcernOptions& writeConcern,
    boost::optional<CollectionType> optFromCollType) {
    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications and generate
    // strictly monotonously increasing collection placement versions
    Lock::ExclusiveLock chunkLk(opCtx, _kChunkOpLock);
    Lock::ExclusiveLock zoneLk(opCtx, _kZoneOpLock);

    std::string logMsg = str::stream() << from << " to " << to;
    if (optFromCollType) {
        // Rename CSRS metadata in case the source collection is sharded
        auto collType = *optFromCollType;
        sharding_ddl_util::shardedRenameMetadata(
            opCtx, _localConfigShard, _localCatalogClient.get(), collType, to, writeConcern);
        ShardingLogging::get(opCtx)->logChange(
            opCtx,
            "renameCollection.metadata",
            str::stream() << logMsg << ": dropped target collection and renamed source collection",
            BSON("newCollMetadata" << collType.toBSON()),
            ShardingCatalogClient::kLocalWriteConcern,
            _localConfigShard,
            _localCatalogClient.get());
    } else {
        // Remove stale CSRS metadata in case the source collection is unsharded and the
        // target collection was sharded
        // throws if the provided UUID does not match
        sharding_ddl_util::removeCollAndChunksMetadataFromConfig_notIdempotent(
            opCtx, _localConfigShard, _localCatalogClient.get(), to, writeConcern);
        sharding_ddl_util::removeTagsMetadataFromConfig_notIdempotent(
            opCtx, _localConfigShard, to, writeConcern);
        ShardingLogging::get(opCtx)->logChange(opCtx,
                                               "renameCollection.metadata",
                                               str::stream()
                                                   << logMsg << " : dropped target collection.",
                                               BSONObj(),
                                               ShardingCatalogClient::kLocalWriteConcern,
                                               _localConfigShard,
                                               _localCatalogClient.get());
    }
}

void ShardingCatalogManager::updateTimeSeriesBucketingParameters(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollModTimeseries& timeseriesParameters) {
    // Take _kChunkOpLock in exclusive mode to prevent concurrent updates of the collection
    // placement version.
    Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

    const auto [cm, _] = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithPlacementRefresh(opCtx,
                                                                                              nss));
    std::set<ShardId> shardIds;
    cm.getAllShardIds(&shardIds);

    withTransaction(opCtx,
                    CollectionType::ConfigNS,
                    [this, &nss, &timeseriesParameters, &shardIds](OperationContext* opCtx,
                                                                   TxnNumber txnNumber) {
                        auto granularityFieldName = CollectionType::kTimeseriesFieldsFieldName +
                            "." + TypeCollectionTimeseriesFields::kGranularityFieldName;
                        auto bucketSpanFieldName = CollectionType::kTimeseriesFieldsFieldName +
                            "." + TypeCollectionTimeseriesFields::kBucketMaxSpanSecondsFieldName;
                        auto bucketRoundingFieldName = CollectionType::kTimeseriesFieldsFieldName +
                            "." + TypeCollectionTimeseriesFields::kBucketRoundingSecondsFieldName;

                        BSONObjBuilder updateCmd;
                        BSONObj bucketUp;
                        if (timeseriesParameters.getGranularity().has_value()) {
                            auto bucketSpan = timeseries::getMaxSpanSecondsFromGranularity(
                                timeseriesParameters.getGranularity().get());
                            updateCmd.append("$unset", BSON(bucketRoundingFieldName << ""));
                            bucketUp = BSON(granularityFieldName
                                            << BucketGranularity_serializer(
                                                   timeseriesParameters.getGranularity().get())
                                            << bucketSpanFieldName << bucketSpan);
                        } else {
                            invariant(timeseriesParameters.getBucketMaxSpanSeconds().has_value() &&
                                      timeseriesParameters.getBucketRoundingSeconds().has_value());
                            updateCmd.append("$unset", BSON(granularityFieldName << ""));
                            bucketUp =
                                BSON(bucketSpanFieldName
                                     << timeseriesParameters.getBucketMaxSpanSeconds().get()
                                     << bucketRoundingFieldName
                                     << timeseriesParameters.getBucketRoundingSeconds().get());
                        }
                        updateCmd.append("$set", bucketUp);

                        writeToConfigDocumentInTxn(
                            opCtx,
                            CollectionType::ConfigNS,
                            BatchedCommandRequest::buildUpdateOp(
                                CollectionType::ConfigNS,
                                BSON(CollectionType::kNssFieldName << nss.ns()) /* query */,
                                updateCmd.obj() /* update */,
                                false /* upsert */,
                                false /* multi */),
                            txnNumber);

                        // Bump the chunk version for shards.
                        bumpMajorVersionOneChunkPerShard(opCtx,
                                                         nss,
                                                         txnNumber,
                                                         {std::make_move_iterator(shardIds.begin()),
                                                          std::make_move_iterator(shardIds.end())});
                    });
}

}  // namespace mongo
