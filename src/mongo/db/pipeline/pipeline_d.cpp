/**
 * Copyright (c) 2012-2014 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/pipeline_d.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_iterator.h"
#include "mongo/db/exec/multi_iterator.h"
#include "mongo/db/exec/shard_filter.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/kill_sessions.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_geo_near_cursor.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge_cursors.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sample_from_random_cursor.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;

namespace {

/**
 * Returns a PlanExecutor which uses a random cursor to sample documents if successful. Returns {}
 * if the storage engine doesn't support random cursors, or if 'sampleSize' is a large enough
 * percentage of the collection.
 */
StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> createRandomCursorExecutor(
    Collection* collection, OperationContext* opCtx, long long sampleSize, long long numRecords) {
    double kMaxSampleRatioForRandCursor = 0.05;
    if (sampleSize > numRecords * kMaxSampleRatioForRandCursor || numRecords <= 100) {
        return {nullptr};
    }

    // Attempt to get a random cursor from the RecordStore. If the RecordStore does not support
    // random cursors, attempt to get one from the _id index.
    std::unique_ptr<RecordCursor> rsRandCursor =
        collection->getRecordStore()->getRandomCursor(opCtx);

    auto ws = stdx::make_unique<WorkingSet>();
    std::unique_ptr<PlanStage> stage;

    if (rsRandCursor) {
        stage = stdx::make_unique<MultiIteratorStage>(opCtx, ws.get(), collection);
        static_cast<MultiIteratorStage*>(stage.get())->addIterator(std::move(rsRandCursor));

    } else {
        auto indexCatalog = collection->getIndexCatalog();
        auto indexDescriptor = indexCatalog->findIdIndex(opCtx);

        if (!indexDescriptor) {
            // There was no _id index.
            return {nullptr};
        }

        IndexAccessMethod* idIam = indexCatalog->getIndex(indexDescriptor);
        auto idxRandCursor = idIam->newRandomCursor(opCtx);

        if (!idxRandCursor) {
            // Storage engine does not support any type of random cursor.
            return {nullptr};
        }

        auto idxIterator = stdx::make_unique<IndexIteratorStage>(opCtx,
                                                                 ws.get(),
                                                                 collection,
                                                                 idIam,
                                                                 indexDescriptor->keyPattern(),
                                                                 std::move(idxRandCursor));
        stage = stdx::make_unique<FetchStage>(
            opCtx, ws.get(), idxIterator.release(), nullptr, collection);
    }

    {
        AutoGetCollectionForRead autoColl(opCtx, collection->ns());

        // If we're in a sharded environment, we need to filter out documents we don't own.
        if (ShardingState::get(opCtx)->needCollectionMetadata(opCtx, collection->ns().ns())) {
            auto shardFilterStage = stdx::make_unique<ShardFilterStage>(
                opCtx,
                CollectionShardingState::get(opCtx, collection->ns())->getMetadata(opCtx),
                ws.get(),
                stage.release());
            return PlanExecutor::make(opCtx,
                                      std::move(ws),
                                      std::move(shardFilterStage),
                                      collection,
                                      PlanExecutor::YIELD_AUTO);
        }
    }

    return PlanExecutor::make(
        opCtx, std::move(ws), std::move(stage), collection, PlanExecutor::YIELD_AUTO);
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

    std::vector<IndexDescriptor*> idxs;
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
                // Replace $sample stage with $sampleFromRandomCursor stage.
                sources.pop_front();
                std::string idString = collection->ns().isOplog() ? "ts" : "_id";
                sources.emplace_front(DocumentSourceSampleFromRandomCursor::create(
                    expCtx, sampleSize, idString, numRecords));

                addCursorSource(
                    pipeline,
                    DocumentSourceCursor::create(collection, std::move(exec), expCtx),
                    pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata));
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

    // Look for an initial sort; we'll try to add this to the Cursor we create. If we're successful
    // in doing that, we'll remove the $sort from the pipeline, because the documents will already
    // come sorted in the specified order as a result of the index scan.
    intrusive_ptr<DocumentSourceSort> sortStage;
    BSONObj sortObj;
    if (!sources.empty()) {
        sortStage = dynamic_cast<DocumentSourceSort*>(sources.front().get());
        if (sortStage) {
            sortObj = sortStage
                          ->sortKeyPattern(
                              DocumentSourceSort::SortKeySerialization::kForPipelineSerialization)
                          .toBson();
        }
    }

    // Create the PlanExecutor.
    auto exec = uassertStatusOK(prepareExecutor(expCtx->opCtx,
                                                collection,
                                                nss,
                                                pipeline,
                                                expCtx,
                                                oplogReplay,
                                                sortStage,
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

    addCursorSource(pipeline,
                    DocumentSourceCursor::create(collection, std::move(exec), expCtx),
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
    const intrusive_ptr<DocumentSourceSort>& sortStage,
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

    // The only way to get meta information (e.g. the text score) is to let the query system handle
    // the projection. In all other cases, unless the query system can do an index-covered
    // projection and avoid going to the raw record at all, it is faster to have ParsedDeps filter
    // the fields we need.
    if (!deps.getNeedsAnyMetadata()) {
        plannerOpts |= QueryPlannerParams::NO_UNCOVERED_PROJECTIONS;
    }

    if (expCtx->needsMerge && expCtx->tailableMode == TailableModeEnum::kTailableAndAwaitData) {
        plannerOpts |= QueryPlannerParams::TRACK_LATEST_OPLOG_TS;
    }

    const BSONObj emptyProjection;
    const BSONObj metaSortProjection = BSON("$meta"
                                            << "sortKey");
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
                                 aggRequest,
                                 plannerOpts,
                                 matcherFeatures);

        if (swExecutorSort.isOK()) {
            // Success! Now see if the query system can also cover the projection.
            auto swExecutorSortAndProj = attemptToGetExecutor(opCtx,
                                                              collection,
                                                              nss,
                                                              expCtx,
                                                              oplogReplay,
                                                              queryObj,
                                                              *projectionObj,
                                                              *sortObj,
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
    pipeline->addInitialSource(std::move(cursor));
}

Timestamp PipelineD::getLatestOplogTimestamp(const Pipeline* pipeline) {
    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(pipeline->_sources.front().get())) {
        return docSourceCursor->getLatestOplogTimestamp();
    }
    return Timestamp();
}

std::string PipelineD::getPlanSummaryStr(const Pipeline* pPipeline) {
    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(pPipeline->_sources.front().get())) {
        return docSourceCursor->getPlanSummaryStr();
    }

    return "";
}

void PipelineD::getPlanSummaryStats(const Pipeline* pPipeline, PlanSummaryStats* statsOut) {
    invariant(statsOut);

    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(pPipeline->_sources.front().get())) {
        *statsOut = docSourceCursor->getPlanSummaryStats();
    }

    bool hasSortStage{false};
    for (auto&& source : pPipeline->_sources) {
        if (dynamic_cast<DocumentSourceSort*>(source.get())) {
            hasSortStage = true;
            break;
        }
    }

    statsOut->hasSortStage = hasSortStage;
}

