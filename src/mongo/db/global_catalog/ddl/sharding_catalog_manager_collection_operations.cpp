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


#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/routing_information_cache.h"
#include "mongo/db/global_catalog/chunk_constraints.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

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
    const auto allShards =
        catalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kLocalReadConcern).value;
    for (const auto& shardEntry : allShards) {
        const auto query = BSON(ChunkType::collectionUUID
                                << coll.getUuid() << ChunkType::shard(shardEntry.getName()));

        const auto chunk = uassertStatusOK(configShard->exhaustiveFindOnConfig(
                                               opCtx,
                                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                               repl::ReadConcernLevel::kLocalReadConcern,
                                               NamespaceString::kConfigsvrChunksNamespace,
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
                DatabaseName::kAdmin,
                BSON("_flushRoutingTableCacheUpdates" << NamespaceStringUtil::serialize(
                         coll.getNss(), SerializationContext::stateDefault())));
        }
    }
}

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
        << BSON("$arrayToObject" << BSON("$concatArrays" << BSON_ARRAY(
                                             BSON("$objectToArray" << "$min") << literalMinObject)))
        << "max"
        << BSON("$let" << BSON(
                    "vars"
                    << BSON("maxAsArray" << BSON("$objectToArray" << "$max")) << "in"
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

void refineCollectionShardKeyInTxn(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const ShardKeyPattern& newShardKeyPattern,
                                   const Timestamp& newTimestamp,
                                   const OID& newEpoch,
                                   boost::optional<Timestamp> oldTimestamp) {
    Timer executionTimer;
    Timer totalTimer;

    auto updateCollectionAndChunksFn = [&](const txn_api::TransactionClient& txnClient,
                                           ExecutorPtr txnExec) -> SemiFuture<void> {
        FindCommandRequest collQuery{CollectionType::ConfigNS};
        BSONObjBuilder builder;
        builder.append(CollectionType::kNssFieldName,
                       NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
        if (oldTimestamp.is_initialized()) {
            builder.append(CollectionType::kTimestampFieldName, *oldTimestamp);
        }
        collQuery.setFilter(builder.obj());
        collQuery.setLimit(1);
        const auto findCollResponse = txnClient.exhaustiveFindSync(collQuery);
        tassert(7906400,
                str::stream()
                    << "Couldn't find collection '" << nss.toStringForErrorMsg()
                    << "' in the cluster catalog during refineCollectionShardKey commit phase",
                findCollResponse.size() == 1);

        CollectionType collType(findCollResponse[0]);
        const auto oldShardKeyPattern = ShardKeyPattern(collType.getKeyPattern());
        const auto oldFields = oldShardKeyPattern.toBSON();
        const auto newFields =
            newShardKeyPattern.toBSON().filterFieldsUndotted(oldFields, false /* inFilter */);

        collType.setEpoch(newEpoch);
        collType.setTimestamp(newTimestamp);
        collType.setKeyPattern(newShardKeyPattern.getKeyPattern());

        // Update the config.collections entry for the given namespace.
        auto catalogUpdateRequest = BatchedCommandRequest::buildUpdateOp(
            CollectionType::ConfigNS,
            BSON(CollectionType::kNssFieldName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
            collType.toBSON(),
            false /* upsert */,
            false /* multi */);
        auto updateCollResponse = txnClient.runCRUDOpSync(catalogUpdateRequest, {});
        uassertStatusOK(updateCollResponse.toStatus());

        LOGV2(7648601,
              "refineCollectionShardKey updated collection entry",
              logAttrs(nss),
              "durationMillis"_attr = executionTimer.millis(),
              "totalTimeMillis"_attr = totalTimer.millis());
        executionTimer.reset();
        if (MONGO_unlikely(hangRefineCollectionShardKeyBeforeUpdatingChunks.shouldFail())) {
            LOGV2(7648602, "Hit hangRefineCollectionShardKeyBeforeUpdatingChunks failpoint");
            hangRefineCollectionShardKeyBeforeUpdatingChunks.pauseWhileSet();
        }

        auto [chunkUpdates, tagUpdates] = makeChunkAndTagUpdatesForRefine(newFields);

        const auto chunksQuery = BSON(ChunkType::collectionUUID << collType.getUuid());
        auto chunksUpdateRequest =
            BatchedCommandRequest::buildPipelineUpdateOp(NamespaceString::kConfigsvrChunksNamespace,
                                                         chunksQuery,
                                                         chunkUpdates,
                                                         false /* upsert */,
                                                         true /* useMultiUpdate */);

        auto updateChunksResponse = txnClient.runCRUDOpSync(chunksUpdateRequest, {});
        uassertStatusOK(updateChunksResponse.toStatus());
        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Expected to match at least one doc but matched "
                              << updateChunksResponse.getN(),
                updateChunksResponse.getN() > 0);

        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Expected to match one doc but matched "
                              << updateCollResponse.getN(),
                updateCollResponse.getN() == 1);
        LOGV2(7648603,
              "refineCollectionShardKey: updated chunk entries",
              logAttrs(nss),
              "durationMillis"_attr = executionTimer.millis(),
              "totalTimeMillis"_attr = totalTimer.millis());
        executionTimer.reset();
        auto tagUpdateRequest = BatchedCommandRequest::buildPipelineUpdateOp(
            TagsType::ConfigNS,
            BSON("ns" << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
            tagUpdates,
            false /* upsert */,
            true /* useMultiUpdate */);
        auto updateTagResponse = txnClient.runCRUDOpSync(tagUpdateRequest, {});
        uassertStatusOK(updateTagResponse.toStatus());

        LOGV2(7648604,
              "refineCollectionShardKey: updated zone entries",
              logAttrs(nss),
              "durationMillis"_attr = executionTimer.millis(),
              "totalTimeMillis"_attr = totalTimer.millis());

        if (MONGO_unlikely(hangRefineCollectionShardKeyBeforeCommit.shouldFail())) {
            LOGV2(7648605, "Hit hangRefineCollectionShardKeyBeforeCommit failpoint");
            hangRefineCollectionShardKeyBeforeCommit.pauseWhileSet();
        }

        return SemiFuture<void>::makeReady();
    };
    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

    txn_api::SyncTransactionWithRetries txn(opCtx, executor, nullptr, inlineExecutor);

    txn.run(opCtx, updateCollectionAndChunksFn);
}

}  // namespace

void ShardingCatalogManager::commitRefineCollectionShardKey(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardKeyPattern& newShardKeyPattern,
    const Timestamp& newTimestamp,
    const OID& newEpoch,
    const Timestamp& oldTimestamp) {
    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications and generate
    // strictly monotonously increasing collection placement versions
    Lock::ExclusiveLock chunkLk(opCtx, _kChunkOpLock);
    Lock::ExclusiveLock zoneLk(opCtx, _kZoneOpLock);

    // Idempotency check: if the shard key is already the one requested, there is nothing to do
    // except waiting for majority, in case the write haven't been majority written.
    auto collType =
        _localCatalogClient->getCollection(opCtx, nss, repl::ReadConcernLevel::kLocalReadConcern);
    if (newTimestamp == collType.getTimestamp()) {
        uassert(7648607,
                str::stream() << "Expected refined key " << newShardKeyPattern.toBSON() << " but "
                              << collType.getKeyPattern().toBSON() << " provided",
                SimpleBSONObjComparator::kInstance.evaluate(collType.getKeyPattern().toBSON() ==
                                                            newShardKeyPattern.toBSON()));
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return;
    }

    // In order to proceed, the timestamp must match.
    uassert(7648608,
            str::stream() << "Expected to find collection " << nss.toStringForErrorMsg()
                          << " with timestamp " << oldTimestamp.toStringPretty(),
            collType.getTimestamp() == oldTimestamp);

    // The transaction API will use the write concern on the opCtx, which will have the default
    // sharding wTimeout of 60 seconds. Refining a shard key may involve writing many more
    // documents than a normal operation, so we override the write concern to not use a
    // wTimeout, matching the behavior before the API was introduced.
    WriteConcernOptions originalWC = opCtx->getWriteConcern();
    opCtx->setWriteConcern(WriteConcernOptions{WriteConcernOptions::kMajority,
                                               WriteConcernOptions::SyncMode::UNSET,
                                               WriteConcernOptions::kNoTimeout});
    ON_BLOCK_EXIT([opCtx, originalWC] { opCtx->setWriteConcern(originalWC); });

    refineCollectionShardKeyInTxn(
        opCtx, nss, newShardKeyPattern, newTimestamp, newEpoch, oldTimestamp);
}

void ShardingCatalogManager::configureCollectionBalancing(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<int32_t> chunkSizeMB,
    boost::optional<bool> defragmentCollection,
    boost::optional<bool> enableAutoMerger,
    boost::optional<bool> enableBalancing) {

    if (!chunkSizeMB && !defragmentCollection && !enableAutoMerger && !enableBalancing) {
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
                                               nss,
                                               logChangeDetail.obj(),
                                               defaultMajorityWriteConcernDoNotUse(),
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
        if (enableBalancing) {
            setClauseBuilder.append(CollectionType::kNoBalanceFieldName, !enableBalancing.value());
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
                            const auto query = BSON(
                                CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                                    nss, SerializationContext::stateDefault()));
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

    const auto cm = uassertStatusOK(
        RoutingInformationCache::get(opCtx)->getCollectionPlacementInfoWithRefresh(opCtx, nss));
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

void ShardingCatalogManager::updateTimeSeriesBucketingParameters(
    OperationContext* opCtx, const NamespaceString& nss, const CollModRequest& collModRequest) {
    // Take _kChunkOpLock in exclusive mode to prevent concurrent updates of the collection
    // placement version.
    Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

    const boost::optional<CollModTimeseries>& timeseriesParameters = collModRequest.getTimeseries();
    const boost::optional<bool>& bucketsMayHaveMixedSchemaData =
        collModRequest.getTimeseriesBucketsMayHaveMixedSchemaData();

    tassert(10533700,
            "Expected timeseries parameters or mixed schema flag to be set",
            timeseriesParameters.has_value() || bucketsMayHaveMixedSchemaData.has_value());

    const auto cm = uassertStatusOK(
        RoutingInformationCache::get(opCtx)->getCollectionPlacementInfoWithRefresh(opCtx, nss));
    std::set<ShardId> shardIds;
    cm.getAllShardIds(&shardIds);
    tassert(10533701,
            "Cannot update timeseries fields on a collection that is not a timeseries",
            cm.isTimeseriesCollection());

    withTransaction(
        opCtx,
        CollectionType::ConfigNS,
        [this, &nss, &timeseriesParameters, &bucketsMayHaveMixedSchemaData, &shardIds](
            OperationContext* opCtx, TxnNumber txnNumber) {
            auto granularityFieldName = CollectionType::kTimeseriesFieldsFieldName + "." +
                TypeCollectionTimeseriesFields::kGranularityFieldName;
            auto bucketSpanFieldName = CollectionType::kTimeseriesFieldsFieldName + "." +
                TypeCollectionTimeseriesFields::kBucketMaxSpanSecondsFieldName;
            auto bucketRoundingFieldName = CollectionType::kTimeseriesFieldsFieldName + "." +
                TypeCollectionTimeseriesFields::kBucketRoundingSecondsFieldName;
            auto mixedSchemaFieldName = CollectionType::kTimeseriesFieldsFieldName + "." +
                TypeCollectionTimeseriesFields::kTimeseriesBucketsMayHaveMixedSchemaDataFieldName;

            BSONObjBuilder updateBob;

            if (timeseriesParameters.has_value()) {
                BSONObj bucketUp;
                if (timeseriesParameters->getGranularity().has_value()) {
                    auto bucketSpan = timeseries::getMaxSpanSecondsFromGranularity(
                        timeseriesParameters->getGranularity().get());
                    updateBob.append("$unset", BSON(bucketRoundingFieldName << ""));
                    bucketUp = BSON(granularityFieldName
                                    << BucketGranularity_serializer(
                                           timeseriesParameters->getGranularity().get())
                                    << bucketSpanFieldName << bucketSpan);
                } else {
                    tassert(
                        10533702,
                        "Expected bucketMaxSpanSeconds and bucketRoundingSeconds to be both set",
                        timeseriesParameters->getBucketMaxSpanSeconds().has_value() &&
                            timeseriesParameters->getBucketRoundingSeconds().has_value());
                    updateBob.append("$unset", BSON(granularityFieldName << ""));
                    bucketUp = BSON(bucketSpanFieldName
                                    << timeseriesParameters->getBucketMaxSpanSeconds().get()
                                    << bucketRoundingFieldName
                                    << timeseriesParameters->getBucketRoundingSeconds().get());
                }
                updateBob.append("$set", bucketUp);
            }

            // TODO SERVER-108908: once 9.0 branches out, remove `isViewLessTimeseries` check
            const bool isViewLessTimeseries = !nss.isTimeseriesBucketsCollection();
            if (isViewLessTimeseries && bucketsMayHaveMixedSchemaData.has_value()) {
                updateBob.append("$set",
                                 BSON(mixedSchemaFieldName << bucketsMayHaveMixedSchemaData.get()));
            }

            const BSONObj update = updateBob.obj();
            if (update.isEmpty()) {
                return;
            }

            writeToConfigDocumentInTxn(
                opCtx,
                CollectionType::ConfigNS,
                BatchedCommandRequest::buildUpdateOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                             nss, SerializationContext::stateDefault())) /* query */,
                    update,
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
