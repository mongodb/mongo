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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/pipeline_d.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/multi_iterator.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/shard_filter.h"
#include "mongo/db/exec/trial_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_geo_near_cursor.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sample_from_random_cursor.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/write_ops/cluster_write.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using write_ops::Insert;

namespace {
/**
 * Returns a PlanExecutor which uses a random cursor to sample documents if successful. Returns {}
 * if the storage engine doesn't support random cursors, or if 'sampleSize' is a large enough
 * percentage of the collection.
 */
StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> createRandomCursorExecutor(
    Collection* coll, OperationContext* opCtx, long long sampleSize, long long numRecords) {
    // Verify that we are already under a collection lock. We avoid taking locks ourselves in this
    // function because double-locking forces any PlanExecutor we create to adopt a NO_YIELD policy.
    invariant(opCtx->lockState()->isCollectionLockedForMode(coll->ns().ns(), MODE_IS));

    static const double kMaxSampleRatioForRandCursor = 0.05;
    if (sampleSize > numRecords * kMaxSampleRatioForRandCursor || numRecords <= 100) {
        return {nullptr};
    }

    // Attempt to get a random cursor from the RecordStore.
    auto rsRandCursor = coll->getRecordStore()->getRandomCursor(opCtx);
    if (!rsRandCursor) {
        // The storage engine has no random cursor support.
        return {nullptr};
    }

    // Build a MultiIteratorStage and pass it the random-sampling RecordCursor.
    auto ws = std::make_unique<WorkingSet>();
    std::unique_ptr<PlanStage> root = std::make_unique<MultiIteratorStage>(opCtx, ws.get(), coll);
    static_cast<MultiIteratorStage*>(root.get())->addIterator(std::move(rsRandCursor));

    // Determine whether this collection is sharded. If so, retrieve its sharding metadata.
    boost::optional<ScopedCollectionMetadata> shardMetadata =
        (ShardingState::get(opCtx)->needCollectionMetadata(opCtx, coll->ns().ns())
             ? CollectionShardingState::get(opCtx, coll->ns())->getMetadataForOperation(opCtx)
             : boost::optional<ScopedCollectionMetadata>{});

    // Because 'numRecords' includes orphan documents, our initial decision to optimize the $sample
    // cursor may have been mistaken. For sharded collections, build a TRIAL plan that will switch
    // to a collection scan if the ratio of orphaned to owned documents encountered over the first
    // 100 works() is such that we would have chosen not to optimize.
    if (shardMetadata && (*shardMetadata)->isSharded()) {
        // The ratio of owned to orphaned documents must be at least equal to the ratio between the
        // requested sampleSize and the maximum permitted sampleSize for the original constraints to
        // be satisfied. For instance, if there are 200 documents and the sampleSize is 5, then at
        // least (5 / (200*0.05)) = (5/10) = 50% of those documents must be owned. If less than 5%
        // of the documents in the collection are owned, we default to the backup plan.
        static const size_t kMaxPresampleSize = 100;
        const auto minWorkAdvancedRatio = std::max(
            sampleSize / (numRecords * kMaxSampleRatioForRandCursor), kMaxSampleRatioForRandCursor);
        // The trial plan is SHARDING_FILTER-MULTI_ITERATOR.
        auto randomCursorPlan =
            std::make_unique<ShardFilterStage>(opCtx, *shardMetadata, ws.get(), root.release());
        // The backup plan is SHARDING_FILTER-COLLSCAN.
        std::unique_ptr<PlanStage> collScanPlan = std::make_unique<CollectionScan>(
            opCtx, coll, CollectionScanParams{}, ws.get(), nullptr);
        collScanPlan = std::make_unique<ShardFilterStage>(
            opCtx, *shardMetadata, ws.get(), collScanPlan.release());
        // Place a TRIAL stage at the root of the plan tree, and pass it the trial and backup plans.
        root = std::make_unique<TrialStage>(opCtx,
                                            ws.get(),
                                            std::move(randomCursorPlan),
                                            std::move(collScanPlan),
                                            kMaxPresampleSize,
                                            minWorkAdvancedRatio);
    }

    return PlanExecutor::make(
        opCtx, std::move(ws), std::move(root), coll, PlanExecutor::YIELD_AUTO);
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> attemptToGetExecutor(
    OperationContext* opCtx,
    Collection* collection,
    const NamespaceString& nss,
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    bool oplogReplay,
    BSONObj queryObj,
    BSONObj projectionObj,
    BSONObj sortObj,
    boost::optional<std::string> groupIdForDistinctScan,
    const AggregationRequest* aggRequest,
    const size_t plannerOpts,
    const MatchExpressionParser::AllowedFeatureSet& matcherFeatures) {
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setTailableMode(pExpCtx->tailableMode);
    qr->setOplogReplay(oplogReplay);
    qr->setFilter(queryObj);
    qr->setProj(projectionObj);
    qr->setSort(sortObj);
    if (aggRequest) {
        qr->setExplain(static_cast<bool>(aggRequest->getExplain()));
        qr->setHint(aggRequest->getHint());
    }

    // If the pipeline has a non-null collator, set the collation option to the result of
    // serializing the collator's spec back into BSON. We do this in order to fill in all options
    // that the user omitted.
    //
    // If pipeline has a null collator (representing the "simple" collation), we simply set the
    // collation option to the original user BSON, which is either the empty object (unspecified),
    // or the specification for the "simple" collation.
    qr->setCollation(pExpCtx->getCollator() ? pExpCtx->getCollator()->getSpec().toBSON()
                                            : pExpCtx->collation);

    const ExtensionsCallbackReal extensionsCallback(pExpCtx->opCtx, &nss);

    auto cq = CanonicalQuery::canonicalize(
        opCtx, std::move(qr), pExpCtx, extensionsCallback, matcherFeatures);

    if (!cq.isOK()) {
        // Return an error instead of uasserting, since there are cases where the combination of
        // sort and projection will result in a bad query, but when we try with a different
        // combination it will be ok. e.g. a sort by {$meta: 'textScore'}, without any projection
        // will fail, but will succeed when the corresponding '$meta' projection is passed in
        // another attempt.
        return {cq.getStatus()};
    }

    if (groupIdForDistinctScan) {
        // When the pipeline includes a $group that groups by a single field
        // (groupIdForDistinctScan), we use getExecutorDistinct() to attempt to get an executor that
        // uses a DISTINCT_SCAN to scan exactly one document for each group. When that's not
        // possible, we return nullptr, and the caller is responsible for trying again without
        // passing a 'groupIdForDistinctScan' value.
        ParsedDistinct parsedDistinct(std::move(cq.getValue()), *groupIdForDistinctScan);
        auto distinctExecutor =
            getExecutorDistinct(opCtx,
                                collection,
                                plannerOpts | QueryPlannerParams::STRICT_DISTINCT_ONLY,
                                &parsedDistinct);
        if (!distinctExecutor.isOK()) {
            return distinctExecutor.getStatus().withContext(
                "Unable to use distinct scan to optimize $group stage");
        } else if (!distinctExecutor.getValue()) {
            return {ErrorCodes::OperationFailed,
                    "Unable to use distinct scan to optimize $group stage"};
        } else {
            return distinctExecutor;
        }
    }

    return getExecutorFind(opCtx, collection, nss, std::move(cq.getValue()), plannerOpts);
}

BSONObj removeSortKeyMetaProjection(BSONObj projectionObj) {
    if (!projectionObj[Document::metaFieldSortKey]) {
        return projectionObj;
    }
    return projectionObj.removeField(Document::metaFieldSortKey);
}

/**
 * Examines the indexes in 'collection' and returns the field name of a geo-indexed field suitable
 * for use in $geoNear. 2d indexes are given priority over 2dsphere indexes.
 *
 * The 'collection' is required to exist. Throws if no usable 2d or 2dsphere index could be found.
 */
StringData extractGeoNearFieldFromIndexes(OperationContext* opCtx, Collection* collection) {
    invariant(collection);

    std::vector<const IndexDescriptor*> idxs;
    collection->getIndexCatalog()->findIndexByType(opCtx, IndexNames::GEO_2D, idxs);
    uassert(ErrorCodes::IndexNotFound,
            str::stream() << "There is more than one 2d index on " << collection->ns().ns()
                          << "; unsure which to use for $geoNear",
            idxs.size() <= 1U);
    if (idxs.size() == 1U) {
        for (auto&& elem : idxs.front()->keyPattern()) {
            if (elem.type() == BSONType::String && elem.valueStringData() == IndexNames::GEO_2D) {
                return elem.fieldNameStringData();
            }
        }
        MONGO_UNREACHABLE;
    }

    // If there are no 2d indexes, look for a 2dsphere index.
    idxs.clear();
    collection->getIndexCatalog()->findIndexByType(opCtx, IndexNames::GEO_2DSPHERE, idxs);
    uassert(ErrorCodes::IndexNotFound,
            "$geoNear requires a 2d or 2dsphere index, but none were found",
            !idxs.empty());
    uassert(ErrorCodes::IndexNotFound,
            str::stream() << "There is more than one 2dsphere index on " << collection->ns().ns()
                          << "; unsure which to use for $geoNear",
            idxs.size() <= 1U);

    invariant(idxs.size() == 1U);
    for (auto&& elem : idxs.front()->keyPattern()) {
        if (elem.type() == BSONType::String && elem.valueStringData() == IndexNames::GEO_2DSPHERE) {
            return elem.fieldNameStringData();
        }
    }
    MONGO_UNREACHABLE;
}
}  // namespace

void PipelineD::prepareCursorSource(Collection* collection,
                                    const NamespaceString& nss,
                                    const AggregationRequest* aggRequest,
                                    Pipeline* pipeline) {
    auto expCtx = pipeline->getContext();

    // We will be modifying the source vector as we go.
    Pipeline::SourceContainer& sources = pipeline->_sources;

    if (!sources.empty() && !sources.front()->constraints().requiresInputDocSource) {
        return;
    }

    // We are going to generate an input cursor, so we need to be holding the collection lock.
    dassert(expCtx->opCtx->lockState()->isCollectionLockedForMode(nss.ns(), MODE_IS));

    if (!sources.empty()) {
        auto sampleStage = dynamic_cast<DocumentSourceSample*>(sources.front().get());
        // Optimize an initial $sample stage if possible.
        if (collection && sampleStage) {
            const long long sampleSize = sampleStage->getSampleSize();
            const long long numRecords = collection->getRecordStore()->numRecords(expCtx->opCtx);
            auto exec = uassertStatusOK(
                createRandomCursorExecutor(collection, expCtx->opCtx, sampleSize, numRecords));
            if (exec) {
                // For sharded collections, the root of the plan tree is a TrialStage that may have
                // chosen either a random-sampling cursor trial plan or a COLLSCAN backup plan. We
                // can only optimize the $sample aggregation stage if the trial plan was chosen.
                auto* trialStage = (exec->getRootStage()->stageType() == StageType::STAGE_TRIAL
                                        ? static_cast<TrialStage*>(exec->getRootStage())
                                        : nullptr);
                if (!trialStage || !trialStage->pickedBackupPlan()) {
                    // Replace $sample stage with $sampleFromRandomCursor stage.
                    pipeline->popFront();
                    std::string idString = collection->ns().isOplog() ? "ts" : "_id";
                    pipeline->addInitialSource(DocumentSourceSampleFromRandomCursor::create(
                        expCtx, sampleSize, idString, numRecords));
                }

                // The order in which we evaluate these arguments is significant. We'd like to be
                // sure that the DocumentSourceCursor is created _last_, because if we run into a
                // case where a DocumentSourceCursor has been created (yet hasn't been put into a
                // Pipeline) and an exception is thrown, an invariant will trigger in the
                // DocumentSourceCursor. This is a design flaw in DocumentSourceCursor.

                // TODO SERVER-37453 this should no longer be necessary when we no don't need locks
                // to destroy a PlanExecutor.
                auto deps = pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
                addCursorSource(pipeline,
                                DocumentSourceCursor::create(collection, std::move(exec), expCtx),
                                std::move(deps));
                return;
            }
        }
    }

    // If the first stage is $geoNear, prepare a special DocumentSourceGeoNearCursor stage;
    // otherwise, create a generic DocumentSourceCursor.
    const auto geoNearStage =
        sources.empty() ? nullptr : dynamic_cast<DocumentSourceGeoNear*>(sources.front().get());
    if (geoNearStage) {
        prepareGeoNearCursorSource(collection, nss, aggRequest, pipeline);
    } else {
        prepareGenericCursorSource(collection, nss, aggRequest, pipeline);
    }
}

namespace {

/**
 * Look for $sort, $group at the beginning of the pipeline, potentially returning either or both.
 * Returns nullptr for any of the stages that are not found. Note that we are not looking for the
 * opposite pattern ($group, $sort). In that case, this function will return only the $group stage.
 *
 * This function will not return the $group in the case that there is an initial $sort with
 * intermediate stages that separate it from the $group (e.g.: $sort, $limit, $group). That includes
 * the case of a $sort with a non-null value for getLimitSrc(), indicating that there was previously
 * a $limit stage that was optimized away.
 */
std::pair<boost::intrusive_ptr<DocumentSourceSort>, boost::intrusive_ptr<DocumentSourceGroup>>
getSortAndGroupStagesFromPipeline(const Pipeline::SourceContainer& sources) {
    boost::intrusive_ptr<DocumentSourceSort> sortStage = nullptr;
    boost::intrusive_ptr<DocumentSourceGroup> groupStage = nullptr;

    auto sourcesIt = sources.begin();
    if (sourcesIt != sources.end()) {
        sortStage = dynamic_cast<DocumentSourceSort*>(sourcesIt->get());
        if (sortStage) {
            if (!sortStage->getLimitSrc()) {
                ++sourcesIt;
            } else {
                // This $sort stage was previously followed by a $limit stage.
                sourcesIt = sources.end();
            }
        }
    }

    if (sourcesIt != sources.end()) {
        groupStage = dynamic_cast<DocumentSourceGroup*>(sourcesIt->get());
    }

    return std::make_pair(sortStage, groupStage);
}

}  // namespace

void PipelineD::prepareGenericCursorSource(Collection* collection,
                                           const NamespaceString& nss,
                                           const AggregationRequest* aggRequest,
                                           Pipeline* pipeline) {
    Pipeline::SourceContainer& sources = pipeline->_sources;
    auto expCtx = pipeline->getContext();

    // Look for an initial match. This works whether we got an initial query or not. If not, it
    // results in a "{}" query, which will be what we want in that case.
    bool oplogReplay = false;
    const BSONObj queryObj = pipeline->getInitialQuery();
    if (!queryObj.isEmpty()) {
        auto matchStage = dynamic_cast<DocumentSourceMatch*>(sources.front().get());
        if (matchStage) {
            oplogReplay = dynamic_cast<DocumentSourceOplogMatch*>(matchStage) != nullptr;
            // If a $match query is pulled into the cursor, the $match is redundant, and can be
            // removed from the pipeline.
            sources.pop_front();
        } else {
            // A $geoNear stage, the only other stage that can produce an initial query, is also
            // a valid initial stage. However, we should be in prepareGeoNearCursorSource() instead.
            MONGO_UNREACHABLE;
        }
    }

    // Find the set of fields in the source documents depended on by this pipeline.
    DepsTracker deps = pipeline->getDependencies(DocumentSourceMatch::isTextQuery(queryObj)
                                                     ? DepsTracker::MetadataAvailable::kTextScore
                                                     : DepsTracker::MetadataAvailable::kNoMetadata);

    BSONObj projForQuery = deps.toProjection();

    boost::intrusive_ptr<DocumentSourceSort> sortStage;
    boost::intrusive_ptr<DocumentSourceGroup> groupStage;
    std::tie(sortStage, groupStage) = getSortAndGroupStagesFromPipeline(pipeline->_sources);

    BSONObj sortObj;
    if (sortStage) {
        sortObj = sortStage
                      ->sortKeyPattern(
                          DocumentSourceSort::SortKeySerialization::kForPipelineSerialization)
                      .toBson();
    }

    std::unique_ptr<GroupFromFirstDocumentTransformation> rewrittenGroupStage;
    if (groupStage) {
        rewrittenGroupStage = groupStage->rewriteGroupAsTransformOnFirstDocument();
    }

    // Create the PlanExecutor.
    auto exec = uassertStatusOK(prepareExecutor(expCtx->opCtx,
                                                collection,
                                                nss,
                                                pipeline,
                                                expCtx,
                                                oplogReplay,
                                                sortStage,
                                                std::move(rewrittenGroupStage),
                                                deps,
                                                queryObj,
                                                aggRequest,
                                                Pipeline::kAllowedMatcherFeatures,
                                                &sortObj,
                                                &projForQuery));


    if (!projForQuery.isEmpty() && !sources.empty()) {
        // Check for redundant $project in query with the same specification as the inclusion
        // projection generated by the dependency optimization.
        auto proj =
            dynamic_cast<DocumentSourceSingleDocumentTransformation*>(sources.front().get());
        if (proj && proj->isSubsetOfProjection(projForQuery)) {
            sources.pop_front();
        }
    }

    // If this is a change stream pipeline, make sure that we tell DSCursor to track the oplog time.
    const bool trackOplogTS =
        (pipeline->peekFront() && pipeline->peekFront()->constraints().isChangeStreamStage());

    addCursorSource(pipeline,
                    DocumentSourceCursor::create(collection, std::move(exec), expCtx, trackOplogTS),
                    deps,
                    queryObj,
                    sortObj,
                    projForQuery);
}

void PipelineD::prepareGeoNearCursorSource(Collection* collection,
                                           const NamespaceString& nss,
                                           const AggregationRequest* aggRequest,
                                           Pipeline* pipeline) {
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "$geoNear requires a geo index to run, but " << nss.ns()
                          << " does not exist",
            collection);

