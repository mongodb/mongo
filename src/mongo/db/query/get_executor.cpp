/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/get_executor.h"

#include <boost/optional.hpp>
#include <limits>
#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/base/parse_number.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/count.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/group.h"
#include "mongo/db/exec/idhack.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/oplogstart.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/shard_filter.h"
#include "mongo/db/exec/sort_key_generator.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/ops/update_lifecycle.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

using std::unique_ptr;
using std::string;
using std::vector;
using stdx::make_unique;

// static
void filterAllowedIndexEntries(const AllowedIndicesFilter& allowedIndicesFilter,
                               std::vector<IndexEntry>* indexEntries) {
    invariant(indexEntries);

    // Filter index entries
    // Check BSON objects in AllowedIndices::_indexKeyPatterns against IndexEntry::keyPattern.
    // Removes IndexEntrys that do not match _indexKeyPatterns.
    std::vector<IndexEntry> temp;
    for (std::vector<IndexEntry>::const_iterator i = indexEntries->begin();
         i != indexEntries->end();
         ++i) {
        const IndexEntry& indexEntry = *i;
        if (allowedIndicesFilter.allows(indexEntry)) {
            // Copy index entry into temp vector if found in query settings.
            temp.push_back(indexEntry);
        }
    }

    // Update results.
    temp.swap(*indexEntries);
}

namespace {
// The body is below in the "count hack" section but getExecutor calls it.
bool turnIxscanIntoCount(QuerySolution* soln);
}  // namespace


void fillOutPlannerParams(OperationContext* opCtx,
                          Collection* collection,
                          CanonicalQuery* canonicalQuery,
                          QueryPlannerParams* plannerParams) {
    // If it's not NULL, we may have indices.  Access the catalog and fill out IndexEntry(s)
    IndexCatalog::IndexIterator ii = collection->getIndexCatalog()->getIndexIterator(opCtx, false);
    while (ii.more()) {
        const IndexDescriptor* desc = ii.next();
        IndexCatalogEntry* ice = ii.catalogEntry(desc);
        plannerParams->indices.push_back(IndexEntry(desc->keyPattern(),
                                                    desc->getAccessMethodName(),
                                                    desc->isMultikey(opCtx),
                                                    ice->getMultikeyPaths(opCtx),
                                                    desc->isSparse(),
                                                    desc->unique(),
                                                    desc->indexName(),
                                                    ice->getFilterExpression(),
                                                    desc->infoObj(),
                                                    ice->getCollator()));
    }

    // If query supports index filters, filter params.indices by indices in query settings.
    // Ignore index filters when it is possible to use the id-hack.
    if (!IDHackStage::supportsQuery(collection, *canonicalQuery)) {
        QuerySettings* querySettings = collection->infoCache()->getQuerySettings();
        PlanCacheKey planCacheKey =
            collection->infoCache()->getPlanCache()->computeKey(*canonicalQuery);

        // Filter index catalog if index filters are specified for query.
        // Also, signal to planner that application hint should be ignored.
        if (boost::optional<AllowedIndicesFilter> allowedIndicesFilter =
                querySettings->getAllowedIndicesFilter(planCacheKey)) {
            filterAllowedIndexEntries(*allowedIndicesFilter, &plannerParams->indices);
            plannerParams->indexFiltersApplied = true;
        }
    }

    // We will not output collection scans unless there are no indexed solutions. NO_TABLE_SCAN
    // overrides this behavior by not outputting a collscan even if there are no indexed
    // solutions.
    if (storageGlobalParams.noTableScan.load()) {
        const string& ns = canonicalQuery->ns();
        // There are certain cases where we ignore this restriction:
        bool ignore = canonicalQuery->getQueryObj().isEmpty() ||
            (string::npos != ns.find(".system.")) || (0 == ns.find("local."));
        if (!ignore) {
            plannerParams->options |= QueryPlannerParams::NO_TABLE_SCAN;
        }
    }

    // If the caller wants a shard filter, make sure we're actually sharded.
    if (plannerParams->options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
        auto collMetadata =
            CollectionShardingState::get(opCtx, canonicalQuery->nss())->getMetadata();
        if (collMetadata) {
            plannerParams->shardKey = collMetadata->getKeyPattern();
        } else {
            // If there's no metadata don't bother w/the shard filter since we won't know what
            // the key pattern is anyway...
            plannerParams->options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
        }
    }

    if (internalQueryPlannerEnableIndexIntersection.load()) {
        plannerParams->options |= QueryPlannerParams::INDEX_INTERSECTION;
    }

    if (internalQueryPlannerGenerateCoveredWholeIndexScans.load()) {
        plannerParams->options |= QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    }

    plannerParams->options |= QueryPlannerParams::SPLIT_LIMITED_SORT;

    // Doc-level locking storage engines cannot answer predicates implicitly via exact index
    // bounds for index intersection plans, as this can lead to spurious matches.
    //
    // Such storage engines do not use the invalidation framework, and therefore
    // have no need for KEEP_MUTATIONS.
    if (supportsDocLocking()) {
        plannerParams->options |= QueryPlannerParams::CANNOT_TRIM_IXISECT;
    } else {
        plannerParams->options |= QueryPlannerParams::KEEP_MUTATIONS;
    }

    // MMAPv1 storage engine should have snapshot() perform an index scan on _id rather than a
    // collection scan since a collection scan on the MMAP storage engine can return duplicates
    // or miss documents.
    if (isMMAPV1()) {
        plannerParams->options |= QueryPlannerParams::SNAPSHOT_USE_ID;
    }
}

namespace {

struct PrepareExecutionResult {
    PrepareExecutionResult(unique_ptr<CanonicalQuery> canonicalQuery,
                           unique_ptr<QuerySolution> querySolution,
                           unique_ptr<PlanStage> root)
        : canonicalQuery(std::move(canonicalQuery)),
          querySolution(std::move(querySolution)),
          root(std::move(root)) {}

