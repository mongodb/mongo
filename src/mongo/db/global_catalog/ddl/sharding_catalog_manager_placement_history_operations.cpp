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
#include "mongo/base/status.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/placement_history_cleaner.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/index_on_config.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/util/pcre_util.h"

#include <algorithm>
#include <vector>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(initializePlacementHistoryHangAfterSettingSnapshotReadConcern);

class PipelineBuilder {

public:
    PipelineBuilder(OperationContext* opCtx,
                    const NamespaceString& nss,
                    std::vector<NamespaceString>&& resolvedNamespaces)
        : _expCtx{ExpressionContextBuilder{}.opCtx(opCtx).ns(nss).build()} {

        ResolvedNamespaceMap resolvedNamespacesMap;

        for (const auto& collNs : resolvedNamespaces) {
            resolvedNamespacesMap[collNs] = {collNs, std::vector<BSONObj>() /* pipeline */};
        }

        _expCtx->setResolvedNamespaces(std::move(resolvedNamespacesMap));
    }

    PipelineBuilder(const boost::intrusive_ptr<ExpressionContext>& expCtx) : _expCtx(expCtx) {}

    template <typename T>
    PipelineBuilder& addStage(mongo::BSONObj&& bsonObj) {
        _stages.emplace_back(_toStage<T>(_expCtx, std::move(bsonObj)));
        return *this;
    }

    std::vector<BSONObj> buildAsBson() {
        auto pipelinePtr = Pipeline::create(_stages, _expCtx);
        return pipelinePtr->serializeToBson();
    }

    AggregateCommandRequest buildAsAggregateCommandRequest() {
        return AggregateCommandRequest(_expCtx->getNamespaceString(), buildAsBson());
    }

    boost::intrusive_ptr<ExpressionContext>& getExpCtx() {
        return _expCtx;
    }

private:
    template <typename T>
    boost::intrusive_ptr<DocumentSource> _toStage(boost::intrusive_ptr<ExpressionContext>& expCtx,
                                                  mongo::BSONObj&& bsonObj) {
        return T::createFromBson(BSON(T::kStageName << std::move(bsonObj)).firstElement(), expCtx);
    }

    boost::intrusive_ptr<ExpressionContext> _expCtx;
    DocumentSourceContainer _stages;
};

AggregateCommandRequest createInitPlacementHistoryAggregationRequest(
    OperationContext* opCtx, const Timestamp& initTimestamp) {
    /* Compose the pipeline to generate a NamespacePlacementType for each existing collection and
     * database in the cluster based on the content of the sharding catalog.
     *
     * 1. Join config.collections with config.chunks to extract
     * - the collection name and uuid
     * - the list of shards containing one or more chunks of the collection
     * - the timestamp of the most recent collection chunk migration received by each shard
     *
     * 2. Project the output to
     * - select the most recent collection chunk migration across shards (using initTimestamp as a
     *   fallback in case no timestamp could be retrieved on stage 1)
     * - fit each document to the  NamespacePlacementType schema
     *
     * 3. Add to the previous results a projection of the config.databases entries that fits the
     *    NamespacePlacementType schema
     *
     * 4. merge everything into config.placementHistory.
     *
     db.collections.aggregate([
     {
         $lookup: {
         from: "chunks",
         localField: "uuid",
         foreignField: "uuid",
         as: "timestampByShard",
         pipeline: [
             {
              $group: {
                 _id: "$shard",
                 value: {
                 $max: "$onCurrentShardSince"
                 }
             }
             }
         ],
         }
     },
     {
         $project: {
         _id: 0,
         nss: "$_id",
         shards: "$timestampByShard._id",
         uuid: 1,
         timestamp: {
             $ifNull: [
             {
                 $max: "$timestampByShard.timestamp"
             },
             <initTimestamp>
             ]
         },
         }
     },
     {
         $unionWith: {
          coll: "databases",
          pipeline: [
             {
             $project: {
                 _id: 0,
                 nss: "$_id",
                 shards: [
                 "$primary"
                 ],
                 timestamp: "$version.timestamp"
             }
             }
         ]
         }
     },
     {
         $merge:
         {
             into: "config.placementHistory",
             on: ["nss", "timestamp"],
             whenMatched: "replace",
             whenNotMatched: "insert"
         }
     }
     ])
     */
    using Lookup = DocumentSourceLookUp;
    using UnionWith = DocumentSourceUnionWith;
    using Merge = DocumentSourceMerge;
    using Group = DocumentSourceGroup;
    using Project = DocumentSourceProject;

    // Aliases for the field names of the the final projections
    const auto kNss = std::string{NamespacePlacementType::kNssFieldName};
    const auto kUuid = std::string{NamespacePlacementType::kUuidFieldName};
    const auto kShards = std::string{NamespacePlacementType::kShardsFieldName};
    const auto kTimestamp = std::string{NamespacePlacementType::kTimestampFieldName};

    auto pipeline = PipelineBuilder(opCtx,
                                    CollectionType::ConfigNS,
                                    {NamespaceString::kConfigsvrChunksNamespace,
                                     CollectionType::ConfigNS,
                                     NamespaceString::kConfigDatabasesNamespace,
                                     NamespaceString::kConfigsvrPlacementHistoryNamespace});

    // Stage 1. Join config.collections and config.chunks using the collection UUID to create the
    // placement-by-shard info documents
    {
        auto lookupPipelineObj =
            PipelineBuilder(pipeline.getExpCtx())
                .addStage<Group>(BSON("_id" << "$shard"
                                            << "value" << BSON("$max" << "$onCurrentShardSince")))
                .buildAsBson();

        pipeline.addStage<Lookup>(BSON("from" << NamespaceString::kConfigsvrChunksNamespace.coll()
                                              << "localField" << CollectionType::kUuidFieldName
                                              << "foreignField" << ChunkType::collectionUUID.name()
                                              << "as"
                                              << "timestampByShard"
                                              << "pipeline" << lookupPipelineObj));
    }

    // Stage 2. Adapt the info on collections to the config.placementHistory entry format
    {
        // Get the most recent collection placement timestamp among all the shards: if not found,
        // apply initTimestamp as a fallback.
        const auto placementTimestampExpr = BSON(
            "$ifNull" << BSON_ARRAY(BSON("$max" << "$timestampByShard.value") << initTimestamp));

        pipeline.addStage<Project>(BSON("_id" << 0 << kNss << "$_id" << kShards
                                              << "$timestampByShard._id" << kUuid << 1 << kTimestamp
                                              << placementTimestampExpr));
    }

    // Stage 3 Add placement info on each database of the cluster
    {
        pipeline.addStage<UnionWith>(
            BSON("coll" << NamespaceString::kConfigDatabasesNamespace.coll() << "pipeline"
                        << PipelineBuilder(pipeline.getExpCtx())
                               .addStage<Project>(BSON("_id" << 0 << kNss << "$_id" << kShards
                                                             << BSON_ARRAY("$primary") << kTimestamp
                                                             << "$version.timestamp"))
                               .buildAsBson()));
    }

    // Stage 4. Merge into the placementHistory collection
    {
        pipeline.addStage<Merge>(BSON("into"
                                      << NamespaceString::kConfigsvrPlacementHistoryNamespace.coll()
                                      << "on" << BSON_ARRAY(kNss << kTimestamp) << "whenMatched"
                                      << "replace"
                                      << "whenNotMatched"
                                      << "insert"));
    }

    return pipeline.buildAsAggregateCommandRequest();
}