    Pipeline::SourceContainer& sources = pipeline->_sources;
    auto expCtx = pipeline->getContext();
    const auto geoNearStage = dynamic_cast<DocumentSourceGeoNear*>(sources.front().get());
    invariant(geoNearStage);

    auto deps = pipeline->getDependencies(DepsTracker::kAllGeoNearDataAvailable);

    // If the user specified a "key" field, use that field to satisfy the "near" query. Otherwise,
    // look for a geo-indexed field in 'collection' that can.
    auto nearFieldName =
        (geoNearStage->getKeyField() ? geoNearStage->getKeyField()->fullPath()
                                     : extractGeoNearFieldFromIndexes(expCtx->opCtx, collection))
            .toString();

    // Create a PlanExecutor whose query is the "near" predicate on 'nearFieldName' combined with
    // the optional "query" argument in the $geoNear stage.
    BSONObj fullQuery = geoNearStage->asNearQuery(nearFieldName);
    BSONObj proj = deps.toProjection();
    BSONObj sortFromQuerySystem;
    auto exec = uassertStatusOK(prepareExecutor(expCtx->opCtx,
                                                collection,
                                                nss,
                                                pipeline,
                                                expCtx,
                                                false,   /* oplogReplay */
                                                nullptr, /* sortStage */
                                                nullptr, /* rewrittenGroupStage */
                                                deps,
                                                std::move(fullQuery),
                                                aggRequest,
                                                Pipeline::kGeoNearMatcherFeatures,
                                                &sortFromQuerySystem,
                                                &proj));