PipelineD::MongoDInterface::MongoDInterface(OperationContext* opCtx) : _client(opCtx) {}

void PipelineD::MongoDInterface::setOperationContext(OperationContext* opCtx) {
    _client.setOpCtx(opCtx);
}

DBClientBase* PipelineD::MongoDInterface::directClient() {
    return &_client;
}

bool PipelineD::MongoDInterface::isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    // TODO SERVER-24960: Use CollectionShardingState::collectionIsSharded() to confirm sharding
    // state.
    auto css = CollectionShardingState::get(opCtx, nss);
    return bool(css->getMetadata(opCtx));
}

BSONObj PipelineD::MongoDInterface::insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const NamespaceString& ns,
                                           const std::vector<BSONObj>& objs) {
    boost::optional<DisableDocumentValidation> maybeDisableValidation;
    if (expCtx->bypassDocumentValidation)
        maybeDisableValidation.emplace(expCtx->opCtx);

    _client.insert(ns.ns(), objs);
    return _client.getLastErrorDetailed();
}

CollectionIndexUsageMap PipelineD::MongoDInterface::getIndexStats(OperationContext* opCtx,
                                                                  const NamespaceString& ns) {
    AutoGetCollectionForReadCommand autoColl(opCtx, ns);

    Collection* collection = autoColl.getCollection();
    if (!collection) {
        LOG(2) << "Collection not found on index stats retrieval: " << ns.ns();
        return CollectionIndexUsageMap();
    }

    return collection->infoCache()->getIndexUsageStats();
}