    unique_ptr<CanonicalQuery> canonicalQuery;
    unique_ptr<QuerySolution> querySolution;
    unique_ptr<PlanStage> root;
};

/**
 * Build an execution tree for the query described in 'canonicalQuery'.
 *
 * If an execution tree could be created, then returns a PrepareExecutionResult that wraps:
 * - The CanonicalQuery describing the query operation. This may be equal to the original canonical
 *   query, or may be modified. This will never be null.
 * - A QuerySolution, representing the associated query solution. This may be null, in certain
 *   circumstances where the constructed execution tree does not have an associated query solution.
 * - A PlanStage, representing the root of the constructed execution tree. This will never be null.
 *
 * If an execution tree could not be created, returns an error Status.
 */
StatusWith<PrepareExecutionResult> prepareExecution(OperationContext* opCtx,
                                                    Collection* collection,
                                                    WorkingSet* ws,
                                                    unique_ptr<CanonicalQuery> canonicalQuery,
                                                    size_t plannerOptions) {
    invariant(canonicalQuery);

    unique_ptr<PlanStage> root;
    unique_ptr<QuerySolution> querySolution;

    // This can happen as we're called by internal clients as well.
    if (NULL == collection) {
        const string& ns = canonicalQuery->ns();
        LOG(2) << "Collection " << ns << " does not exist."
               << " Using EOF plan: " << redact(canonicalQuery->toStringShort());
        root = make_unique<EOFStage>(opCtx);
        return PrepareExecutionResult(
            std::move(canonicalQuery), std::move(querySolution), std::move(root));
    }

    // Fill out the planning params.  We use these for both cached solutions and non-cached.
    QueryPlannerParams plannerParams;
    plannerParams.options = plannerOptions;
    fillOutPlannerParams(opCtx, collection, canonicalQuery.get(), &plannerParams);

    // If the canonical query does not have a user-specified collation, set it from the collection
    // default.
    if (canonicalQuery->getQueryRequest().getCollation().isEmpty() &&
        collection->getDefaultCollator()) {
        canonicalQuery->setCollator(collection->getDefaultCollator()->clone());
    }

    const IndexDescriptor* descriptor = collection->getIndexCatalog()->findIdIndex(opCtx);

    // If we have an _id index we can use an idhack plan.
    if (descriptor && IDHackStage::supportsQuery(collection, *canonicalQuery)) {
        LOG(2) << "Using idhack: " << redact(canonicalQuery->toStringShort());

        root = make_unique<IDHackStage>(opCtx, collection, canonicalQuery.get(), ws, descriptor);

        // Might have to filter out orphaned docs.
        if (plannerParams.options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
            root = make_unique<ShardFilterStage>(
                opCtx,
                CollectionShardingState::get(opCtx, canonicalQuery->nss())->getMetadata(),
                ws,
                root.release());
        }

        // There might be a projection. The idhack stage will always fetch the full
        // document, so we don't support covered projections. However, we might use the
        // simple inclusion fast path.
        if (NULL != canonicalQuery->getProj()) {
            ProjectionStageParams params(ExtensionsCallbackReal(opCtx, &collection->ns()));
            params.projObj = canonicalQuery->getProj()->getProjObj();
            params.collator = canonicalQuery->getCollator();

            // Add a SortKeyGeneratorStage if there is a $meta sortKey projection.
            if (canonicalQuery->getProj()->wantSortKey()) {
                root =
                    make_unique<SortKeyGeneratorStage>(opCtx,
                                                       root.release(),
                                                       ws,
                                                       canonicalQuery->getQueryRequest().getSort(),
                                                       canonicalQuery->getCollator());
            }

            // Stuff the right data into the params depending on what proj impl we use.
            if (canonicalQuery->getProj()->requiresDocument() ||
                canonicalQuery->getProj()->wantIndexKey() ||
                canonicalQuery->getProj()->wantSortKey() ||
                canonicalQuery->getProj()->hasDottedFieldPath()) {
                params.fullExpression = canonicalQuery->root();
                params.projImpl = ProjectionStageParams::NO_FAST_PATH;
            } else {
                params.projImpl = ProjectionStageParams::SIMPLE_DOC;
            }

            root = make_unique<ProjectionStage>(opCtx, params, ws, root.release());
        }

        return PrepareExecutionResult(
            std::move(canonicalQuery), std::move(querySolution), std::move(root));
    }

    // Tailable: If the query requests tailable the collection must be capped.
    if (canonicalQuery->getQueryRequest().isTailable()) {
        if (!collection->isCapped()) {
            return Status(ErrorCodes::BadValue,
                          "error processing query: " + canonicalQuery->toString() +
                              " tailable cursor requested on non capped collection");
        }
    }

    // Try to look up a cached solution for the query.
    CachedSolution* rawCS;
    if (PlanCache::shouldCacheQuery(*canonicalQuery) &&
        collection->infoCache()->getPlanCache()->get(*canonicalQuery, &rawCS).isOK()) {
        // We have a CachedSolution.  Have the planner turn it into a QuerySolution.
        unique_ptr<CachedSolution> cs(rawCS);
        QuerySolution* qs;
        Status status = QueryPlanner::planFromCache(*canonicalQuery, plannerParams, *cs, &qs);

        if (status.isOK()) {
            if ((plannerParams.options & QueryPlannerParams::IS_COUNT) && turnIxscanIntoCount(qs)) {
                LOG(2) << "Using fast count: " << redact(canonicalQuery->toStringShort());
            }

            PlanStage* rawRoot;
            verify(StageBuilder::build(opCtx, collection, *canonicalQuery, *qs, ws, &rawRoot));

            // Add a CachedPlanStage on top of the previous root.
            //
            // 'decisionWorks' is used to determine whether the existing cache entry should
            // be evicted, and the query replanned.
            root = make_unique<CachedPlanStage>(opCtx,
                                                collection,
                                                ws,
                                                canonicalQuery.get(),
                                                plannerParams,
                                                cs->decisionWorks,
                                                rawRoot);
            querySolution.reset(qs);
            return PrepareExecutionResult(
                std::move(canonicalQuery), std::move(querySolution), std::move(root));
        }
    }

    if (internalQueryPlanOrChildrenIndependently.load() &&
        SubplanStage::canUseSubplanning(*canonicalQuery)) {
        LOG(2) << "Running query as sub-queries: " << redact(canonicalQuery->toStringShort());

        root =
            make_unique<SubplanStage>(opCtx, collection, ws, plannerParams, canonicalQuery.get());
        return PrepareExecutionResult(
            std::move(canonicalQuery), std::move(querySolution), std::move(root));
    }

    vector<QuerySolution*> solutions;
    Status status = QueryPlanner::plan(*canonicalQuery, plannerParams, &solutions);
    if (!status.isOK()) {
        return Status(ErrorCodes::BadValue,
                      "error processing query: " + canonicalQuery->toString() +
                          " planner returned error: " + status.reason());
    }

    // We cannot figure out how to answer the query.  Perhaps it requires an index
    // we do not have?
    if (0 == solutions.size()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "error processing query: " << canonicalQuery->toString()
                                    << " No query solutions");
    }

    // See if one of our solutions is a fast count hack in disguise.
    if (plannerParams.options & QueryPlannerParams::IS_COUNT) {
        for (size_t i = 0; i < solutions.size(); ++i) {
            if (turnIxscanIntoCount(solutions[i])) {
                // Great, we can use solutions[i].  Clean up the other QuerySolution(s).
                for (size_t j = 0; j < solutions.size(); ++j) {
                    if (j != i) {
                        delete solutions[j];
                    }
                }

                // We're not going to cache anything that's fast count.
                PlanStage* rawRoot;
                verify(StageBuilder::build(
                    opCtx, collection, *canonicalQuery, *solutions[i], ws, &rawRoot));
                root.reset(rawRoot);

                LOG(2) << "Using fast count: " << redact(canonicalQuery->toStringShort())
                       << ", planSummary: " << redact(Explain::getPlanSummary(root.get()));

                querySolution.reset(solutions[i]);
                return PrepareExecutionResult(
                    std::move(canonicalQuery), std::move(querySolution), std::move(root));
            }
        }
    }