    invariant(sortFromQuerySystem.isEmpty(),
              str::stream() << "Unexpectedly got the following sort from the query system: "
                            << sortFromQuerySystem.jsonString());

    auto geoNearCursor =
        DocumentSourceGeoNearCursor::create(collection,
                                            std::move(exec),
                                            expCtx,
                                            geoNearStage->getDistanceField(),
                                            geoNearStage->getLocationField(),
                                            geoNearStage->getDistanceMultiplier().value_or(1.0));

    // Remove the initial $geoNear; it will be replaced by $geoNearCursor.
    sources.pop_front();
    addCursorSource(pipeline, std::move(geoNearCursor), std::move(deps));
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PipelineD::prepareExecutor(
    OperationContext* opCtx,
    Collection* collection,
    const NamespaceString& nss,
    Pipeline* pipeline,
    const intrusive_ptr<ExpressionContext>& expCtx,
    bool oplogReplay,
    const boost::intrusive_ptr<DocumentSourceSort>& sortStage,
    std::unique_ptr<GroupFromFirstDocumentTransformation> rewrittenGroupStage,
    const DepsTracker& deps,
    const BSONObj& queryObj,
    const AggregationRequest* aggRequest,
    const MatchExpressionParser::AllowedFeatureSet& matcherFeatures,
    BSONObj* sortObj,
    BSONObj* projectionObj) {
    // The query system has the potential to use an index to provide a non-blocking sort and/or to
    // use the projection to generate a covered plan. If this is possible, it is more efficient to
    // let the query system handle those parts of the pipeline. If not, it is more efficient to use
    // a $sort and/or a ParsedDeps object. Thus, we will determine whether the query system can
    // provide a non-blocking sort or a covered projection before we commit to a PlanExecutor.
    //
    // To determine if the query system can provide a non-blocking sort, we pass the
    // NO_BLOCKING_SORT planning option, meaning 'getExecutor' will not produce a PlanExecutor if
    // the query system would use a blocking sort stage.
    //
    // To determine if the query system can provide a covered projection, we pass the
    // NO_UNCOVERED_PROJECTS planning option, meaning 'getExecutor' will not produce a PlanExecutor
    // if the query system would need to fetch the document to do the projection. The following
    // logic uses the above strategies, with multiple calls to 'attemptToGetExecutor' to determine
    // the most efficient way to handle the $sort and $project stages.
    //
    // LATER - We should attempt to determine if the results from the query are returned in some
    // order so we can then apply other optimizations there are tickets for, such as SERVER-4507.
    size_t plannerOpts = QueryPlannerParams::DEFAULT | QueryPlannerParams::NO_BLOCKING_SORT;

    if (deps.hasNoRequirements()) {
        // If we don't need any fields from the input document, performing a count is faster, and
        // will output empty documents, which is okay.
        plannerOpts |= QueryPlannerParams::IS_COUNT;
    }

    if (pipeline->peekFront() && pipeline->peekFront()->constraints().isChangeStreamStage()) {
        invariant(expCtx->tailableMode == TailableModeEnum::kTailableAndAwaitData);
        plannerOpts |= QueryPlannerParams::TRACK_LATEST_OPLOG_TS;
    }

    if (rewrittenGroupStage) {
        BSONObj emptySort;

        // See if the query system can handle the $group and $sort stage using a DISTINCT_SCAN
        // (SERVER-9507). Note that passing the empty projection (as we do for some of the
        // attemptToGetExecutor() calls below) causes getExecutorDistinct() to ignore some otherwise
        // valid DISTINCT_SCAN plans, so we pass the projection and exclude the
        // NO_UNCOVERED_PROJECTIONS planner parameter.
        auto swExecutorGrouped = attemptToGetExecutor(opCtx,
                                                      collection,
                                                      nss,
                                                      expCtx,
                                                      oplogReplay,
                                                      queryObj,
                                                      *projectionObj,
                                                      sortObj ? *sortObj : emptySort,
                                                      rewrittenGroupStage->groupId(),
                                                      aggRequest,
                                                      plannerOpts,
                                                      matcherFeatures);

        if (swExecutorGrouped.isOK()) {
            // Any $limit stage before the $group stage should make the pipeline ineligible for this
            // optimization.
            invariant(!sortStage || !sortStage->getLimitSrc());

            // We remove the $sort and $group stages that begin the pipeline, because the executor
            // will handle the sort, and the groupTransform (added below) will handle the $group
            // stage.
            pipeline->popFrontWithName(DocumentSourceSort::kStageName);
            pipeline->popFrontWithName(DocumentSourceGroup::kStageName);

            boost::intrusive_ptr<DocumentSource> groupTransform(
                new DocumentSourceSingleDocumentTransformation(
                    expCtx,
                    std::move(rewrittenGroupStage),
                    "$groupByDistinctScan",
                    false /* independentOfAnyCollection */));
            pipeline->addInitialSource(groupTransform);

            return swExecutorGrouped;
        } else if (swExecutorGrouped == ErrorCodes::QueryPlanKilled) {
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to determine whether query system can provide a "
                                     "DISTINCT_SCAN grouping: "
                                  << swExecutorGrouped.getStatus().toString()};
        }
    }