void PipelineD::MongoDInterface::appendLatencyStats(OperationContext* opCtx,
                                                    const NamespaceString& nss,
                                                    bool includeHistograms,
                                                    BSONObjBuilder* builder) const {
    Top::get(opCtx->getServiceContext()).appendLatencyStats(nss.ns(), includeHistograms, builder);
}

Status PipelineD::MongoDInterface::appendStorageStats(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const BSONObj& param,
                                                      BSONObjBuilder* builder) const {
    return appendCollectionStorageStats(opCtx, nss, param, builder);
}

Status PipelineD::MongoDInterface::appendRecordCount(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     BSONObjBuilder* builder) const {
    return appendCollectionRecordCount(opCtx, nss, builder);
}

BSONObj PipelineD::MongoDInterface::getCollectionOptions(const NamespaceString& nss) {
    const auto infos = _client.getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));
    return infos.empty() ? BSONObj() : infos.front().getObjectField("options").getOwned();
}

Status PipelineD::MongoDInterface::renameIfOptionsAndIndexesHaveNotChanged(
    OperationContext* opCtx,
    const BSONObj& renameCommandObj,
    const NamespaceString& targetNs,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {
    Lock::GlobalWrite globalLock(opCtx);

    if (SimpleBSONObjComparator::kInstance.evaluate(originalCollectionOptions !=
                                                    getCollectionOptions(targetNs))) {
        return {ErrorCodes::CommandFailed,
                str::stream() << "collection options of target collection " << targetNs.ns()
                              << " changed during processing. Original options: "
                              << originalCollectionOptions
                              << ", new options: "
                              << getCollectionOptions(targetNs)};
    }

    auto currentIndexes = _client.getIndexSpecs(targetNs.ns());
    if (originalIndexes.size() != currentIndexes.size() ||
        !std::equal(originalIndexes.begin(),
                    originalIndexes.end(),
                    currentIndexes.begin(),
                    SimpleBSONObjComparator::kInstance.makeEqualTo())) {
        return {ErrorCodes::CommandFailed,
                str::stream() << "indexes of target collection " << targetNs.ns()
                              << " changed during processing."};
    }

    BSONObj info;
    bool ok = _client.runCommand("admin", renameCommandObj, info);
    return ok ? Status::OK() : Status{ErrorCodes::CommandFailed,
                                      str::stream() << "renameCollection failed: " << info};
}

StatusWith<std::unique_ptr<Pipeline, PipelineDeleter>> PipelineD::MongoDInterface::makePipeline(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MakePipelineOptions opts) {
    auto pipeline = Pipeline::parse(rawPipeline, expCtx);
    if (!pipeline.isOK()) {
        return pipeline.getStatus();
    }

    if (opts.optimize) {
        pipeline.getValue()->optimizePipeline();
    }

    Status cursorStatus = Status::OK();

    if (opts.attachCursorSource) {
        cursorStatus = attachCursorSourceToPipeline(expCtx, pipeline.getValue().get());
    }

    return cursorStatus.isOK() ? std::move(pipeline) : cursorStatus;
}

Status PipelineD::MongoDInterface::attachCursorSourceToPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* pipeline) {
    invariant(pipeline->getSources().empty() ||
              !dynamic_cast<DocumentSourceCursor*>(pipeline->getSources().front().get()));

    boost::optional<AutoGetCollectionForReadCommand> autoColl;
    if (expCtx->uuid) {
        try {
            autoColl.emplace(expCtx->opCtx,
                             NamespaceStringOrUUID{expCtx->ns.db().toString(), *expCtx->uuid});
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
            // The UUID doesn't exist anymore
            return ex.toStatus();
        }
    } else {
        autoColl.emplace(expCtx->opCtx, expCtx->ns);
    }

    // makePipeline() is only called to perform secondary aggregation requests and expects the
    // collection representing the document source to be not-sharded. We confirm sharding state
    // here to avoid taking a collection lock elsewhere for this purpose alone.
    // TODO SERVER-27616: This check is incorrect in that we don't acquire a collection cursor
    // until after we release the lock, leaving room for a collection to be sharded inbetween.
    // TODO SERVER-24960: Use CollectionShardingState::collectionIsSharded() to confirm sharding
    // state.
    auto css = CollectionShardingState::get(expCtx->opCtx, expCtx->ns);
    uassert(4567,
            str::stream() << "from collection (" << expCtx->ns.ns() << ") cannot be sharded",
            !bool(css->getMetadata(expCtx->opCtx)));

    PipelineD::prepareCursorSource(autoColl->getCollection(), expCtx->ns, nullptr, pipeline);

    return Status::OK();
}