    if (1 == solutions.size()) {
        // Only one possible plan.  Run it.  Build the stages from the solution.
        PlanStage* rawRoot;
        verify(
            StageBuilder::build(opCtx, collection, *canonicalQuery, *solutions[0], ws, &rawRoot));
        root.reset(rawRoot);

        LOG(2) << "Only one plan is available; it will be run but will not be cached. "
               << redact(canonicalQuery->toStringShort())
               << ", planSummary: " << redact(Explain::getPlanSummary(root.get()));

        querySolution.reset(solutions[0]);
        return PrepareExecutionResult(
            std::move(canonicalQuery), std::move(querySolution), std::move(root));
    } else {
        // Many solutions. Create a MultiPlanStage to pick the best, update the cache,
        // and so on. The working set will be shared by all candidate plans.
        auto multiPlanStage = make_unique<MultiPlanStage>(opCtx, collection, canonicalQuery.get());

        for (size_t ix = 0; ix < solutions.size(); ++ix) {
            if (solutions[ix]->cacheData.get()) {
                solutions[ix]->cacheData->indexFilterApplied = plannerParams.indexFiltersApplied;
            }

            // version of StageBuild::build when WorkingSet is shared
            PlanStage* nextPlanRoot;
            verify(StageBuilder::build(
                opCtx, collection, *canonicalQuery, *solutions[ix], ws, &nextPlanRoot));

            // Owns none of the arguments
            multiPlanStage->addPlan(solutions[ix], nextPlanRoot, ws);
        }

        root = std::move(multiPlanStage);
        return PrepareExecutionResult(
            std::move(canonicalQuery), std::move(querySolution), std::move(root));
    }
}

}  // namespace

StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutor(
    OperationContext* opCtx,
    Collection* collection,
    unique_ptr<CanonicalQuery> canonicalQuery,
    PlanExecutor::YieldPolicy yieldPolicy,
    size_t plannerOptions) {
    unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
    StatusWith<PrepareExecutionResult> executionResult =
        prepareExecution(opCtx, collection, ws.get(), std::move(canonicalQuery), plannerOptions);
    if (!executionResult.isOK()) {
        return executionResult.getStatus();
    }
    invariant(executionResult.getValue().root);
    // We must have a tree of stages in order to have a valid plan executor, but the query
    // solution may be null.
    return PlanExecutor::make(opCtx,
                              std::move(ws),
                              std::move(executionResult.getValue().root),
                              std::move(executionResult.getValue().querySolution),
                              std::move(executionResult.getValue().canonicalQuery),
                              collection,
                              yieldPolicy);
}

//
// Find
//

namespace {

/**
 * Returns true if 'me' is a GTE or GE predicate over the "ts" field.
 * Such predicates can be used for the oplog start hack.
 */
bool isOplogTsPred(const mongo::MatchExpression* me) {
    if (mongo::MatchExpression::GT != me->matchType() &&
        mongo::MatchExpression::GTE != me->matchType()) {
        return false;
    }

    return mongoutils::str::equals(me->path().rawData(), "ts");
}

mongo::BSONElement extractOplogTsOptime(const mongo::MatchExpression* me) {
    invariant(isOplogTsPred(me));
    return static_cast<const mongo::ComparisonMatchExpression*>(me)->getData();
}

StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getOplogStartHack(
    OperationContext* opCtx, Collection* collection, unique_ptr<CanonicalQuery> cq) {
    invariant(collection);
    invariant(cq.get());

    if (!collection->isCapped()) {
        return Status(ErrorCodes::BadValue,
                      "OplogReplay cursor requested on non-capped collection");
    }

    // If the canonical query does not have a user-specified collation, set it from the collection
    // default.
    if (cq->getQueryRequest().getCollation().isEmpty() && collection->getDefaultCollator()) {
        cq->setCollator(collection->getDefaultCollator()->clone());
    }

    // A query can only do oplog start finding if it has a top-level $gt or $gte predicate over
    // the "ts" field (the operation's timestamp). Find that predicate and pass it to
    // the OplogStart stage.
    MatchExpression* tsExpr = NULL;
    if (MatchExpression::AND == cq->root()->matchType()) {
        // The query has an AND at the top-level. See if any of the children
        // of the AND are $gt or $gte predicates over 'ts'.
        for (size_t i = 0; i < cq->root()->numChildren(); ++i) {
            MatchExpression* me = cq->root()->getChild(i);
            if (isOplogTsPred(me)) {
                tsExpr = me;
                break;
            }
        }
    } else if (isOplogTsPred(cq->root())) {
        // The root of the tree is a $gt or $gte predicate over 'ts'.
        tsExpr = cq->root();
    }

    if (NULL == tsExpr) {
        return Status(ErrorCodes::OplogOperationUnsupported,
                      "OplogReplay query does not contain top-level "
                      "$gt or $gte over the 'ts' field.");
    }

    boost::optional<RecordId> startLoc = boost::none;

    // See if the RecordStore supports the oplogStartHack
    const BSONElement tsElem = extractOplogTsOptime(tsExpr);
    if (tsElem.type() == bsonTimestamp) {
        StatusWith<RecordId> goal = oploghack::keyForOptime(tsElem.timestamp());
        if (goal.isOK()) {
            startLoc = collection->getRecordStore()->oplogStartHack(opCtx, goal.getValue());
        }
    }

    if (startLoc) {
        LOG(3) << "Using direct oplog seek";
    } else {
        LOG(3) << "Using OplogStart stage";

        // Fallback to trying the OplogStart stage.
        unique_ptr<WorkingSet> oplogws = make_unique<WorkingSet>();
        unique_ptr<OplogStart> stage =
            make_unique<OplogStart>(opCtx, collection, tsExpr, oplogws.get());
        // Takes ownership of oplogws and stage.
        auto statusWithPlanExecutor = PlanExecutor::make(
            opCtx, std::move(oplogws), std::move(stage), collection, PlanExecutor::YIELD_AUTO);
        invariant(statusWithPlanExecutor.isOK());
        unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec =
            std::move(statusWithPlanExecutor.getValue());

        // The stage returns a RecordId of where to start.
        startLoc = RecordId();
        PlanExecutor::ExecState state = exec->getNext(NULL, startLoc.get_ptr());

        // This is normal.  The start of the oplog is the beginning of the collection.
        if (PlanExecutor::IS_EOF == state) {
            return getExecutor(opCtx, collection, std::move(cq), PlanExecutor::YIELD_AUTO);
        }

        // This is not normal.  An error was encountered.
        if (PlanExecutor::ADVANCED != state) {
            return Status(ErrorCodes::InternalError, "quick oplog start location had error...?");
        }
    }

    // Build our collection scan...
    CollectionScanParams params;
    params.collection = collection;
    params.start = *startLoc;
    params.direction = CollectionScanParams::FORWARD;
    params.tailable = cq->getQueryRequest().isTailable();

    // If the query is just tsExpr, we know that every document in the collection after the first
    // matching one must also match. To avoid wasting time running the match expression on every
    // document to be returned, we tell the CollectionScan stage to stop applying the filter once it
    // finds the first match.
    if (cq->root() == tsExpr) {
        params.stopApplyingFilterAfterFirstMatch = true;
    }

    unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
    unique_ptr<CollectionScan> cs =
        make_unique<CollectionScan>(opCtx, params, ws.get(), cq->root());
    // Takes ownership of 'ws', 'cs', and 'cq'.
    return PlanExecutor::make(
        opCtx, std::move(ws), std::move(cs), std::move(cq), collection, PlanExecutor::YIELD_AUTO);
}

}  // namespace

StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorFind(
    OperationContext* opCtx,
    Collection* collection,
    const NamespaceString& nss,
    unique_ptr<CanonicalQuery> canonicalQuery,
    PlanExecutor::YieldPolicy yieldPolicy) {
    if (NULL != collection && canonicalQuery->getQueryRequest().isOplogReplay()) {
        return getOplogStartHack(opCtx, collection, std::move(canonicalQuery));
    }

    size_t options = QueryPlannerParams::DEFAULT;
    if (ShardingState::get(opCtx)->needCollectionMetadata(opCtx, nss.ns())) {
        options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    }
    return getExecutor(
        opCtx, collection, std::move(canonicalQuery), PlanExecutor::YIELD_AUTO, options);
}

namespace {

/**
 * Wrap the specified 'root' plan stage in a ProjectionStage. Does not take ownership of any
 * arguments other than root.
 *
 * If the projection was valid, then return Status::OK() with a pointer to the newly created
 * ProjectionStage. Otherwise, return a status indicating the error reason.
 */
StatusWith<unique_ptr<PlanStage>> applyProjection(OperationContext* opCtx,
                                                  const NamespaceString& nsString,
                                                  CanonicalQuery* cq,
                                                  const BSONObj& proj,
                                                  bool allowPositional,
                                                  WorkingSet* ws,
                                                  unique_ptr<PlanStage> root) {
    invariant(!proj.isEmpty());

    ParsedProjection* rawParsedProj;
    Status ppStatus = ParsedProjection::make(
        proj.getOwned(), cq->root(), &rawParsedProj, ExtensionsCallbackDisallowExtensions());
    if (!ppStatus.isOK()) {
        return ppStatus;
    }
    unique_ptr<ParsedProjection> pp(rawParsedProj);

    // ProjectionExec requires the MatchDetails from the query expression when the projection
    // uses the positional operator. Since the query may no longer match the newly-updated
    // document, we forbid this case.
    if (!allowPositional && pp->requiresMatchDetails()) {
        return {ErrorCodes::BadValue,
                "cannot use a positional projection and return the new document"};
    }

    // $meta sortKey is not allowed to be projected in findAndModify commands.
    if (pp->wantSortKey()) {
        return {ErrorCodes::BadValue,
                "Cannot use a $meta sortKey projection in findAndModify commands."};
    }

    ProjectionStageParams params(ExtensionsCallbackReal(opCtx, &nsString));
    params.projObj = proj;
    params.collator = cq->getCollator();
    params.fullExpression = cq->root();
    return {make_unique<ProjectionStage>(opCtx, params, ws, root.release())};
}

}  // namespace

//
// Delete
//

StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorDelete(
    OperationContext* opCtx, OpDebug* opDebug, Collection* collection, ParsedDelete* parsedDelete) {
    const DeleteRequest* request = parsedDelete->getRequest();

    const NamespaceString& nss(request->getNamespaceString());
    if (!request->isGod()) {
        if (nss.isSystem() && opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
            uassert(12050, "cannot delete from system namespace", nss.isLegalClientSystemNS());
        }
        if (nss.isVirtualized()) {
            log() << "cannot delete from a virtual collection: " << nss;
            uasserted(10100, "cannot delete from a virtual collection");
        }
    }

    if (collection && collection->isCapped()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "cannot remove from a capped collection: " << nss.ns());
    }

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::PrimarySteppedDown,
                      str::stream() << "Not primary while removing from " << nss.ns());
    }

    DeleteStageParams deleteStageParams;
    deleteStageParams.isMulti = request->isMulti();
    deleteStageParams.fromMigrate = request->isFromMigrate();
    deleteStageParams.isExplain = request->isExplain();
    deleteStageParams.returnDeleted = request->shouldReturnDeleted();
    deleteStageParams.sort = request->getSort();
    deleteStageParams.opDebug = opDebug;
    deleteStageParams.stmtId = request->getStmtId();

    unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
    const PlanExecutor::YieldPolicy policy = parsedDelete->yieldPolicy();

    if (!parsedDelete->hasParsedQuery()) {
        // This is the idhack fast-path for getting a PlanExecutor without doing the work
        // to create a CanonicalQuery.
        const BSONObj& unparsedQuery = request->getQuery();

        if (!collection) {
            // Treat collections that do not exist as empty collections.  Note that the explain
            // reporting machinery always assumes that the root stage for a delete operation is
            // a DeleteStage, so in this case we put a DeleteStage on top of an EOFStage.
            LOG(2) << "Collection " << nss.ns() << " does not exist."
                   << " Using EOF stage: " << redact(unparsedQuery);
            auto deleteStage = make_unique<DeleteStage>(
                opCtx, deleteStageParams, ws.get(), nullptr, new EOFStage(opCtx));
            return PlanExecutor::make(opCtx, std::move(ws), std::move(deleteStage), nss, policy);
        }

        const IndexDescriptor* descriptor = collection->getIndexCatalog()->findIdIndex(opCtx);

        // Construct delete request collator.
        std::unique_ptr<CollatorInterface> collator;
        if (!request->getCollation().isEmpty()) {
            auto statusWithCollator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                          ->makeFromBSON(request->getCollation());
            if (!statusWithCollator.isOK()) {
                return statusWithCollator.getStatus();
            }
            collator = std::move(statusWithCollator.getValue());
        }
        const bool hasCollectionDefaultCollation = request->getCollation().isEmpty() ||
            CollatorInterface::collatorsMatch(collator.get(), collection->getDefaultCollator());

        if (descriptor && CanonicalQuery::isSimpleIdQuery(unparsedQuery) &&
            request->getProj().isEmpty() && hasCollectionDefaultCollation) {
            LOG(2) << "Using idhack: " << redact(unparsedQuery);

            PlanStage* idHackStage = new IDHackStage(
                opCtx, collection, unparsedQuery["_id"].wrap(), ws.get(), descriptor);
            unique_ptr<DeleteStage> root = make_unique<DeleteStage>(
                opCtx, deleteStageParams, ws.get(), collection, idHackStage);
            return PlanExecutor::make(opCtx, std::move(ws), std::move(root), collection, policy);
        }

        // If we're here then we don't have a parsed query, but we're also not eligible for
        // the idhack fast path. We need to force canonicalization now.
        Status cqStatus = parsedDelete->parseQueryToCQ();
        if (!cqStatus.isOK()) {
            return cqStatus;
        }
    }

    // This is the regular path for when we have a CanonicalQuery.
    unique_ptr<CanonicalQuery> cq(parsedDelete->releaseParsedQuery());

    const size_t defaultPlannerOptions = 0;
    StatusWith<PrepareExecutionResult> executionResult =
        prepareExecution(opCtx, collection, ws.get(), std::move(cq), defaultPlannerOptions);
    if (!executionResult.isOK()) {
        return executionResult.getStatus();
    }
    cq = std::move(executionResult.getValue().canonicalQuery);
    unique_ptr<QuerySolution> querySolution = std::move(executionResult.getValue().querySolution);
    unique_ptr<PlanStage> root = std::move(executionResult.getValue().root);

    deleteStageParams.canonicalQuery = cq.get();

    invariant(root);
    root = make_unique<DeleteStage>(opCtx, deleteStageParams, ws.get(), collection, root.release());

    if (!request->getProj().isEmpty()) {
        invariant(request->shouldReturnDeleted());

        const bool allowPositional = true;
        StatusWith<unique_ptr<PlanStage>> projStatus = applyProjection(
            opCtx, nss, cq.get(), request->getProj(), allowPositional, ws.get(), std::move(root));
        if (!projStatus.isOK()) {
            return projStatus.getStatus();
        }
        root = std::move(projStatus.getValue());
    }

    // We must have a tree of stages in order to have a valid plan executor, but the query
    // solution may be null.
    return PlanExecutor::make(opCtx,
                              std::move(ws),
                              std::move(root),
                              std::move(querySolution),
                              std::move(cq),
                              collection,
                              policy);
}