    const BSONObj emptyProjection;
    const BSONObj metaSortProjection = BSON("$meta"
                                            << "sortKey");

    // The only way to get meta information (e.g. the text score) is to let the query system handle
    // the projection. In all other cases, unless the query system can do an index-covered
    // projection and avoid going to the raw record at all, it is faster to have ParsedDeps filter
    // the fields we need.
    if (!deps.getNeedsAnyMetadata()) {
        plannerOpts |= QueryPlannerParams::NO_UNCOVERED_PROJECTIONS;
    }

    if (sortStage) {
        // See if the query system can provide a non-blocking sort.
        auto swExecutorSort =
            attemptToGetExecutor(opCtx,
                                 collection,
                                 nss,
                                 expCtx,
                                 oplogReplay,
                                 queryObj,
                                 expCtx->needsMerge ? metaSortProjection : emptyProjection,
                                 *sortObj,
                                 boost::none, /* groupIdForDistinctScan */
                                 aggRequest,
                                 plannerOpts,
                                 matcherFeatures);

        if (swExecutorSort.isOK()) {
            // Success! Now see if the query system can also cover the projection.
            auto swExecutorSortAndProj =
                attemptToGetExecutor(opCtx,
                                     collection,
                                     nss,
                                     expCtx,
                                     oplogReplay,
                                     queryObj,
                                     *projectionObj,
                                     *sortObj,
                                     boost::none, /* groupIdForDistinctScan */
                                     aggRequest,
                                     plannerOpts,
                                     matcherFeatures);

            std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
            if (swExecutorSortAndProj.isOK()) {
                // Success! We have a non-blocking sort and a covered projection.
                exec = std::move(swExecutorSortAndProj.getValue());
            } else if (swExecutorSortAndProj == ErrorCodes::QueryPlanKilled) {
                return {ErrorCodes::OperationFailed,
                        str::stream() << "Failed to determine whether query system can provide a "
                                         "covered projection in addition to a non-blocking sort: "
                                      << swExecutorSortAndProj.getStatus().toString()};
            } else {
                // The query system couldn't cover the projection.
                *projectionObj = BSONObj();
                exec = std::move(swExecutorSort.getValue());
            }

            // We know the sort is being handled by the query system, so remove the $sort stage.
            pipeline->_sources.pop_front();

            if (sortStage->getLimitSrc()) {
                // We need to reinsert the coalesced $limit after removing the $sort.
                pipeline->_sources.push_front(sortStage->getLimitSrc());
            }
            return std::move(exec);
        } else if (swExecutorSort == ErrorCodes::QueryPlanKilled) {
            return {
                ErrorCodes::OperationFailed,
                str::stream()
                    << "Failed to determine whether query system can provide a non-blocking sort: "
                    << swExecutorSort.getStatus().toString()};
        }
        // The query system can't provide a non-blocking sort.
        *sortObj = BSONObj();
    }