AggregateCommandRequest findAllShardsAggRequest(OperationContext* opCtx) {
    DocumentSourceContainer stageContainer;
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .ns(NamespaceString::kConfigsvrShardsNamespace)
                      .build();

    auto findAllPipeline = Pipeline::create({}, expCtx);
    return AggregateCommandRequest(NamespaceString::kConfigsvrShardsNamespace,
                                   findAllPipeline->serializeToBson());
}

void setInitializationTimeOnPlacementHistory(
    OperationContext* opCtx,
    Timestamp initializationTime,
    std::vector<ShardId> placementResponseForPreInitQueries) {
    auto transactionChain = [&](const txn_api::TransactionClient& txnClient,
                                ExecutorPtr txnExec) -> SemiFuture<void> {
        write_ops::DeleteCommandRequest deleteOldMetadata(
            NamespaceString::kConfigsvrPlacementHistoryNamespace);
        write_ops::DeleteOpEntry deleteStmt;
        deleteStmt.setQ(
            BSON(NamespacePlacementType::kNssFieldName << NamespaceStringUtil::serialize(
                     ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker,
                     SerializationContext::stateDefault())));
        deleteStmt.setMulti(true);
        deleteOldMetadata.setDeletes({std::move(deleteStmt)});

        auto deleteResponse = txnClient.runCRUDOpSync(deleteOldMetadata, {});
        uassertStatusOK(deleteResponse.toStatus());

        write_ops::InsertCommandRequest insertNewMetadata =
            ShardingCatalogManager::buildInsertReqForPlacementHistoryOperationalBoundaries(
                initializationTime, placementResponseForPreInitQueries);

        auto insertResponse = txnClient.runCRUDOpSync(insertNewMetadata, {});
        uassertStatusOK(insertResponse.toStatus());

        return SemiFuture<void>::makeReady();
    };

    WriteConcernOptions originalWC = opCtx->getWriteConcern();
    opCtx->setWriteConcern(WriteConcernOptions{WriteConcernOptions::kMajority,
                                               WriteConcernOptions::SyncMode::UNSET,
                                               WriteConcernOptions::kNoTimeout});

    ScopeGuard resetWriteConcerGuard([opCtx, &originalWC] { opCtx->setWriteConcern(originalWC); });

    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

    txn_api::SyncTransactionWithRetries txn(
        opCtx, executor, nullptr /* resourceYielder */, inlineExecutor);
    txn.run(opCtx, transactionChain);

    LOGV2(7068807,
          "Initialization metadata of placement.history have been updated",
          "initializationTime"_attr = initializationTime);
}

/**
 * Helper class used to do the heavy-lifting for 'ShardingCatalogManager::getHistoricalPlacement()'.
 */
class HistoricalPlacementReader {
public:
    HistoricalPlacementReader(OperationContext* opCtx,
                              const boost::optional<NamespaceString>& nss,
                              const VectorClock::VectorTime& vcTime,
                              ShardingCatalogClient* localCatalogClient)
        : _opCtx(opCtx), _nss(nss), _vcTime(vcTime), _localCatalogClient(localCatalogClient) {
        ResolvedNamespaceMap resolvedNamespaces;
        resolvedNamespaces[NamespaceString::kConfigsvrPlacementHistoryNamespace] = {
            NamespaceString::kConfigsvrPlacementHistoryNamespace,
            std::vector<BSONObj>() /* pipeline */};
        _expCtx = ExpressionContextBuilder{}
                      .opCtx(_opCtx)
                      .ns(NamespaceString::kConfigsvrPlacementHistoryNamespace)
                      .resolvedNamespace(std::move(resolvedNamespaces))
                      .build();
    }