//
// Update
//

StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorUpdate(
    OperationContext* opCtx, OpDebug* opDebug, Collection* collection, ParsedUpdate* parsedUpdate) {
    const UpdateRequest* request = parsedUpdate->getRequest();
    UpdateDriver* driver = parsedUpdate->getDriver();

    const NamespaceString& nss = request->getNamespaceString();
    UpdateLifecycle* lifecycle = request->getLifecycle();

    if (nss.isSystem() && opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
        uassert(10156, "cannot update a system namespace", nss.isLegalClientSystemNS());
    }
    if (nss.isVirtualized()) {
        log() << "cannot update a virtual collection: " << nss;
        uasserted(10155, "cannot update a virtual collection");
    }

    // If there is no collection and this is an upsert, callers are supposed to create
    // the collection prior to calling this method. Explain, however, will never do
    // collection or database creation.
    if (!collection && request->isUpsert()) {
        invariant(request->isExplain());
    }

    // If the parsed update does not have a user-specified collation, set it from the collection
    // default.
    if (collection && parsedUpdate->getRequest()->getCollation().isEmpty() &&
        collection->getDefaultCollator()) {
        parsedUpdate->setCollator(collection->getDefaultCollator()->clone());
    }

    // If this is a user-issued update, then we want to return an error: you cannot perform
    // writes on a secondary. If this is an update to a secondary from the replication system,
    // however, then we make an exception and let the write proceed.
    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::PrimarySteppedDown,
                      str::stream() << "Not primary while performing update on " << nss.ns());
    }

    if (lifecycle) {
        lifecycle->setCollection(collection);
        driver->refreshIndexKeys(lifecycle->getIndexKeys(opCtx));
    }

    const PlanExecutor::YieldPolicy policy = parsedUpdate->yieldPolicy();

    unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
    UpdateStageParams updateStageParams(request, driver, opDebug);

    if (!parsedUpdate->hasParsedQuery()) {
        // This is the idhack fast-path for getting a PlanExecutor without doing the work
        // to create a CanonicalQuery.
        const BSONObj& unparsedQuery = request->getQuery();

        if (!collection) {
            // Treat collections that do not exist as empty collections. Note that the explain
            // reporting machinery always assumes that the root stage for an update operation is
            // an UpdateStage, so in this case we put an UpdateStage on top of an EOFStage.
            LOG(2) << "Collection " << nss.ns() << " does not exist."
                   << " Using EOF stage: " << redact(unparsedQuery);
            auto updateStage = make_unique<UpdateStage>(
                opCtx, updateStageParams, ws.get(), collection, new EOFStage(opCtx));
            return PlanExecutor::make(opCtx, std::move(ws), std::move(updateStage), nss, policy);
        }

        const IndexDescriptor* descriptor = collection->getIndexCatalog()->findIdIndex(opCtx);

        const bool hasCollectionDefaultCollation = CollatorInterface::collatorsMatch(
            parsedUpdate->getCollator(), collection->getDefaultCollator());

        if (descriptor && CanonicalQuery::isSimpleIdQuery(unparsedQuery) &&
            request->getProj().isEmpty() && hasCollectionDefaultCollation) {
            LOG(2) << "Using idhack: " << redact(unparsedQuery);

            // Working set 'ws' is discarded. InternalPlanner::updateWithIdHack() makes its own
            // WorkingSet.
            return InternalPlanner::updateWithIdHack(opCtx,
                                                     collection,
                                                     updateStageParams,
                                                     descriptor,
                                                     unparsedQuery["_id"].wrap(),
                                                     policy);
        }

        // If we're here then we don't have a parsed query, but we're also not eligible for
        // the idhack fast path. We need to force canonicalization now.
        Status cqStatus = parsedUpdate->parseQueryToCQ();
        if (!cqStatus.isOK()) {
            return cqStatus;
        }
    }

    // This is the regular path for when we have a CanonicalQuery.
    unique_ptr<CanonicalQuery> cq(parsedUpdate->releaseParsedQuery());

    const size_t defaultPlannerOptions = 0;
    StatusWith<PrepareExecutionResult> executionResult =
        prepareExecution(opCtx, collection, ws.get(), std::move(cq), defaultPlannerOptions);
    if (!executionResult.isOK()) {
        return executionResult.getStatus();
    }
    cq = std::move(executionResult.getValue().canonicalQuery);
    unique_ptr<QuerySolution> querySolution = std::move(executionResult.getValue().querySolution);
    unique_ptr<PlanStage> root = std::move(executionResult.getValue().root);

    invariant(root);
    updateStageParams.canonicalQuery = cq.get();

    root = stdx::make_unique<UpdateStage>(
        opCtx, updateStageParams, ws.get(), collection, root.release());

    if (!request->getProj().isEmpty()) {
        invariant(request->shouldReturnAnyDocs());

        // If the plan stage is to return the newly-updated version of the documents, then it
        // is invalid to use a positional projection because the query expression need not
        // match the array element after the update has been applied.
        const bool allowPositional = request->shouldReturnOldDocs();
        StatusWith<unique_ptr<PlanStage>> projStatus = applyProjection(
            opCtx, nss, cq.get(), request->getProj(), allowPositional, ws.get(), std::move(root));
        if (!projStatus.isOK()) {
            return projStatus.getStatus();
        }
        root = std::move(projStatus.getValue());
    }

    // We must have a tree of stages in order to have a valid plan executor, but the query
    // solution may be null. Takes ownership of all args other than 'collection' and 'opCtx'
    return PlanExecutor::make(opCtx,
                              std::move(ws),
                              std::move(root),
                              std::move(querySolution),
                              std::move(cq),
                              collection,
                              policy);
}

//
// Group
//

StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorGroup(
    OperationContext* opCtx,
    Collection* collection,
    const GroupRequest& request,
    PlanExecutor::YieldPolicy yieldPolicy) {
    if (!getGlobalScriptEngine()) {
        return Status(ErrorCodes::BadValue, "server-side JavaScript execution is disabled");
    }

    unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();

    if (!collection) {
        // Treat collections that do not exist as empty collections.  Note that the explain
        // reporting machinery always assumes that the root stage for a group operation is a
        // GroupStage, so in this case we put a GroupStage on top of an EOFStage.
        unique_ptr<PlanStage> root =
            make_unique<GroupStage>(opCtx, request, ws.get(), new EOFStage(opCtx));

        return PlanExecutor::make(opCtx, std::move(ws), std::move(root), request.ns, yieldPolicy);
    }

    const NamespaceString nss(request.ns);
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(request.query);
    qr->setCollation(request.collation);
    qr->setExplain(request.explain);

    const ExtensionsCallbackReal extensionsCallback(opCtx, &nss);

    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx, std::move(qr), extensionsCallback);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }
    unique_ptr<CanonicalQuery> canonicalQuery = std::move(statusWithCQ.getValue());

    const size_t defaultPlannerOptions = 0;
    StatusWith<PrepareExecutionResult> executionResult = prepareExecution(
        opCtx, collection, ws.get(), std::move(canonicalQuery), defaultPlannerOptions);
    if (!executionResult.isOK()) {
        return executionResult.getStatus();
    }
    canonicalQuery = std::move(executionResult.getValue().canonicalQuery);
    unique_ptr<QuerySolution> querySolution = std::move(executionResult.getValue().querySolution);
    unique_ptr<PlanStage> root = std::move(executionResult.getValue().root);

    invariant(root);

    root = make_unique<GroupStage>(opCtx, request, ws.get(), root.release());
    // We must have a tree of stages in order to have a valid plan executor, but the query
    // solution may be null. Takes ownership of all args other than 'collection'.
    return PlanExecutor::make(opCtx,
                              std::move(ws),
                              std::move(root),
                              std::move(querySolution),
                              std::move(canonicalQuery),
                              collection,
                              yieldPolicy);
}