std::string PipelineD::MongoDInterface::getShardName(OperationContext* opCtx) const {
    if (ShardingState::get(opCtx)->enabled()) {
        return ShardingState::get(opCtx)->getShardName();
    }

    return std::string();
}

std::pair<std::vector<FieldPath>, bool> PipelineD::MongoDInterface::collectDocumentKeyFields(
    OperationContext* opCtx, UUID uuid) const {
    if (serverGlobalParams.clusterRole != ClusterRole::ShardServer) {
        return {{"_id"}, false};  // Nothing is sharded.
    }

    // An empty namespace indicates that the collection has been dropped. Treat it as unsharded and
    // mark the fields as final.
    auto nss = UUIDCatalog::get(opCtx).lookupNSSByUUID(uuid);
    if (nss.isEmpty()) {
        return {{"_id"}, true};
    }

    // Before taking a collection lock to retrieve the shard key fields, consult the catalog cache
    // to determine whether the collection is sharded in the first place.
    auto catalogCache = Grid::get(opCtx)->catalogCache();

    const bool collectionIsSharded = catalogCache && [&]() {
        auto routingInfo = catalogCache->getCollectionRoutingInfo(opCtx, nss);
        return routingInfo.isOK() && routingInfo.getValue().cm();
    }();

    // Collection exists and is not sharded, mark as not final.
    if (!collectionIsSharded) {
        return {{"_id"}, false};
    }

    auto scm = [opCtx, &nss]() -> ScopedCollectionMetadata {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        return CollectionShardingState::get(opCtx, nss)->getMetadata(opCtx);
    }();

    // Collection is not sharded or UUID mismatch implies collection has been dropped and recreated
    // as sharded.
    if (!scm || !scm->uuidMatches(uuid)) {
        return {{"_id"}, false};
    }

    // Unpack the shard key.
    std::vector<FieldPath> result;
    bool gotId = false;
    for (auto& field : scm->getKeyPatternFields()) {
        result.emplace_back(field->dottedField());
        gotId |= (result.back().fullPath() == "_id");
    }
    if (!gotId) {  // If not part of the shard key, "_id" comes last.
        result.emplace_back("_id");
    }
    // Collection is now sharded so the document key fields will never change, mark as final.
    return {result, true};
}

std::vector<GenericCursor> PipelineD::MongoDInterface::getCursors(
    const intrusive_ptr<ExpressionContext>& expCtx) const {
    return CursorManager::getAllCursors(expCtx->opCtx);
}