    // The aggregation is composed by 2 sub pipelines:
    // 1. The first sub pipeline pulls the 'config.placementHistory' initialization document
    //    related to the requested 'atClusterTime', whose content will be later processed to
    //    determine if the query may be served (and if so, through which data).
    // 2. The second sub pipeline retrieves the set of data bearing shards for the nss/whole
    //    cluster at the requested time, combining the placement metadata of each
    //    collection/database name that matches the search criteria.
    // These two sub pipelines will be executed as facets in a main pipeline.
    // ************************************************************
    // Full pipeline for collection level search:
    // [
    //   {
    //     '$facet': {
    //       'computedPlacement': [
    //         {
    //           '$match': {
    //             'timestamp': {
    //               '$lte': '<atClusterTime>'
    //             },
    //               'nss': {'$regex': '^dbName(\\.collName)?$'}
    //           }
    //         },
    //         {
    //           '$sort': {'timestamp': -1}
    //         },
    //         {
    //           '$group': {
    //             '_id': '$nss',
    //             'shards': {
    //               '$first': '$shards'
    //             }
    //           }
    //         },
    //         {
    //           '$sort': {'_id': -1}
    //         },
    //         {
    //           '$match': {'shards': {'$ne': []}}
    //         },
    //         {
    //           '$limit': 1
    //         },
    //       ],
    //       'metadataFromInitDoc': [
    //         {
    //           '$match': {
    //             'timestamp': {
    //               '$lte': '<atClusterTime>'
    //               },
    //             'nss': '.'
    //           }
    //         },
    //         {
    //           '$sort': {'timestamp': -1}
    //         },
    //         {
    //           '$limit': 1
    //         },
    //         {
    //           '$project': {
    //             'consistentMetadataAvailable': {
    //               '$eq': [
    //                 {
    //                   '$size': '$shards'
    //                 },
    //                 0
    //               ]
    //             },
    //             'shards': 1,
    //             '_id': 0
    //           }
    //         }
    //       ]
    //     }
    //   },
    //   {
    //     '$project': {
    //       'computedPlacement': {
    //         '$ifNull': [{'$first': '$computedPlacement.shards'}, []]
    //       },
    //       'isComputedPlacementAccurate': {
    //         '$ifNull': [{'$first': '$metadataFromInitDoc.consistentMetadataAvailable'}, false]
    //       },
    //       'placementAtInitTime': {
    //         '$ifNull': [{'$first': '$metadataFromInitDoc.shards'}, null]
    //       }
    //     }
    //   }
    // ]
    // ************************************************************
    // Full pipeline for database & cluster level search:
    // [
    //   {
    //     '$facet': {
    //       'computedPlacement': [
    //         {
    //           '$match': {
    //             'timestamp': {
    //               '$lte': '<atClusterTime>'
    //             },
    //                      ||            DB Level             || Cluster Level
    //               'nss': || {'$regex': '^dbName(\\..*)?$'}  || {'$ne': '.'}
    //           }
    //         },
    //         {
    //           '$sort': {'timestamp': -1}
    //         },
    //         {
    //           '$group': {
    //             '_id': '$nss',
    //             'shards': {
    //               '$first': '$shards'
    //             }
    //           }
    //         },
    //         {
    //           '$unwind': '$shards'
    //         },
    //         {
    //           '$group': {
    //             '_id': '',
    //             'shards': {
    //               '$addToSet': '$shards'
    //             }
    //           }
    //         },
    //       ],
    //       'metadataFromInitDoc': [
    //         {
    //           '$match': {
    //             'timestamp': {
    //               '$lte': '<atClusterTime>'
    //               },
    //             'nss': '.'
    //           }
    //         },
    //         {
    //           '$sort': {'timestamp': -1}
    //         },
    //         {
    //           '$limit': 1
    //         },
    //         {
    //           '$project': {
    //             'consistentMetadataAvailable': {
    //               '$eq': [
    //                 {
    //                   '$size': '$shards'
    //                 },
    //                 0
    //               ]
    //             },
    //             'shards': 1,
    //             '_id': 0
    //           }
    //         }
    //       ]
    //     }
    //   },
    //   {
    //     '$project': {
    //       'computedPlacement': {
    //         '$ifNull': [{'$first': '$computedPlacement.shards'}, []]
    //       },
    //       'isComputedPlacementAccurate': {
    //         '$ifNull': [{'$first': '$metadataFromInitDoc.consistentMetadataAvailable'}, false]
    //       },
    //       'placementAtInitTime': {
    //         '$ifNull': [{'$first': '$metadataFromInitDoc.shards'}, null]
    //       }
    //     }
    //   }
    // ]
    HistoricalPlacement getHistoricalPlacementStrictMode(Timestamp atClusterTime) {
        // Compose sub pipelines, which will be used as facets in the main pipeline.
        auto initializationDocumentSubPipeline =
            _buildInitializationDocumentSubPipeline(atClusterTime);
        auto retrievePlacementSubPipeline = _buildRetrievePlacementSubPipeline(atClusterTime);

        // Compose and execute the main pipeline using the two facets created above.
        auto mainPipeline = _buildMainPipeline(std::move(initializationDocumentSubPipeline),
                                               std::move(retrievePlacementSubPipeline));

        auto aggResult = _runLocalCatalogSnapshotQuery(*mainPipeline);
        tassert(
            10748901,
            "Placement query inside _configsvrGetHistoricalPlacement command must return a single "
            "document",
            aggResult.size() == 1);

        const auto& result = aggResult.front();
        LOGV2_DEBUG(10719401,
                    1,
                    "getHistoricalPlacement() query completed",
                    "nss"_attr = _nss ? _nss->toStringForErrorMsg() : "whole cluster",
                    "atClusterTime"_attr = atClusterTime,
                    "result"_attr = result);

        // Finally, process the result of the aggregation.
        // The expected schema of its only document is:
        // {
        //   computedPlacement: <arrayofShardIds>,
        //   isComputedPlacementAccurate: <bool>,
        //   placementAtInitTime: <arrayofShardIds or null>,
        // }
        const auto isComputedPlacementAccurate =
            result.getBoolField("isComputedPlacementAccurate"_sd);

        // No placement data may be returned if no response may be computed 'atClusterTime' and no
        // approximate data is available from the initialization doc.
        if (!isComputedPlacementAccurate && result.getField("placementAtInitTime"_sd).isNull()) {
            return HistoricalPlacement{{}, HistoricalPlacementStatus::NotAvailable};
        }

        const StringData sourceField =
            isComputedPlacementAccurate ? "computedPlacement"_sd : "placementAtInitTime"_sd;

        std::vector<ShardId> shardIds = [&]() {
            // Extract all shard ids from the 'fieldName' field of 'result'. The 'sourceField' field
            // value is expected to be an array of string values.
            std::vector<ShardId> shards;
            auto shardsArray = result.getField(sourceField).Obj();
            for (const auto& shardObj : shardsArray) {
                shards.push_back(shardObj.String());
            }
            return shards;
        }();

        HistoricalPlacement historicalPlacementResult;
        historicalPlacementResult.setStatus(HistoricalPlacementStatus::OK);
        historicalPlacementResult.setShards(std::move(shardIds));
        return historicalPlacementResult;
    }