//
// Count hack
//

namespace {

/**
 * Returns 'true' if the provided solution 'soln' can be rewritten to use
 * a fast counting stage.  Mutates the tree in 'soln->root'.
 *
 * Otherwise, returns 'false'.
 */
bool turnIxscanIntoCount(QuerySolution* soln) {
    QuerySolutionNode* root = soln->root.get();

    // Root should be an ixscan or fetch w/o any filters.
    if (!(STAGE_FETCH == root->getType() || STAGE_IXSCAN == root->getType())) {
        return false;
    }

    if (STAGE_FETCH == root->getType() && NULL != root->filter.get()) {
        return false;
    }

    // If the root is a fetch, its child should be an ixscan
    if (STAGE_FETCH == root->getType() && STAGE_IXSCAN != root->children[0]->getType()) {
        return false;
    }

    IndexScanNode* isn = (STAGE_FETCH == root->getType())
        ? static_cast<IndexScanNode*>(root->children[0])
        : static_cast<IndexScanNode*>(root);

    // No filters allowed and side-stepping isSimpleRange for now.  TODO: do we ever see
    // isSimpleRange here?  because we could well use it.  I just don't think we ever do see
    // it.

    if (NULL != isn->filter.get() || isn->bounds.isSimpleRange) {
        return false;
    }

    // Make sure the bounds are OK.
    BSONObj startKey;
    bool startKeyInclusive;
    BSONObj endKey;
    bool endKeyInclusive;

    if (!IndexBoundsBuilder::isSingleInterval(
            isn->bounds, &startKey, &startKeyInclusive, &endKey, &endKeyInclusive)) {
        return false;
    }

    // Make the count node that we replace the fetch + ixscan with.
    CountScanNode* csn = new CountScanNode(isn->index);
    csn->startKey = startKey;
    csn->startKeyInclusive = startKeyInclusive;
    csn->endKey = endKey;
    csn->endKeyInclusive = endKeyInclusive;
    // Takes ownership of 'cn' and deletes the old root.
    soln->root.reset(csn);
    return true;
}

/**
 * Returns true if indices contains an index that can be used with DistinctNode (the "fast distinct
 * hack" node, which can be used only if there is an empty query predicate).  Sets indexOut to the
 * array index of PlannerParams::indices.  Look for the index for the fewest fields.  Criteria for
 * suitable index is that the index cannot be special (geo, hashed, text, ...), and the index cannot
 * be a partial index.
 *
 * Multikey indices are not suitable for DistinctNode when the projection is on an array element.
 * Arrays are flattened in a multikey index which makes it impossible for the distinct scan stage
 * (plan stage generated from DistinctNode) to select the requested element by array index.
 *
 * Multikey indices cannot be used for the fast distinct hack if the field is dotted.  Currently the
 * solution generated for the distinct hack includes a projection stage and the projection stage
 * cannot be covered with a dotted field.
 */
bool getDistinctNodeIndex(const std::vector<IndexEntry>& indices,
                          const std::string& field,
                          const CollatorInterface* collator,
                          size_t* indexOut) {
    invariant(indexOut);
    bool isDottedField = str::contains(field, '.');
    int minFields = std::numeric_limits<int>::max();
    for (size_t i = 0; i < indices.size(); ++i) {
        // Skip indices with non-matching collator.
        if (!CollatorInterface::collatorsMatch(indices[i].collator, collator)) {
            continue;
        }
        // Skip special indices.
        if (!IndexNames::findPluginName(indices[i].keyPattern).empty()) {
            continue;
        }
        // Skip partial indices.
        if (indices[i].filterExpr) {
            continue;
        }
        // Skip multikey indices if we are projecting on a dotted field.
        if (indices[i].multikey && isDottedField) {
            continue;
        }
        // Skip indices where the first key is not field.
        if (indices[i].keyPattern.firstElement().fieldNameStringData() != StringData(field)) {
            continue;
        }
        int nFields = indices[i].keyPattern.nFields();
        // Pick the index with the lowest number of fields.
        if (nFields < minFields) {
            minFields = nFields;
            *indexOut = i;
        }
    }
    return minFields != std::numeric_limits<int>::max();
}

/**
 * Checks dotted field for a projection and truncates the
 * field name if we could be projecting on an array element.
 * Sets 'isIDOut' to true if the projection is on a sub document of _id.
 * For example, _id.a.2, _id.b.c.
 */
std::string getProjectedDottedField(const std::string& field, bool* isIDOut) {
    // Check if field contains an array index.
    std::vector<std::string> res;
    mongo::splitStringDelim(field, &res, '.');

    // Since we could exit early from the loop,
    // we should check _id here and set '*isIDOut' accordingly.
    *isIDOut = ("_id" == res[0]);

    // Skip the first dotted component. If the field starts
    // with a number, the number cannot be an array index.
    int arrayIndex = 0;
    for (size_t i = 1; i < res.size(); ++i) {
        if (mongo::parseNumberFromStringWithBase(res[i], 10, &arrayIndex).isOK()) {
            // Array indices cannot be negative numbers (this is not $slice).
            // Negative numbers are allowed as field names.
            if (arrayIndex >= 0) {
                // Generate prefix of field up to (but not including) array index.
                std::vector<std::string> prefixStrings(res);
                prefixStrings.resize(i);
                // Reset projectedField. Instead of overwriting, joinStringDelim() appends joined
                // string
                // to the end of projectedField.
                std::string projectedField;
                mongo::joinStringDelim(prefixStrings, &projectedField, '.');
                return projectedField;
            }
        }
    }

    return field;
}

/**
 * Creates a projection spec for a distinct command from the requested field.
 * In most cases, the projection spec will be {_id: 0, key: 1}.
 * The exceptions are:
 * 1) When the requested field is '_id', the projection spec will {_id: 1}.
 * 2) When the requested field could be an array element (eg. a.0),
 *    the projected field will be the prefix of the field up to the array element.
 *    For example, a.b.2 => {_id: 0, 'a.b': 1}
 *    Note that we can't use a $slice projection because the distinct command filters
 *    the results from the executor using the dotted field name. Using $slice will
 *    re-order the documents in the array in the results.
 */
BSONObj getDistinctProjection(const std::string& field) {
    std::string projectedField(field);

    bool isID = false;
    if ("_id" == field) {
        isID = true;
    } else if (str::contains(field, '.')) {
        projectedField = getProjectedDottedField(field, &isID);
    }
    BSONObjBuilder bob;
    if (!isID) {
        bob.append("_id", 0);
    }
    bob.append(projectedField, 1);
    return bob.obj();
}

}  // namespace

StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorCount(
    OperationContext* opCtx,
    Collection* collection,
    const CountRequest& request,
    bool explain,
    PlanExecutor::YieldPolicy yieldPolicy) {
    unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();

    auto qr = stdx::make_unique<QueryRequest>(request.getNs());
    qr->setFilter(request.getQuery());
    qr->setCollation(request.getCollation());
    qr->setHint(request.getHint());
    qr->setExplain(explain);

    auto statusWithCQ = CanonicalQuery::canonicalize(
        opCtx,
        std::move(qr),
        collection ? static_cast<const ExtensionsCallback&>(
                         ExtensionsCallbackReal(opCtx, &collection->ns()))
                   : static_cast<const ExtensionsCallback&>(ExtensionsCallbackNoop()));

    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
    if (!collection) {
        // Treat collections that do not exist as empty collections. Note that the explain
        // reporting machinery always assumes that the root stage for a count operation is
        // a CountStage, so in this case we put a CountStage on top of an EOFStage.
        const bool useRecordStoreCount = false;
        CountStageParams params(request, useRecordStoreCount);
        unique_ptr<PlanStage> root = make_unique<CountStage>(
            opCtx, collection, std::move(params), ws.get(), new EOFStage(opCtx));
        return PlanExecutor::make(
            opCtx, std::move(ws), std::move(root), request.getNs(), yieldPolicy);
    }

    // If the query is empty, then we can determine the count by just asking the collection
    // for its number of records. This is implemented by the CountStage, and we don't need
    // to create a child for the count stage in this case.
    //
    // If there is a hint, then we can't use a trival count plan as described above.
    const bool isEmptyQueryPredicate =
        cq->root()->matchType() == MatchExpression::AND && cq->root()->numChildren() == 0;
    const bool useRecordStoreCount = isEmptyQueryPredicate && request.getHint().isEmpty();
    CountStageParams params(request, useRecordStoreCount);

    if (useRecordStoreCount) {
        unique_ptr<PlanStage> root =
            make_unique<CountStage>(opCtx, collection, std::move(params), ws.get(), nullptr);
        return PlanExecutor::make(
            opCtx, std::move(ws), std::move(root), request.getNs(), yieldPolicy);
    }

    const size_t plannerOptions = QueryPlannerParams::IS_COUNT;
    StatusWith<PrepareExecutionResult> executionResult =
        prepareExecution(opCtx, collection, ws.get(), std::move(cq), plannerOptions);
    if (!executionResult.isOK()) {
        return executionResult.getStatus();
    }
    cq = std::move(executionResult.getValue().canonicalQuery);
    unique_ptr<QuerySolution> querySolution = std::move(executionResult.getValue().querySolution);
    unique_ptr<PlanStage> root = std::move(executionResult.getValue().root);

    invariant(root);

    // Make a CountStage to be the new root.
    root = make_unique<CountStage>(opCtx, collection, std::move(params), ws.get(), root.release());
    // We must have a tree of stages in order to have a valid plan executor, but the query
    // solution may be NULL. Takes ownership of all args other than 'collection' and 'opCtx'
    return PlanExecutor::make(opCtx,
                              std::move(ws),
                              std::move(root),
                              std::move(querySolution),
                              std::move(cq),
                              collection,
                              yieldPolicy);
}

//
// Distinct hack
//

bool turnIxscanIntoDistinctIxscan(QuerySolution* soln, const string& field) {
    QuerySolutionNode* root = soln->root.get();

    // Solution must have a filter.
    if (soln->filterData.isEmpty()) {
        return false;
    }

    // Root stage must be a project.
    if (STAGE_PROJECTION != root->getType()) {
        return false;
    }

    // Child should be either an ixscan or fetch.
    if (STAGE_IXSCAN != root->children[0]->getType() &&
        STAGE_FETCH != root->children[0]->getType()) {
        return false;
    }

    IndexScanNode* indexScanNode = nullptr;
    FetchNode* fetchNode = nullptr;
    if (STAGE_IXSCAN == root->children[0]->getType()) {
        indexScanNode = static_cast<IndexScanNode*>(root->children[0]);
    } else {
        fetchNode = static_cast<FetchNode*>(root->children[0]);
        // If the fetch has a filter, we're out of luck. We can't skip all keys with a given value,
        // since one of them may key a document that passes the filter.
        if (fetchNode->filter) {
            return false;
        }

        if (STAGE_IXSCAN != fetchNode->children[0]->getType()) {
            return false;
        }

        indexScanNode = static_cast<IndexScanNode*>(fetchNode->children[0]);
    }

    // An additional filter must be applied to the data in the key, so we can't just skip
    // all the keys with a given value; we must examine every one to find the one that (may)
    // pass the filter.
    if (indexScanNode->filter) {
        return false;
    }

    // We only set this when we have special query modifiers (.max() or .min()) or other
    // special cases.  Don't want to handle the interactions between those and distinct.
    // Don't think this will ever really be true but if it somehow is, just ignore this
    // soln.
    if (indexScanNode->bounds.isSimpleRange) {
        return false;
    }

    // Figure out which field we're skipping to the next value of.
    int fieldNo = 0;
    BSONObjIterator it(indexScanNode->index.keyPattern);
    while (it.more()) {
        if (field == it.next().fieldName()) {
            break;
        }
        ++fieldNo;
    }

    // We should not use a distinct scan if the field over which we are computing the distinct is
    // multikey.
    if (indexScanNode->index.multikey) {
        const auto& multikeyPaths = indexScanNode->index.multikeyPaths;
        if (multikeyPaths.empty()) {
            // We don't have path-level multikey information available.
            return false;
        }

        if (!multikeyPaths[fieldNo].empty()) {
            // Path-level multikey information indicates that the distinct key contains at least one
            // array component.
            return false;
        }
    }

    // Make a new DistinctNode. We will swap this for the ixscan in the provided solution.
    auto distinctNode = stdx::make_unique<DistinctNode>(indexScanNode->index);
    distinctNode->direction = indexScanNode->direction;
    distinctNode->bounds = indexScanNode->bounds;
    distinctNode->fieldNo = fieldNo;

    if (fetchNode) {
        // If there is a fetch node, then there is no need for the projection. The fetch node should
        // become the new root, with the distinct as its child. The PROJECT=>FETCH=>IXSCAN tree
        // should become FETCH=>DISTINCT_SCAN.
        invariant(STAGE_PROJECTION == root->getType());
        invariant(STAGE_FETCH == root->children[0]->getType());
        invariant(STAGE_IXSCAN == root->children[0]->children[0]->getType());

        // Detach the fetch from its parent projection.
        root->children.clear();

        // Make the fetch the new root. This destroys the project stage.
        soln->root.reset(fetchNode);

        // Take ownership of the index scan node, detaching it from the solution tree.
        std::unique_ptr<IndexScanNode> ownedIsn(indexScanNode);

        // Attach the distinct node in the index scan's place.
        fetchNode->children[0] = distinctNode.release();
    } else {
        // There is no fetch node. The PROJECT=>IXSCAN tree should become PROJECT=>DISTINCT_SCAN.
        invariant(STAGE_PROJECTION == root->getType());
        invariant(STAGE_IXSCAN == root->children[0]->getType());

        // Take ownership of the index scan node, detaching it from the solution tree.
        std::unique_ptr<IndexScanNode> ownedIsn(indexScanNode);

        // Attach the distinct node in the index scan's place.
        root->children[0] = distinctNode.release();
    }

    return true;
}

StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorDistinct(
    OperationContext* opCtx,
    Collection* collection,
    const std::string& ns,
    ParsedDistinct* parsedDistinct,
    PlanExecutor::YieldPolicy yieldPolicy) {
    if (!collection) {
        // Treat collections that do not exist as empty collections.
        return PlanExecutor::make(opCtx,
                                  make_unique<WorkingSet>(),
                                  make_unique<EOFStage>(opCtx),
                                  parsedDistinct->releaseQuery(),
                                  collection,
                                  yieldPolicy);
    }

    // TODO: check for idhack here?

    // When can we do a fast distinct hack?
    // 1. There is a plan with just one leaf and that leaf is an ixscan.
    // 2. The ixscan indexes the field we're interested in.
    // 2a: We are correct if the index contains the field but for now we look for prefix.
    // 3. The query is covered/no fetch.
    //
    // We go through normal planning (with limited parameters) to see if we can produce
    // a soln with the above properties.

    QueryPlannerParams plannerParams;
    plannerParams.options = QueryPlannerParams::NO_TABLE_SCAN;

    IndexCatalog::IndexIterator ii = collection->getIndexCatalog()->getIndexIterator(opCtx, false);
    while (ii.more()) {
        const IndexDescriptor* desc = ii.next();
        IndexCatalogEntry* ice = ii.catalogEntry(desc);
        if (desc->keyPattern().hasField(parsedDistinct->getKey())) {
            plannerParams.indices.push_back(IndexEntry(desc->keyPattern(),
                                                       desc->getAccessMethodName(),
                                                       desc->isMultikey(opCtx),
                                                       ice->getMultikeyPaths(opCtx),
                                                       desc->isSparse(),
                                                       desc->unique(),
                                                       desc->indexName(),
                                                       ice->getFilterExpression(),
                                                       desc->infoObj(),
                                                       ice->getCollator()));
        }
    }

    const ExtensionsCallbackReal extensionsCallback(opCtx, &collection->ns());

    // If there are no suitable indices for the distinct hack bail out now into regular planning
    // with no projection.
    if (plannerParams.indices.empty()) {
        return getExecutor(opCtx, collection, parsedDistinct->releaseQuery(), yieldPolicy);
    }

    //
    // If we're here, we have an index prefixed by the field we're distinct-ing over.
    //

    // Applying a projection allows the planner to try to give us covered plans that we can turn
    // into the projection hack.  getDistinctProjection deals with .find() projection semantics
    // (ie _id:1 being implied by default).
    BSONObj projection = getDistinctProjection(parsedDistinct->getKey());

    auto qr = stdx::make_unique<QueryRequest>(parsedDistinct->getQuery()->getQueryRequest());
    qr->setProj(projection);

    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx, std::move(qr), extensionsCallback);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }

    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    // If the canonical query does not have a user-specified collation, set it from the collection
    // default.
    if (cq->getQueryRequest().getCollation().isEmpty() && collection->getDefaultCollator()) {
        cq->setCollator(collection->getDefaultCollator()->clone());
    }

    // If there's no query, we can just distinct-scan one of the indices.
    // Not every index in plannerParams.indices may be suitable. Refer to
    // getDistinctNodeIndex().
    size_t distinctNodeIndex = 0;
    if (parsedDistinct->getQuery()->getQueryRequest().getFilter().isEmpty() &&
        getDistinctNodeIndex(plannerParams.indices,
                             parsedDistinct->getKey(),
                             cq->getCollator(),
                             &distinctNodeIndex)) {
        auto dn = stdx::make_unique<DistinctNode>(plannerParams.indices[distinctNodeIndex]);
        dn->direction = 1;
        IndexBoundsBuilder::allValuesBounds(dn->index.keyPattern, &dn->bounds);
        dn->fieldNo = 0;

        // An index with a non-simple collation requires a FETCH stage.
        std::unique_ptr<QuerySolutionNode> solnRoot = std::move(dn);
        if (plannerParams.indices[distinctNodeIndex].collator) {
            if (!solnRoot->fetched()) {
                auto fetch = stdx::make_unique<FetchNode>();
                fetch->children.push_back(solnRoot.release());
                solnRoot = std::move(fetch);
            }
        }

        QueryPlannerParams params;

        unique_ptr<QuerySolution> soln(
            QueryPlannerAnalysis::analyzeDataAccess(*cq, params, std::move(solnRoot)));
        invariant(soln);

        unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
        PlanStage* rawRoot;
        verify(StageBuilder::build(opCtx, collection, *cq, *soln, ws.get(), &rawRoot));
        unique_ptr<PlanStage> root(rawRoot);

        LOG(2) << "Using fast distinct: " << redact(cq->toStringShort())
               << ", planSummary: " << redact(Explain::getPlanSummary(root.get()));

        return PlanExecutor::make(opCtx,
                                  std::move(ws),
                                  std::move(root),
                                  std::move(soln),
                                  std::move(cq),
                                  collection,
                                  yieldPolicy);
    }

    // See if we can answer the query in a fast-distinct compatible fashion.
    vector<QuerySolution*> solutions;
    Status status = QueryPlanner::plan(*cq, plannerParams, &solutions);
    if (!status.isOK()) {
        return getExecutor(opCtx, collection, std::move(cq), yieldPolicy);
    }

    // We look for a solution that has an ixscan we can turn into a distinctixscan
    for (size_t i = 0; i < solutions.size(); ++i) {
        if (turnIxscanIntoDistinctIxscan(solutions[i], parsedDistinct->getKey())) {
            // Great, we can use solutions[i].  Clean up the other QuerySolution(s).
            for (size_t j = 0; j < solutions.size(); ++j) {
                if (j != i) {
                    delete solutions[j];
                }
            }

            // Build and return the SSR over solutions[i].
            unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
            unique_ptr<QuerySolution> currentSolution(solutions[i]);
            PlanStage* rawRoot;
            verify(
                StageBuilder::build(opCtx, collection, *cq, *currentSolution, ws.get(), &rawRoot));
            unique_ptr<PlanStage> root(rawRoot);

            LOG(2) << "Using fast distinct: " << redact(cq->toStringShort())
                   << ", planSummary: " << redact(Explain::getPlanSummary(root.get()));

            return PlanExecutor::make(opCtx,
                                      std::move(ws),
                                      std::move(root),
                                      std::move(currentSolution),
                                      std::move(cq),
                                      collection,
                                      yieldPolicy);
        }
    }

    // If we're here, the planner made a soln with the restricted index set but we couldn't
    // translate any of them into a distinct-compatible soln.  So, delete the solutions and just
    // go through normal planning.
    for (size_t i = 0; i < solutions.size(); ++i) {
        delete solutions[i];
    }

    return getExecutor(opCtx, collection, parsedDistinct->releaseQuery(), yieldPolicy);
}

}  // namespace mongo