    // Either there was no $sort stage, or the query system could not provide a non-blocking
    // sort.
    dassert(sortObj->isEmpty());
    *projectionObj = removeSortKeyMetaProjection(*projectionObj);
    const auto metadataRequired = deps.getAllRequiredMetadataTypes();
    if (metadataRequired.size() == 1 &&
        metadataRequired.front() == DepsTracker::MetadataType::SORT_KEY) {
        // A sort key requirement would have prevented us from being able to add this parameter
        // before, but now we know the query system won't cover the sort, so we will be able to
        // compute the sort key ourselves during the $sort stage, and thus don't need a query
        // projection to do so.
        plannerOpts |= QueryPlannerParams::NO_UNCOVERED_PROJECTIONS;
    }

    // See if the query system can cover the projection.
    auto swExecutorProj = attemptToGetExecutor(opCtx,
                                               collection,
                                               nss,
                                               expCtx,
                                               oplogReplay,
                                               queryObj,
                                               *projectionObj,
                                               *sortObj,
                                               boost::none, /* groupIdForDistinctScan */
                                               aggRequest,
                                               plannerOpts,
                                               matcherFeatures);
    if (swExecutorProj.isOK()) {
        // Success! We have a covered projection.
        return std::move(swExecutorProj.getValue());
    } else if (swExecutorProj == ErrorCodes::QueryPlanKilled) {
        return {ErrorCodes::OperationFailed,
                str::stream()
                    << "Failed to determine whether query system can provide a covered projection: "
                    << swExecutorProj.getStatus().toString()};
    }