    HistoricalPlacement getHistoricalPlacementIgnoreRemovedShardsMode(Timestamp atClusterTime) {
        // Fetch ids of all shards that are present in the local copy of the shard registry. Sort
        // the shard ids because we will be doing a binary search in them later when we look for
        // removed shards.
        // We do not need to worry about a stale shard registry here because the shard registry is
        // reloaded whenever a shard is added or removed. The reloads happen under the same DDL lock
        // that is held when branching into the ExpectedResponseBuilder.
        const std::vector<ShardId> allAvailableShardIds = [this]() {
            auto shardIds = Grid::get(_opCtx)->shardRegistry()->getAllShardIds(_opCtx);
            std::sort(shardIds.begin(), shardIds.end());
            return shardIds;
        }();

        // Timestamp of the placement history initialization point. Initially empty, but may be
        // computed once. We keep the initialization point timestamp across multiple possible
        // iterations of the following while loop.
        boost::optional<Timestamp> placementHistoryInitializationPoint;

        // This value can be increased at the end of the following loop, so the loop can be executed
        // multiple times with increasing 'atStartOfSegment' values.
        Timestamp atStartOfSegment = atClusterTime;
        while (true) {
            HistoricalPlacement historicalPlacementResult =
                getHistoricalPlacementStrictMode(atStartOfSegment);

            if (historicalPlacementResult.getStatus() != HistoricalPlacementStatus::OK) {
                // Early exit here if the placement status is not available.
                // The status "FutureClusterTime" cannot occur here.
                return historicalPlacementResult;
            }

            // Remove all shards from 'historicalPlacementResult' that are not present anymore in
            // the current copy of the shard registry. The following call sets the 'shards' and
            // 'anyRemovedShardDetected' values in the 'historicalPlacementResult'.
            _removeAllNonAvailableShards(historicalPlacementResult, allAvailableShardIds);

            // If all shards are still present, return the result from the placement history as is.
            if (!historicalPlacementResult.getAnyRemovedShardDetected().value()) {
                if (atStartOfSegment > atClusterTime) {
                    // Set 'openCursorAt' in case we have advanced beyond the original
                    // 'atClusterTime' value.
                    historicalPlacementResult.setOpenCursorAt(atStartOfSegment);
                }
                return historicalPlacementResult;
            }

            // At least one of the required shards is missing.
            // We now need to continue to find the start and end of the segment for which there is
            // at least one shard available.
            const auto matchNssExpression = _buildMatchNssExpressionForIgnoreRemovedShardsMode();

            // Determine placement history initialization point if not yet computed. The
            // initialization point must always be present in the placement history, otherwise this
            // would violate a design invariant.
            // TODO SERVER-111901: There is currently the possibility of failing to retrieve an
            // initialization point upon the execution of the first call to the strict mode query.
            // This will be fixed by raising an exception in the strict mode query and triggering a
            // lazy initialization.
            if (!placementHistoryInitializationPoint.has_value()) {
                // Only calculate initialization point once per invocation.
                placementHistoryInitializationPoint = _findPlacementHistoryInitializationPoint();
            }

            // The end of the segment depends on whether the placement history initialization point
            // is before or after 'atStartOfSegment'.
            const boost::optional<Timestamp> nextPlacementChangedAt =
                [&]() -> boost::optional<Timestamp> {
                // If the query uses an 'atClusterTime' that predates the time of the last
                // initialization of the placement history ("placement history initialization
                // point"), then we cannot trust any placement document that falls within the
                // [atClusterTime, initializationPoint) range. In such a subcase it is then safe to
                // take the time of the most recent initialization as the "end of the segment".
                if (atStartOfSegment < *placementHistoryInitializationPoint) {
                    // The placement history initialization point is after the 'atStartOfSegment'
                    // timestamp, so the end of the segment is the placement history reset
                    // point.
                    return *placementHistoryInitializationPoint;
                }

                // The placement history initialization point is before or equal to
                // 'atStartOfSegment'. This means we need to find the timestamp of the next
                // placement history entry for the requested namespace with a greater
                // timestamp than the 'atStartOfSegment' timestamp.
                return _findNextPlacementHistoryEntryTimestamp(matchNssExpression,
                                                               atStartOfSegment);
            }();

            if (!historicalPlacementResult.getShards().empty()) {
                // At least any of the original shards is still present in the cluster's current
                // topology. The current 'atStartOfSegment' value is used as the start timestamp of
                // the segment.
                historicalPlacementResult.setOpenCursorAt(atStartOfSegment);
                historicalPlacementResult.setNextPlacementChangedAt(nextPlacementChangedAt);
                return historicalPlacementResult;
            }

            // None of the required shards are still present in the cluster's current topology.
            // We need to skip the current segment and find the next segment in which at least one
            // shard is still present.

            // There must always be a follow-up placement history entry for the requested namespace,
            // because a shard cannot be removed from the cluster topology without its associated
            // databases/collections have been moved over to other shards.
            tassert(11314303,
                    str::stream() << "expecting follow-up placement history entry for namespace "
                                  << (_nss ? _nss->toStringForErrorMsg() : "whole cluster")
                                  << " to be present in placement history",
                    nextPlacementChangedAt.has_value());

            // Restart query processing loop with updated 'atStartOfSegment' value in next
            // iteration.
            tassert(11314302,
                    "expecting increasing 'atStartOfSegment' value at end of 'ignoreRemovedShards' "
                    "loop "
                    "iteration",
                    *nextPlacementChangedAt > atStartOfSegment);

            atStartOfSegment = *nextPlacementChangedAt;
        }
    }

private:
    // Scope of a placement history query.
    enum class PlacementHistoryQueryScope {
        // Placement history query is made for a single collection.
        kCollectionLevelScope,

        // Placement history query is made for a single database.
        kDatabaseLevelScope,

        // Placement history query is made for the whole cluster.
        kClusterLevelScope,
    };

    // Builds a regex string that matches both the collection in the namespace or just its parent
    // database exactly.
    std::string _buildRegexForCollectionLevelSearch() const {
        return fmt::format("^{}(\\.{})?$",
                           pcre_util::quoteMeta(_nss->db_forSharding()),
                           pcre_util::quoteMeta(_nss->coll()));
    }

    // Builds a regex string that matches the database in the namespace exactly, plus all
    // collections inside this database.
    std::string _buildRegexForDatabaseLevelSearch() const {
        return fmt::format("^{}(\\..*)?$", pcre_util::quoteMeta(_nss->db_forSharding()));
    }

    // Builds the NamespaceString value for an empty namespace, which can be used to match the
    // special placement history initialization markers.
    static std::string _buildInitDocumentLabel() {
        static std::string namespaceString = NamespaceStringUtil::serialize(
            ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker,
            SerializationContext::stateDefault());
        return namespaceString;
    }

    // Executes the specified query pipeline against the local 'config.placementHistory' collection,
    // using a snapshot read and the initially set config time value.
    std::vector<BSONObj> _runLocalCatalogSnapshotQuery(const Pipeline& pipeline) const {
        auto aggRequest = AggregateCommandRequest(
            NamespaceString::kConfigsvrPlacementHistoryNamespace, pipeline.serializeToBson());

        // Use a snapshot read at the latest majority committed time to retrieve the most recent
        // results.
        const repl::ReadConcernArgs readConcern{_vcTime.configTime(),
                                                repl::ReadConcernLevel::kSnapshotReadConcern};
        auto aggResult =
            _localCatalogClient->runCatalogAggregation(_opCtx, aggRequest, readConcern);

        LOGV2_DEBUG(11314300,
                    3,
                    "querying placement history in _configsvrGetHistoricalPlacement",
                    "pipeline"_attr = pipeline.serializeToBson(),
                    "nss"_attr = _nss ? _nss->toStringForErrorMsg() : "whole cluster",
                    "result"_attr = aggResult);

        return aggResult;
    }

    // Determine the scope of the placement history query from namespace.
    PlacementHistoryQueryScope _getPlacementHistoryQueryScope() const {
        if (!_nss.has_value()) {
            return PlacementHistoryQueryScope::kClusterLevelScope;
        }
        if (_nss->isDbOnly()) {
            return PlacementHistoryQueryScope::kDatabaseLevelScope;
        }
        return PlacementHistoryQueryScope::kCollectionLevelScope;
    }

    // Finds the next placement history entry with a 'timestamp' value > 'ts' for the requested
    // namespace, and returns its 'timestamp' field value if present. Otherwise returns boost::none.
    boost::optional<Timestamp> _findNextPlacementHistoryEntryTimestamp(
        const BSONObj& matchNssExpression, Timestamp ts) const {
        // Builds the pipeline to the placement history entry with a 'timestamp' value > 'ts' for
        // the requested namespace.
        // The resulting pipeline is
        // [
        //     {$match: {timestamp: {$gt: <ts>}, nss: <matchExpression>}},
        //     {$sort: {timestamp: -1}},
        //     {$limit: 1},
        //     {$project: {timestamp: 1}}
        // ]
        // with <ts> being an arbitrary timestamp, and <matchExpression> being one of the following:
        // - {$ne: ""}                             for whole-cluster placement queries
        // - {$regex: "^<dbName>(\..*")?$}         for database-level placement queries
        // - {$regex: "^<dbName>(\.<collName>)?$}  for collection-level placement queries
        DocumentSourceContainer pipelineStages;

        // The index on 'config.placementHistory' is 'nss_1_timestamp_-1' and may not serve this
        // pipeline very well.
        // TODO SERVER-115207: investigate alternative pipelines or indexes to improve this query.
        pipelineStages.push_back(DocumentSourceMatch::create(
            BSON("timestamp" << BSON("$gt" << ts) << "nss" << matchNssExpression), _expCtx));
        pipelineStages.push_back(DocumentSourceSort::create(_expCtx, BSON("timestamp" << 1)));
        pipelineStages.push_back(DocumentSourceLimit::create(_expCtx, 1));
        pipelineStages.push_back(DocumentSourceProject::create(
            BSON("timestamp" << 1), _expCtx, DocumentSourceProject::kStageName));

        auto pipeline = Pipeline::create(std::move(pipelineStages), _expCtx);

        auto aggResult = _runLocalCatalogSnapshotQuery(*pipeline);
        if (!aggResult.empty()) {
            return aggResult.front().getField("timestamp"_sd).timestamp();
        }
        return boost::none;
    }