boost::optional<Document> PipelineD::MongoDInterface::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    UUID collectionUUID,
    const Document& documentKey,
    boost::optional<BSONObj> readConcern) {
    invariant(!readConcern);  // We don't currently support a read concern on mongod - it's only
                              // expected to be necessary on mongos.

    std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
    try {
        // Be sure to do the lookup using the collection default collation
        auto foreignExpCtx = expCtx->copyWith(
            nss,
            collectionUUID,
            _getCollectionDefaultCollator(expCtx->opCtx, nss.db(), collectionUUID));
        pipeline = uassertStatusOK(makePipeline({BSON("$match" << documentKey)}, foreignExpCtx));
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return boost::none;
    }

    auto lookedUpDocument = pipeline->getNext();
    if (auto next = pipeline->getNext()) {
        uasserted(ErrorCodes::TooManyMatchingDocuments,
                  str::stream() << "found more than one document with document key "
                                << documentKey.toString()
                                << " ["
                                << lookedUpDocument->toString()
                                << ", "
                                << next->toString()
                                << "]");
    }
    return lookedUpDocument;
}

BSONObj PipelineD::MongoDInterface::_reportCurrentOpForClient(
    OperationContext* opCtx, Client* client, CurrentOpTruncateMode truncateOps) const {
    BSONObjBuilder builder;

    CurOp::reportCurrentOpForClient(
        opCtx, client, (truncateOps == CurrentOpTruncateMode::kTruncateOps), &builder);

    // Append lock stats before returning.
    if (auto clientOpCtx = client->getOperationContext()) {
        if (auto lockerInfo = clientOpCtx->lockState()->getLockerInfo()) {
            fillLockerInfo(*lockerInfo, builder);
        }
    }

    return builder.obj();
}

void PipelineD::MongoDInterface::_reportCurrentOpsForIdleSessions(OperationContext* opCtx,
                                                                  CurrentOpUserMode userMode,
                                                                  std::vector<BSONObj>* ops) const {
    auto sessionCatalog = SessionCatalog::get(opCtx);

    const bool authEnabled =
        AuthorizationSession::get(opCtx->getClient())->getAuthorizationManager().isAuthEnabled();

    // If the user is listing only their own ops, we use makeSessionFilterForAuthenticatedUsers to
    // create a pattern that will match against all authenticated usernames for the current client.
    // If the user is listing ops for all users, we create an empty pattern; constructing an
    // instance of SessionKiller::Matcher with this empty pattern will return all sessions.
    auto sessionFilter = (authEnabled && userMode == CurrentOpUserMode::kExcludeOthers
                              ? makeSessionFilterForAuthenticatedUsers(opCtx)
                              : KillAllSessionsByPatternSet{{}});

    sessionCatalog->scanSessions(opCtx,
                                 {std::move(sessionFilter)},
                                 [&](OperationContext* opCtx, Session* session) {
                                     auto op = session->reportStashedState();
                                     if (!op.isEmpty()) {
                                         ops->emplace_back(op);
                                     }
                                 });
}

std::unique_ptr<CollatorInterface> PipelineD::MongoDInterface::_getCollectionDefaultCollator(
    OperationContext* opCtx, StringData dbName, UUID collectionUUID) {
    auto it = _collatorCache.find(collectionUUID);
    if (it == _collatorCache.end()) {
        auto collator = [&]() -> std::unique_ptr<CollatorInterface> {
            AutoGetCollection autoColl(opCtx, {dbName.toString(), collectionUUID}, MODE_IS);
            if (!autoColl.getCollection()) {
                // This collection doesn't exist, so assume a nullptr default collation
                return nullptr;
            } else {
                auto defaultCollator = autoColl.getCollection()->getDefaultCollator();
                // Clone the collator so that we can safely use the pointer if the collection
                // disappears right after we release the lock.
                return defaultCollator ? defaultCollator->clone() : nullptr;
            }
        }();

        it = _collatorCache.emplace(collectionUUID, std::move(collator)).first;
    }

    auto& collator = it->second;
    return collator ? collator->clone() : nullptr;
}

}  // namespace mongo