    // The query system couldn't provide a covered projection.
    *projectionObj = BSONObj();
    // If this doesn't work, nothing will.
    return attemptToGetExecutor(opCtx,
                                collection,
                                nss,
                                expCtx,
                                oplogReplay,
                                queryObj,
                                *projectionObj,
                                *sortObj,
                                boost::none, /* groupIdForDistinctScan */
                                aggRequest,
                                plannerOpts,
                                matcherFeatures);
}

void PipelineD::addCursorSource(Pipeline* pipeline,
                                boost::intrusive_ptr<DocumentSourceCursor> cursor,
                                DepsTracker deps,
                                const BSONObj& queryObj,
                                const BSONObj& sortObj,
                                const BSONObj& projectionObj) {
    // Add the cursor to the pipeline first so that it's correctly disposed of as part of the
    // pipeline if an exception is thrown during this method.
    pipeline->addInitialSource(cursor);

    cursor->setQuery(queryObj);
    cursor->setSort(sortObj);
    if (deps.hasNoRequirements()) {
        cursor->shouldProduceEmptyDocs();
    }

    if (!projectionObj.isEmpty()) {
        cursor->setProjection(projectionObj, boost::none);
    } else {
        // There may be fewer dependencies now if the sort was covered.
        if (!sortObj.isEmpty()) {
            deps = pipeline->getDependencies(DocumentSourceMatch::isTextQuery(queryObj)
                                                 ? DepsTracker::MetadataAvailable::kTextScore
                                                 : DepsTracker::MetadataAvailable::kNoMetadata);
        }

        cursor->setProjection(deps.toProjection(), deps.toParsedDeps());
    }
}

Timestamp PipelineD::getLatestOplogTimestamp(const Pipeline* pipeline) {
    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(pipeline->_sources.front().get())) {
        return docSourceCursor->getLatestOplogTimestamp();
    }
    return Timestamp();
}

std::string PipelineD::getPlanSummaryStr(const Pipeline* pipeline) {
    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(pipeline->_sources.front().get())) {
        return docSourceCursor->getPlanSummaryStr();
    }

    return "";
}

void PipelineD::getPlanSummaryStats(const Pipeline* pipeline, PlanSummaryStats* statsOut) {
    invariant(statsOut);

    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(pipeline->_sources.front().get())) {
        *statsOut = docSourceCursor->getPlanSummaryStats();
    }

    bool hasSortStage{false};
    bool usedDisk{false};
    for (auto&& source : pipeline->_sources) {
        if (dynamic_cast<DocumentSourceSort*>(source.get()))
            hasSortStage = true;

        usedDisk = usedDisk || source->usedDisk();
        if (usedDisk && hasSortStage)
            break;
    }
    statsOut->hasSortStage = hasSortStage;
    statsOut->usedDisk = usedDisk;
}

}  // namespace mongo