    // Builds the sub-pipeline used for retrieving the placement history initialization document
    // with a timestamp <= 'atClusterTime'.
    std::unique_ptr<Pipeline> _buildInitializationDocumentSubPipeline(
        Timestamp atClusterTime) const {
        // a. Get the most recent initialization doc that satisfies <= 'atClusterTime'.
        auto matchStage =
            DocumentSourceMatch::create(BSON("timestamp" << BSON("$lte" << atClusterTime) << "nss"
                                                         << _buildInitDocumentLabel()),
                                        _expCtx);
        auto sortStage = DocumentSourceSort::create(_expCtx, BSON("timestamp" << -1));
        auto limitStage = DocumentSourceLimit::create(_expCtx, 1);

        constexpr auto kConsistentMetadataAvailable = "consistentMetadataAvailable"_sd;

        // b. Reformat the content of the initialization document:
        //  - the 'kConsistentMetadataAvailable' field gives an indication on whether the
        //    initialization state 'atClusterTime' allows to retrieve accurate placement
        //    metadata through computation (the 2nd subpipeline)
        //  - the 'shards' field contains the full composition of cluster when the
        //    initialization process was run for the last time (and it can be used as an
        //    approximated response to this query when no computation is possible).
        auto projectStage = DocumentSourceProject::create(
            BSON("_id" << 0 << "shards" << 1 << kConsistentMetadataAvailable
                       << BSON("$eq" << BSON_ARRAY(BSON("$size" << "$shards") << 0))),
            _expCtx,
            DocumentSourceProject::kStageName);

        DocumentSourceContainer initDocSubPipelineStages;
        initDocSubPipelineStages.emplace_back(std::move(matchStage));
        initDocSubPipelineStages.emplace_back(std::move(sortStage));
        initDocSubPipelineStages.emplace_back(std::move(limitStage));
        initDocSubPipelineStages.emplace_back(std::move(projectStage));

        return Pipeline::create(std::move(initDocSubPipelineStages), _expCtx);
    }

    // Builds the sub-pipeline used to retrieve the placement information for the queried namespace
    // with a timestamp <= 'atClusterTime'.
    std::unique_ptr<Pipeline> _buildRetrievePlacementSubPipeline(Timestamp atClusterTime) const {
        const auto scope = _getPlacementHistoryQueryScope();

        // The shape of the pipeline varies depending on the search level.
        DocumentSourceContainer placementSearchSubPipelineStages;

        if (scope == PlacementHistoryQueryScope::kCollectionLevelScope) {
            // a. Retrieve the most recent placement doc recorded for both the collection and its
            // parent database (note: docs may not be present when the collection was untracked or
            // the database/collection were not existing 'atClusterTime').
            auto matchStage = DocumentSourceMatch::create(
                BSON("nss" << BSON("$regex" << _buildRegexForCollectionLevelSearch()) << "timestamp"
                           << BSON("$lte" << atClusterTime)),
                _expCtx);

            auto sortByTimestampStage =
                DocumentSourceSort::create(_expCtx, BSON("timestamp" << -1));

            auto groupByNamespaceAndTakeLatestPlacementSpec =
                BSON("_id" << "$nss" << "shards" << BSON("$first" << "$shards"));
            auto groupByNamespaceStage = DocumentSourceGroup::createFromBson(
                BSON(DocumentSourceGroup::kStageName
                     << std::move(groupByNamespaceAndTakeLatestPlacementSpec))
                    .firstElement(),
                _expCtx);

            // b. Select the placement information about the collection or fallback to the parent
            // database; placement information containing an empty set of shards have to be
            // discarded (they describe a dropped namespace, which may be treated as a "non existing
            // one"). The sorting is done over the _id field, which contains nss value, as a
            // consequence of the projection performed by sortGroupsByNamespaceStage.
            auto sortGroupsByNamespaceStage =
                DocumentSourceSort::create(_expCtx, BSON("_id" << -1));
            auto filterOutDroppedNamespacesStage =
                DocumentSourceMatch::create(BSON("shards" << BSON("$ne" << BSONArray())), _expCtx);
            auto takeFirstNamespaceGroupStage = DocumentSourceLimit::create(_expCtx, 1);

            placementSearchSubPipelineStages.emplace_back(std::move(matchStage));
            placementSearchSubPipelineStages.emplace_back(std::move(sortByTimestampStage));
            placementSearchSubPipelineStages.emplace_back(std::move(groupByNamespaceStage));
            placementSearchSubPipelineStages.emplace_back(std::move(sortGroupsByNamespaceStage));
            placementSearchSubPipelineStages.emplace_back(
                std::move(filterOutDroppedNamespacesStage));
            placementSearchSubPipelineStages.emplace_back(std::move(takeFirstNamespaceGroupStage));
            return Pipeline::create(std::move(placementSearchSubPipelineStages), _expCtx);
        }

        tassert(10719400,
                "Unexpected kind of search detected",
                scope == PlacementHistoryQueryScope::kDatabaseLevelScope ||
                    scope == PlacementHistoryQueryScope::kClusterLevelScope);

        // a. Retrieve the latest placement information for each collection and database that falls
        // within the search criteria.
        const auto matchNssExpression = [&] {
            if (scope == PlacementHistoryQueryScope::kClusterLevelScope) {
                // Only discard documents containing 'config.placementHistory' initialization
                // metadata.
                return BSON("$ne" << _buildInitDocumentLabel());
            }

            // Capture documents about the database itself and any tracked collection under it.
            return BSON("$regex" << _buildRegexForDatabaseLevelSearch());
        }();

        auto matchStage = DocumentSourceMatch::create(
            BSON("nss" << matchNssExpression << "timestamp" << BSON("$lte" << atClusterTime)),
            _expCtx);

        auto sortByTimestampStage = DocumentSourceSort::create(_expCtx, BSON("timestamp" << -1));
        auto groupByNamespaceAndTakeLatestPlacementSpec =
            BSON("_id" << "$nss" << "shards" << BSON("$first" << "$shards"));
        auto groupByNamespaceStage = DocumentSourceGroup::createFromBson(
            BSON(DocumentSourceGroup::kStageName
                 << std::move(groupByNamespaceAndTakeLatestPlacementSpec))
                .firstElement(),
            _expCtx);

        // b. Merge the placement of each matched namespace into a single set field.
        auto unwindShardsStage =
            DocumentSourceUnwind::create(_expCtx,
                                         std::string("shards"),
                                         false /* preserveNullAndEmptyArrays */,
                                         boost::none /* indexPath */);

        auto mergeAllGroupsInASingleShardListSpec =
            BSON("_id" << "" << "shards" << BSON("$addToSet" << "$shards"));

        auto mergeAllGroupsStage = DocumentSourceGroup::createFromBson(
            BSON(DocumentSourceGroup::kStageName << std::move(mergeAllGroupsInASingleShardListSpec))
                .firstElement(),
            _expCtx);

        placementSearchSubPipelineStages.emplace_back(std::move(matchStage));
        placementSearchSubPipelineStages.emplace_back(std::move(sortByTimestampStage));
        placementSearchSubPipelineStages.emplace_back(std::move(groupByNamespaceStage));
        placementSearchSubPipelineStages.emplace_back(std::move(unwindShardsStage));
        placementSearchSubPipelineStages.emplace_back(std::move(mergeAllGroupsStage));

        return Pipeline::create(std::move(placementSearchSubPipelineStages), _expCtx);
    }

