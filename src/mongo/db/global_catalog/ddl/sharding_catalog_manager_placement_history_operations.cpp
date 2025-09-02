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
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/placement_history_cleaner.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
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
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/util/pcre_util.h"

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

        _expCtx->setResolvedNamespaces(resolvedNamespacesMap);
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

void setInitializationTimeOnPlacementHistory(
    OperationContext* opCtx,
    Timestamp initializationTime,
    std::vector<ShardId> placementResponseForPreInitQueries) {
    /*
     * The initialization metadata of config.placementHistory is composed by two special docs,
     * identified by kConfigPlacementHistoryInitializationMarker:
     * - initializationTimeInfo: contains the time of the initialization and an empty set of shards.
     *   It will allow getHistoricalPlacement() to serve accurate responses to queries concerning
     *   the [initializationTime, +inf) range.
     * - approximatedPlacementForPreInitQueries:  contains the cluster topology at the time of the
     *   initialization and is marked with Timestamp(0,1).
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
    approximatedPlacementForPreInitQueries.setShards(placementResponseForPreInitQueries);

    auto transactionChain = [initializationTimeInfo = std::move(initializationTimeInfo),
                             approximatedPlacementForPreInitQueries =
                                 std::move(approximatedPlacementForPreInitQueries)](
                                const txn_api::TransactionClient& txnClient,
                                ExecutorPtr txnExec) -> SemiFuture<void> {
        // Delete the current initialization metadata
        write_ops::DeleteCommandRequest deleteRequest(
            NamespaceString::kConfigsvrPlacementHistoryNamespace);
        write_ops::DeleteOpEntry entryDelMarker;
        entryDelMarker.setQ(
            BSON(NamespacePlacementType::kNssFieldName << NamespaceStringUtil::serialize(
                     ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker,
                     SerializationContext::stateDefault())));
        entryDelMarker.setMulti(true);
        deleteRequest.setDeletes({entryDelMarker});

        return txnClient.runCRUDOp(deleteRequest, {})
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& _) {
                // Insert the new initialization metadata
                write_ops::InsertCommandRequest insertMarkerRequest(
                    NamespaceString::kConfigsvrPlacementHistoryNamespace);
                insertMarkerRequest.setDocuments({initializationTimeInfo.toBSON(),
                                                  approximatedPlacementForPreInitQueries.toBSON()});
                return txnClient.runCRUDOp(insertMarkerRequest, {});
            })
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& _) { return; })
            .semi();
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
}  // namespace

HistoricalPlacement ShardingCatalogManager::getHistoricalPlacement(
    OperationContext* opCtx,
    const boost::optional<NamespaceString>& nss,
    const Timestamp& atClusterTime) {

    uassert(ErrorCodes::InvalidOptions,
            "unsupported namespace for historical placement query",
            !nss || (!nss->dbName().isEmpty() && !nss->isOnInternalDb()));
    // If 'atClusterTime' is greater than current config time, then we can not return correct
    // placement history and early exit with HistoricalPlacementStatus::FutureClusterTime.
    const auto vcTime = VectorClock::get(opCtx)->getTime();
    if (atClusterTime > vcTime.configTime().asTimestamp()) {
        return HistoricalPlacement{{}, HistoricalPlacementStatus::FutureClusterTime};
    }

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

    ResolvedNamespaceMap resolvedNamespaces;
    resolvedNamespaces[NamespaceString::kConfigsvrPlacementHistoryNamespace] = {
        NamespaceString::kConfigsvrPlacementHistoryNamespace,
        std::vector<BSONObj>() /* pipeline */};
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .ns(NamespaceString::kConfigsvrPlacementHistoryNamespace)
                      .resolvedNamespace(std::move(resolvedNamespaces))
                      .build();

    const auto kInitDocumentLabel = NamespaceStringUtil::serialize(
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker,
        SerializationContext::stateDefault());
    constexpr auto kConsistentMetadataAvailable = "consistentMetadataAvailable"_sd;

    // The aggregation is composed by 2 sub pipelines:
    // 1. The first sub pipeline pulls the 'config.placementHistory' initialization document related
    //    to the requested 'atClusterTime', whose content will be later processed to determine
    //    if the query may be served (and if so, through which data).
    auto initializationDocumentSubPipeline = [&] {
        DocumentSourceContainer initDocSubPipelineStages;
        // a. Get the most recent initialization doc that satisfies <= 'atClusterTime'.
        auto matchStage = DocumentSourceMatch::create(
            BSON("timestamp" << BSON("$lte" << atClusterTime) << "nss" << kInitDocumentLabel),
            expCtx);
        auto sortStage = DocumentSourceSort::create(expCtx, BSON("timestamp" << -1));
        auto limitStage = DocumentSourceLimit::create(expCtx, 1);

        // b. Reformat the content of the initialization document:
        //  - the 'kConsistentMetadataAvailable' field gives an indication on whether the
        //    initialization state 'atClusterTime' allows to retrieve accurate placement metadata
        //    through computation (the 2nd subpipeline)
        //  - the 'shards' field contains the full composition of cluster when the initialization
        //    process was run for the last time (and it can be used as an approximated response to
        //    this query when no computation is possible).
        auto projectStage = DocumentSourceProject::create(
            BSON("_id" << 0 << "shards" << 1 << kConsistentMetadataAvailable
                       << BSON("$eq" << BSON_ARRAY(BSON("$size" << "$shards") << 0))),
            expCtx,
            DocumentSourceProject::kStageName);

        initDocSubPipelineStages.emplace_back(std::move(matchStage));
        initDocSubPipelineStages.emplace_back(std::move(sortStage));
        initDocSubPipelineStages.emplace_back(std::move(limitStage));
        initDocSubPipelineStages.emplace_back(std::move(projectStage));

        return Pipeline::create(std::move(initDocSubPipelineStages), expCtx);
    }();

    // 2. The second sub pipeline retrieves the set of data bearing shards for the nss/whole cluster
    //    at the requested time, combining the placement metadata of each collection/database name
    //    that matches the search criteria.
    auto retrievePlacementSubPipeline = [&] {
        // The shape of the pipeline varies depending on the search level.
        const auto clusterLevelSearch = !nss.has_value();
        const auto dbLevelSearch = !clusterLevelSearch && nss->isDbOnly();
        const auto collectionLevelSearch = !clusterLevelSearch && !dbLevelSearch;
        DocumentSourceContainer placementSearchSubPipelineStages;

        if (collectionLevelSearch) {
            // a. Retrieve the most recent placement doc recorded for both the collection and its
            //    parent database (note: docs may not be present when the collection was untracked
            //    or the database/collection were not existing 'atClusterTime').
            auto matchCollAndParentDbExpr = "^" + pcre_util::quoteMeta(nss->db_forSharding()) +
                "(\\." + pcre_util::quoteMeta(nss->coll()) + ")?$";

            auto matchStage = DocumentSourceMatch::create(
                BSON("nss" << BSON("$regex" << matchCollAndParentDbExpr) << "timestamp"
                           << BSON("$lte" << atClusterTime)),
                expCtx);

            auto sortByTimestampStage = DocumentSourceSort::create(expCtx, BSON("timestamp" << -1));

            auto groupByNamespaceAndTakeLatestPlacementSpec =
                BSON("_id" << "$nss" << "shards" << BSON("$first" << "$shards"));
            auto groupByNamespaceStage = DocumentSourceGroup::createFromBson(
                BSON(DocumentSourceGroup::kStageName
                     << std::move(groupByNamespaceAndTakeLatestPlacementSpec))
                    .firstElement(),
                expCtx);

            // b. Select the placement information about the collection or fallback to the parent
            //    database; placement information containing an empty set of shards have to be
            //    discarded (they describe a dropped namespace, which may be treated as a "non
            //    existing one").
            //    The sorting is done over the _id field, which contains nss value, as a consequence
            //    of the projection performed by sortGroupsByNamespaceStage.
            auto sortGroupsByNamespaceStage = DocumentSourceSort::create(expCtx, BSON("_id" << -1));
            auto filterOutDroppedNamespacesStage =
                DocumentSourceMatch::create(BSON("shards" << BSON("$ne" << BSONArray())), expCtx);
            auto takeFirstNamespaceGroupStage = DocumentSourceLimit::create(expCtx, 1);

            placementSearchSubPipelineStages.emplace_back(std::move(matchStage));
            placementSearchSubPipelineStages.emplace_back(std::move(sortByTimestampStage));
            placementSearchSubPipelineStages.emplace_back(std::move(groupByNamespaceStage));
            placementSearchSubPipelineStages.emplace_back(std::move(sortGroupsByNamespaceStage));
            placementSearchSubPipelineStages.emplace_back(
                std::move(filterOutDroppedNamespacesStage));
            placementSearchSubPipelineStages.emplace_back(std::move(takeFirstNamespaceGroupStage));
            return Pipeline::create(std::move(placementSearchSubPipelineStages), expCtx);
        }

        tassert(
            10719400, "Unexpected kind of search detected", dbLevelSearch || clusterLevelSearch);
        // a. Retrieve the latest placement information for each collection and database that falls
        // within
        //    the search criteria.
        auto matchNssExpression = [&] {
            if (clusterLevelSearch) {
                // Only discard documents containing 'config.placementHistory' initialization
                // metadata.
                return BSON("$ne" << kInitDocumentLabel);
            }

            // Capture documents about the database itself and any tracked collection under it.
            auto matchDbAndCollectionsExpr =
                "^" + pcre_util::quoteMeta(nss->db_forSharding()) + "(\\..*)?$";
            return BSON("$regex" << matchDbAndCollectionsExpr);
        }();

        auto matchStage = DocumentSourceMatch::create(
            BSON("nss" << matchNssExpression << "timestamp" << BSON("$lte" << atClusterTime)),
            expCtx);

        auto sortByTimestampStage = DocumentSourceSort::create(expCtx, BSON("timestamp" << -1));
        auto groupByNamespaceAndTakeLatestPlacementSpec =
            BSON("_id" << "$nss" << "shards" << BSON("$first" << "$shards"));
        auto groupByNamespaceStage = DocumentSourceGroup::createFromBson(
            BSON(DocumentSourceGroup::kStageName
                 << std::move(groupByNamespaceAndTakeLatestPlacementSpec))
                .firstElement(),
            expCtx);

        // b. Merge the placement of each matched namespace into a single set field.
        auto unwindShardsStage =
            DocumentSourceUnwind::create(expCtx,
                                         std::string("shards"),
                                         false /* preserveNullAndEmptyArrays */,
                                         boost::none /* indexPath */);

        auto mergeAllGroupsInASingleShardListSpec =
            BSON("_id" << "" << "shards" << BSON("$addToSet" << "$shards"));

        auto mergeAllGroupsStage = DocumentSourceGroup::createFromBson(
            BSON(DocumentSourceGroup::kStageName << std::move(mergeAllGroupsInASingleShardListSpec))
                .firstElement(),
            expCtx);

        auto projectStage = DocumentSourceProject::create(
            BSON("_id" << 0 << "shards" << 1), expCtx, DocumentSourceProject::kStageName);

        placementSearchSubPipelineStages.emplace_back(std::move(matchStage));
        placementSearchSubPipelineStages.emplace_back(std::move(sortByTimestampStage));
        placementSearchSubPipelineStages.emplace_back(std::move(groupByNamespaceStage));
        placementSearchSubPipelineStages.emplace_back(std::move(unwindShardsStage));
        placementSearchSubPipelineStages.emplace_back(std::move(mergeAllGroupsStage));
        return Pipeline::create(std::move(placementSearchSubPipelineStages), expCtx);
    }();

    // Compose the main aggregation; first, combine the two sub pipelines within a $facets stage...
    constexpr auto kInitMetadataRetrievalSubPipelineName = "metadataFromInitDoc"_sd;
    constexpr auto kPlacementRetrievalSubPipelineName = "computedPlacement"_sd;
    const auto mainPipeline = [&] {
        auto facetStageBson = BSON(kInitMetadataRetrievalSubPipelineName
                                   << initializationDocumentSubPipeline->serializeToBson()
                                   << kPlacementRetrievalSubPipelineName
                                   << retrievePlacementSubPipeline->serializeToBson());
        auto facetStage = DocumentSourceFacet::createFromBson(
            BSON(DocumentSourceFacet::kStageName << std::move(facetStageBson)).firstElement(),
            expCtx);

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
            projectStageBson, expCtx, DocumentSourceProject::kStageName);

        return Pipeline::create({std::move(facetStage), std::move(projectStage)}, expCtx);
    }();

    // 4. Run the aggregation.
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
    auto aggRequest = AggregateCommandRequest(NamespaceString::kConfigsvrPlacementHistoryNamespace,
                                              mainPipeline->serializeToBson());

    // Use a snapshot read at the latest majority committed time to retrieve the most recent
    // results.
    const repl::ReadConcernArgs readConcern{vcTime.configTime(),
                                            repl::ReadConcernLevel::kSnapshotReadConcern};
    auto aggrResult = _localCatalogClient->runCatalogAggregation(opCtx, aggRequest, readConcern);
    tassert(10748901,
            "GetHistoricalPlacement command must return a single document",
            aggrResult.size() == 1);

    const auto& result = aggrResult.front();
    LOGV2_DEBUG(10719401,
                1,
                "getHistoricalPlacement() query completed",
                "nss"_attr = nss ? nss->toStringForErrorMsg() : "whole cluster",
                "atClusterTime"_attr = atClusterTime,
                "result"_attr = result);

    // Finally, process the result of the aggregation.
    // The expected schema of its only document is:
    // {
    //   computedPlacement: <arrayofShardIds>,
    //   isComputedPlacementAccurate: <bool>,
    //   placementAtInitTime: <arrayofShardIds or null>,
    // }
    const auto isComputedPlacementAccurate = result.getBoolField("isComputedPlacementAccurate");
    // No placement data may be returned if no response may be computed 'atClusterTime'
    // and no approximate data is available from the initialization doc.
    if (!isComputedPlacementAccurate && result.getField("placementAtInitTime").isNull()) {
        return HistoricalPlacement{{}, HistoricalPlacementStatus::NotAvailable};
    }

    auto extractShardIds = [&](const auto& fieldName) {
        std::vector<ShardId> shards;
        auto shardsArray = result.getField(fieldName).Obj();
        for (const auto& shardObj : shardsArray) {
            shards.push_back(shardObj.String());
        }
        return shards;
    };

    const auto sourceField =
        isComputedPlacementAccurate ? "computedPlacement" : "placementAtInitTime";
    return HistoricalPlacement{extractShardIds(sourceField), HistoricalPlacementStatus::OK};
}