    std::unique_ptr<Pipeline> _buildMainPipeline(
        std::unique_ptr<Pipeline> initializationDocumentSubPipeline,
        std::unique_ptr<Pipeline> retrievePlacementSubPipeline) const {

        constexpr auto kInitMetadataRetrievalSubPipelineName = "metadataFromInitDoc"_sd;
        constexpr auto kPlacementRetrievalSubPipelineName = "computedPlacement"_sd;

        // Compose the main aggregation; first, combine the two sub pipelines within a $facets
        // stage...
        auto facetStageBson = BSON(kInitMetadataRetrievalSubPipelineName
                                   << initializationDocumentSubPipeline->serializeToBson()
                                   << kPlacementRetrievalSubPipelineName
                                   << retrievePlacementSubPipeline->serializeToBson());
        auto facetStage = DocumentSourceFacet::createFromBson(
            BSON(DocumentSourceFacet::kStageName << std::move(facetStageBson)).firstElement(),
            _expCtx);

        // ... then merge the results into a single document.
        // Each subpipeline is expected to produce an array containing at most one element:
        // - If no initialization document may be retrieved, produce the proper fields to express
        //   that the response cannot be neither computed nor approximated;
        // - If the computation of the placement returns an empty result, this means that there was
        //   no existing collection/database at the requested 'atClusterTime'; this state gets
        //   remapped into an empty set of shards.
        auto projectStageBson =
            BSON("isComputedPlacementAccurate"
                 << BSON("$ifNull" << BSON_ARRAY(
                             BSON("$first" << "$metadataFromInitDoc.consistentMetadataAvailable")
                             << false))
                 << "placementAtInitTime"
                 << BSON("$ifNull"
                         << BSON_ARRAY(BSON("$first" << "$metadataFromInitDoc.shards") << BSONNULL))
                 << "computedPlacement"
                 << BSON("$ifNull" << BSON_ARRAY(BSON("$first" << "$computedPlacement.shards")
                                                 << BSONArray())));
        auto projectStage = DocumentSourceProject::create(
            projectStageBson, _expCtx, DocumentSourceProject::kStageName);

        return Pipeline::create({std::move(facetStage), std::move(projectStage)}, _expCtx);
    }

    // Removes all shard ids from the 'HistoricalPlacement' that are not present in the
    // 'allAvailableShardIds' vector. The 'allAvailableShardIds' vector values must be sorted. Also
    // sets the 'anyRemovedShardDetected' value of the 'HistoricalPlacement' value as a side-effect.
    void _removeAllNonAvailableShards(HistoricalPlacement& historicalPlacementResult,
                                      const std::vector<ShardId>& allAvailableShardIds) {
        // Intentionally create a copy here, because we are about to remove shards from the vector.
        auto shardIds = historicalPlacementResult.getShards();
        const size_t originalNumberOfShards = shardIds.size();
        std::erase_if(shardIds, [&](const ShardId& shardId) {
            return !std::binary_search(
                allAvailableShardIds.begin(), allAvailableShardIds.end(), shardId);
        });
        historicalPlacementResult.setAnyRemovedShardDetected(shardIds.size() <
                                                             originalNumberOfShards);
        historicalPlacementResult.setShards(std::move(shardIds));
    }

    // Builds a partial match expression to be used on the 'nss' field for a placement history query
    // in ignoreRemovedShards mode.
    BSONObj _buildMatchNssExpressionForIgnoreRemovedShardsMode() const {
        switch (_getPlacementHistoryQueryScope()) {
            case PlacementHistoryQueryScope::kClusterLevelScope:
                // Only discard documents containing 'config.placementHistory' initialization
                // metadata.
                return BSON("$ne" << _buildInitDocumentLabel());
            case PlacementHistoryQueryScope::kDatabaseLevelScope:
                // Capture documents about the database itself and any tracked collection under it.
                return BSON("$regex" << _buildRegexForDatabaseLevelSearch());
            case PlacementHistoryQueryScope::kCollectionLevelScope:
                // Capture documents about the collection and its parent database.
                return BSON("$regex" << _buildRegexForCollectionLevelSearch());
        }

        MONGO_UNREACHABLE_TASSERT(11314304);
    }

    // Find placement history initialization point. The placement history initialization point has
    // an 'nss' value equal to the empty string, a 'shards' value equal to an empty array, and a
    // 'timestamp' value greater than (0, 1). There must always be a single such entry present.
    // The placement history initialization and cleanup procedures ensure this. They remove the
    // existing entries for the initialization point and the approximated response, and inserts the
    // new documents for the initialization point and the approximated response inside a single
    // transaction, which is executed atomically.
    Timestamp _findPlacementHistoryInitializationPoint() {
        DocumentSourceContainer pipelineStages;

        pipelineStages.push_back(DocumentSourceMatch::create(
            BSON("timestamp" << BSON("$gt" << Timestamp(0, 1)) << "nss" << _buildInitDocumentLabel()
                             << "shards" << BSON("$eq" << BSONArray())),
            _expCtx));
        pipelineStages.push_back(DocumentSourceLimit::create(_expCtx, 1));
        pipelineStages.push_back(DocumentSourceProject::create(
            BSON("timestamp" << 1), _expCtx, DocumentSourceProject::kStageName));

        auto pipeline = Pipeline::create(std::move(pipelineStages), _expCtx);

        auto aggResult = _runLocalCatalogSnapshotQuery(*pipeline);
        tassert(11314301,
                "expecting placement history initialization point to be present",
                aggResult.size() == 1);
        return aggResult.front().getField("timestamp"_sd).timestamp();
    }

    // OperationContext used for executing operations. Managed externally, but guaranteed to remain
    // valid for the lifetime of the placement fetcher.
    OperationContext* _opCtx;

    // Namespace used for the placement history query.
    const boost::optional<NamespaceString> _nss;

    // Vector clock time used for the "config.placementHistory" snapshot read.
    const VectorClock::VectorTime _vcTime;

    // Catalog client used for local reads of "config.placementHistory" collection. Managed
    // externally, but guaranteed to remain valid for the lifetime of the placement fetcher.
    ShardingCatalogClient* _localCatalogClient;

    // ExpressionContext used for building pipelines inside the placement fetcher.
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

}  // namespace

Status ShardingCatalogManager::createIndexForConfigPlacementHistory(OperationContext* opCtx) {
    return createIndexOnConfigCollection(opCtx,
                                         NamespaceString::kConfigsvrPlacementHistoryNamespace,
                                         BSON(NamespacePlacementType::kNssFieldName
                                              << 1 << NamespacePlacementType::kTimestampFieldName
                                              << -1),
                                         true /*unique*/);
}

write_ops::InsertCommandRequest
ShardingCatalogManager::buildInsertReqForPlacementHistoryOperationalBoundaries(
    const Timestamp& initializationTime, const std::vector<ShardId>& defaultPlacement) {
    /*
     * The 'operational boundaries' of config.placementHistory are described through two 'metadata'
     * documents, both identified by the kConfigPlacementHistoryInitializationMarker namespace:
     * - initializationTimeInfo: contains the time of the initialization and an empty set of shards.
     *   It will allow getHistoricalPlacement() to serve accurate responses to queries targeting a
     * PIT within the [initializationTime, +inf) range.
     * - approximatedPlacementForPreInitQueries:  contains the cluster topology at the time of the
     *   initialization and it associated with the 'Dawn of Time' Timestamp(0,1).
     *   It will allow getHistoricalPlacement() to serve approximated responses to queries
     *   concerning the [-inf, initializationTime) range.
     */
    NamespacePlacementType initializationTimeInfo;
    initializationTimeInfo.setNss(
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker);
    initializationTimeInfo.setTimestamp(initializationTime);
    initializationTimeInfo.setShards({});

    NamespacePlacementType approximatedPlacementForPreInitQueries;
    approximatedPlacementForPreInitQueries.setNss(
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker);
    approximatedPlacementForPreInitQueries.setTimestamp(Timestamp(0, 1));
    approximatedPlacementForPreInitQueries.setShards(defaultPlacement);

    write_ops::InsertCommandRequest insertMarkerRequest(
        NamespaceString::kConfigsvrPlacementHistoryNamespace);
    insertMarkerRequest.setDocuments(
        {initializationTimeInfo.toBSON(), approximatedPlacementForPreInitQueries.toBSON()});
    return insertMarkerRequest;
}

HistoricalPlacement ShardingCatalogManager::getHistoricalPlacement(
    OperationContext* opCtx,
    const boost::optional<NamespaceString>& nss,
    const Timestamp& atClusterTime,
    bool checkIfPointInTimeIsInFuture,
    bool ignoreRemovedShards) {

    uassert(ErrorCodes::InvalidOptions,
            "unsupported namespace for historical placement query",
            !nss || (!nss->dbName().isEmpty() && !nss->isOnInternalDb()));

    // Acquire a shared lock on config.placementHistory to serialize incoming queries with
    // inflight attempts to modify its content through InitializePlacementHistoryCoordinator
    // (this requires making this read operation interruptible).
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
    DDLLockManager::ScopedBaseDDLLock placementHistoryLock(
        opCtx,
        shard_role_details::getLocker(opCtx),
        NamespaceString::kConfigsvrPlacementHistoryNamespace,
        "getHistoricalPlacement" /* reason */,
        MODE_IS,
        true /*waitForRecovery*/);

    // If 'atClusterTime' is greater than current config time, then we can not return correct
    // placement history and early exit with HistoricalPlacementStatus::FutureClusterTime.
    const auto vcTime = VectorClock::get(opCtx)->getTime();
    if (checkIfPointInTimeIsInFuture && atClusterTime > vcTime.configTime().asTimestamp()) {
        return HistoricalPlacement{{}, HistoricalPlacementStatus::FutureClusterTime};
    }

    HistoricalPlacementReader reader(opCtx, nss, vcTime, _localCatalogClient.get());
    if (ignoreRemovedShards) {
        return reader.getHistoricalPlacementIgnoreRemovedShardsMode(atClusterTime);
    }
    return reader.getHistoricalPlacementStrictMode(atClusterTime);
}