void ShardingCatalogManager::initializePlacementHistory(OperationContext* opCtx) {
    /**
     * This function will establish an initialization time to collect a consistent description of
     * the placement of each existing namespace through a snapshot read of the sharding catalog.
     * Such description will then be persisted in config.placementHistory.
     */

    // Delete any existing document that has been already majority committed.
    {
        // Set the needed read concern for the operation; since its execution through
        // _localConfigShard involves the DBDirectClient, RecoveryUnit::ReadSource also needs to
        // be restored upon exit.
        auto originalReadConcern =
            std::exchange(repl::ReadConcernArgs::get(opCtx),
                          repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern));

        auto originalReadSource =
            shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource();
        boost::optional<Timestamp> originalReadTimestamp;
        if (originalReadSource == RecoveryUnit::ReadSource::kProvided) {
            originalReadTimestamp =
                shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();
        }

        ScopeGuard resetopCtxStateGuard([&] {
            repl::ReadConcernArgs::get(opCtx) = std::move(originalReadConcern);
            shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
                originalReadSource, originalReadTimestamp);
        });

        write_ops::DeleteCommandRequest deleteOp(
            NamespaceString::kConfigsvrPlacementHistoryNamespace);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ({});
            entry.setMulti(true);
            return entry;
        }()});
        deleteOp.setWriteConcern(ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

        uassertStatusOK(_localConfigShard->runCommand(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            NamespaceString::kConfigsvrPlacementHistoryNamespace.dbName(),
            deleteOp.toBSON(),
            Shard::RetryPolicy::kNotIdempotent));

        const auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
        auto awaitReplicationResult = repl::ReplicationCoordinator::get(opCtx)->awaitReplication(
            opCtx, replClient.getLastOp(), defaultMajorityWriteConcernDoNotUse());
    }

    // Set the time of the initialization.
    Timestamp initializationTime;
    std::vector<ShardId> shardsAtInitializationTime;
    {
        Shard::QueryResponse allShardsQueryResponse;
        {
            // Ensure isolation from concurrent add/removeShards while the initializationTime is
            // set. Also, retrieve the content of config.shards (it will later form part of the
            // metadata describing the initialization of config.placementHistor).
            auto topologyScopedLock = enterStableTopologyRegion(opCtx);

            const auto now = VectorClock::get(opCtx)->getTime();
            initializationTime = now.configTime().asTimestamp();

            allShardsQueryResponse = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
                opCtx,
                ReadPreferenceSetting(ReadPreference::Nearest, TagSet{}),
                repl::ReadConcernLevel::kMajorityReadConcern,
                NamespaceString::kConfigsvrShardsNamespace,
                {},
                {},
                boost::none));
        }

        std::transform(allShardsQueryResponse.docs.begin(),
                       allShardsQueryResponse.docs.end(),
                       std::back_inserter(shardsAtInitializationTime),
                       [](const BSONObj& doc) {
                           return ShardId(std::string{doc.getStringField(ShardType::name.name())});
                       });
    }

    // Setup and run the aggregation that will perform the snapshot read of the sharding catalog and
    // persist its output into config.placementHistory.
    // (This operation includes a $merge stage writing into the config database, which requires
    // internal client credentials).
    {
        // TODO(SERVER-74658): Please revisit if this thread could be made killable.
        auto altClient = opCtx->getServiceContext()
                             ->getService(ClusterRole::ShardServer)
                             ->makeClient("initializePlacementHistory",
                                          Client::noSession(),
                                          ClientOperationKillableByStepdown{false});

        AuthorizationSession::get(altClient.get())->grantInternalAuthorization();
        AlternativeClientRegion acr(altClient);
        auto executor =
            Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
        CancelableOperationContext altOpCtx(
            cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

        auto aggRequest =
            createInitPlacementHistoryAggregationRequest(altOpCtx.get(), initializationTime);
        aggRequest.setUnwrappedReadPref({});
        repl::ReadConcernArgs readConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
        readConcernArgs.setArgsAtClusterTimeForSnapshot(initializationTime);
        aggRequest.setReadConcern(readConcernArgs);
        aggRequest.setWriteConcern({});
        auto noopCallback = [](const std::vector<BSONObj>& batch,
                               const boost::optional<BSONObj>& postBatchResumeToken) {
            return true;
        };

        // Failpoint to hang the operation after setting the snapshot read concern and before
        // running the aggregation.
        initializePlacementHistoryHangAfterSettingSnapshotReadConcern.pauseWhileSet();

        Status status = _localConfigShard->runAggregation(altOpCtx.get(), aggRequest, noopCallback);
        uassertStatusOK(status);
    }

    /*
     * config.placementHistory has now a full representation of the cluster at initializationTime.
     * As a final step, persist also the initialization metadata so that the whole content may be
     * consistently queried.
     */
    setInitializationTimeOnPlacementHistory(
        opCtx, initializationTime, std::move(shardsAtInitializationTime));
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
            getHistoricalPlacement(opCtx, boost::none /*namespace*/, earliestClusterTime);
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

    uassertStatusOK(_localConfigShard->runAggregation(opCtx, aggRequest, callback));

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