void ShardingCatalogManager::initializePlacementHistory(OperationContext* opCtx,
                                                        const Timestamp& initializationTime) {
    /**
     * This function will perform the following steps:
     * 1. Collect a consistent description of the placement of each existing namespace through a
     *    snapshot read of the sharding catalog at initializationTime and appending it to
     *    config.placementHistory through a single aggregation.
     * 2. Rewrite the 'initialization metadata' documents with updated values for
     *    initializationTime and the full list of shards (a.k.a. the fallback response to
     *    getPlacementHistory() calls referencing older points in time) through a transaction.
     *
     * On the other hand, it won't delete any other pre-existing placement content, including
     * placement information generated by previous (and possibly incomplete, due to stepdowns)
     * initialization. Such an action is either delegated to the caller (who has the responsibility
     * of enforcing the serialization properties required by the use case) or deferred to the
     * PlacementHistoryCleaner background job.
     */

    // Create an alternative opCtx to perform the needed snapshot reads without polluting the state
    // of object passed in by the caller; Internal auth credentials will be also applied to ensure
    // that the $merge stage included in the first step is able to write into the config db.
    auto altClient = opCtx->getServiceContext()
                         ->getService(ClusterRole::ShardServer)
                         ->makeClient("initializePlacementHistory");

    AuthorizationSession::get(altClient.get())->grantInternalAuthorization();
    AlternativeClientRegion acr(altClient);
    auto executor = Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();

    CancelableOperationContext snapshotReadsOpCtx(
        cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

    repl::ReadConcernArgs snapshotReadConcern(repl::ReadConcernLevel::kSnapshotReadConcern);
    snapshotReadConcern.setArgsAtClusterTimeForSnapshot(initializationTime);

    //  Step 1.
    {
        auto createInitialSnapshotAggReq = createInitPlacementHistoryAggregationRequest(
            snapshotReadsOpCtx.get(), initializationTime);
        createInitialSnapshotAggReq.setUnwrappedReadPref({});
        createInitialSnapshotAggReq.setReadConcern(snapshotReadConcern);
        createInitialSnapshotAggReq.setWriteConcern({});

        initializePlacementHistoryHangAfterSettingSnapshotReadConcern.pauseWhileSet();

        auto noopCallback = [](const std::vector<BSONObj>& batch,
                               const boost::optional<BSONObj>& postBatchResumeToken) {
            return true;
        };

        auto noopOnRetry = [](const Status&) {
        };

        // This aggregation cannot be retried because it performs writes in one of its steps.
        // Restarting the aggregation process would potentially perform the write again, so we
        // prohibit retries for this one.
        // TODO(SERVER-113598): Consider finding a way to not need noop callbacks when no retries
        // are needed.
        Status status = _localConfigShard->runAggregation(snapshotReadsOpCtx.get(),
                                                          createInitialSnapshotAggReq,
                                                          Shard::RetryPolicy::kNoRetry,
                                                          noopCallback,
                                                          noopOnRetry);
        uassertStatusOK(status);
    }

    // Step 2.
    {
        auto findAllShardsReq = findAllShardsAggRequest(snapshotReadsOpCtx.get());
        findAllShardsReq.setUnwrappedReadPref({});
        findAllShardsReq.setReadConcern(snapshotReadConcern);

        std::vector<ShardId> shardsAtInitializationTime;
        auto consumeBatchResponse = [&](const auto& batch, const boost::optional<BSONObj>&) {
            for (const auto& doc : batch) {
                shardsAtInitializationTime.emplace_back(
                    ShardId(std::string(doc.getStringField(ShardType::name.name()))));
            }

            return true;
        };

        auto resetOnRetriableFailure = [&](const Status& status) {
            shardsAtInitializationTime.clear();
        };

        uassertStatusOK(
            _localConfigShard->runAggregation(snapshotReadsOpCtx.get(),
                                              findAllShardsReq,
                                              Shard::RetryPolicy::kStrictlyNotIdempotent,
                                              consumeBatchResponse,
                                              resetOnRetriableFailure));

        setInitializationTimeOnPlacementHistory(
            opCtx, initializationTime, std::move(shardsAtInitializationTime));
    }
}

void ShardingCatalogManager::cleanUpPlacementHistory(OperationContext* opCtx,
                                                     const Timestamp& earliestClusterTime) {
    LOGV2(
        7068803, "Cleaning up placement history", "earliestClusterTime"_attr = earliestClusterTime);
    /*
     * The method implements the following optimistic approach for data cleanup:
     * 1. Set earliestOpTime as the new initialization time of config.placementHistory;
     * this will have the effect of hiding older(deletable) documents when the collection is queried
     * by the ShardingCatalogClient.
     * TODO SERVER-108231 validate the code against an empty set of shards and future values for
     * earliestClusterTime; add tassertions accordingly.
     */
    auto allShardIds = [&] {
        const auto clusterPlacementAtEarliestClusterTime =
            getHistoricalPlacement(opCtx,
                                   boost::none /*namespace*/,
                                   earliestClusterTime,
                                   true /* checkIfPointInTimeIsInFuture */,
                                   false /* ignoreRemovedShards */);
        return clusterPlacementAtEarliestClusterTime.getShards();
    }();

    setInitializationTimeOnPlacementHistory(opCtx, earliestClusterTime, std::move(allShardIds));

    /*
     * 2. Build up and execute the delete request to remove the disposable documents. This
     * operation is not atomic and it may be interrupted by a stepdown event, but we rely on the
     * fact that the cleanup is periodically invoked to ensure that the content in excess will be
     *    eventually deleted.
     *
     * 2.1 For each namespace represented in config.placementHistory, collect the timestamp of its
     *     most recent placement doc (initialization markers are not part of the output).
     *
     *     config.placementHistory.aggregate([
     *      {
     *          $group : {
     *              _id : "$nss",
     *              mostRecentTimestamp: {$max : "$timestamp"},
     *          }
     *      },
     *      {
     *          $match : {
     *              _id : { $ne : "kConfigPlacementHistoryInitializationMarker"}
     *          }
     *      }
     *  ])
     */
    auto pipeline = PipelineBuilder(opCtx,
                                    NamespaceString::kConfigsvrPlacementHistoryNamespace,
                                    {NamespaceString::kConfigsvrPlacementHistoryNamespace});

    pipeline.addStage<DocumentSourceGroup>(
        BSON("_id" << "$" + NamespacePlacementType::kNssFieldName << "mostRecentTimestamp"
                   << BSON("$max" << "$" + NamespacePlacementType::kTimestampFieldName)));
    pipeline.addStage<DocumentSourceMatch>(
        BSON("_id" << BSON("$ne" << NamespaceStringUtil::serialize(
                               ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker,
                               SerializationContext::stateDefault()))));

    auto aggRequest = pipeline.buildAsAggregateCommandRequest();

    repl::ReadConcernArgs readConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);
    aggRequest.setReadConcern(readConcernArgs);

    /*
     * 2.2 For each namespace found, compose a delete statement.
     */
    std::vector<write_ops::DeleteOpEntry> deleteStatements;
    auto callback = [&deleteStatements,
                     &earliestClusterTime](const std::vector<BSONObj>& batch,
                                           const boost::optional<BSONObj>& postBatchResumeToken) {
        deleteStatements.reserve(batch.size());
        for (const auto& obj : batch) {
            const auto nss = NamespaceStringUtil::deserialize(
                boost::none, obj["_id"].String(), SerializationContext::stateDefault());
            const auto timeOfMostRecentDoc = obj["mostRecentTimestamp"].timestamp();
            write_ops::DeleteOpEntry stmt;

            const auto minTimeToPreserve = std::min(timeOfMostRecentDoc, earliestClusterTime);
            stmt.setQ(
                BSON(NamespacePlacementType::kNssFieldName
                     << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
                     << NamespacePlacementType::kTimestampFieldName
                     << BSON("$lt" << minTimeToPreserve)));
            stmt.setMulti(true);
            deleteStatements.emplace_back(std::move(stmt));
        }
        return true;
    };

    auto onRetry = [&](const Status&) {
        deleteStatements.clear();
    };

    // TODO(SERVER-113416): Consider using kIdempotent since onRetry allows read only aggregation
    // processes to be restarted.
    uassertStatusOK(_localConfigShard->runAggregation(
        opCtx, aggRequest, Shard::RetryPolicy::kStrictlyNotIdempotent, callback, onRetry));

    LOGV2_DEBUG(7068806,
                2,
                "Cleaning up placement history - about to clean entries",
                "timestamp"_attr = earliestClusterTime,
                "numNssToClean"_attr = deleteStatements.size());

    /*
     * Send the delete request.
     */
    write_ops::DeleteCommandRequest deleteRequest(
        NamespaceString::kConfigsvrPlacementHistoryNamespace);
    deleteRequest.setDeletes(std::move(deleteStatements));
    uassertStatusOK(
        _localConfigShard->runCommand(opCtx,
                                      ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                      NamespaceString::kConfigsvrPlacementHistoryNamespace.dbName(),
                                      deleteRequest.toBSON(),
                                      Shard::RetryPolicy::kIdempotent));

    LOGV2_DEBUG(7068808, 2, "Cleaning up placement history - done deleting entries");
}

}  // namespace mongo
