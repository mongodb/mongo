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

#include "mongo/db/query/get_executor.h"

#include <boost/optional.hpp>
#include <limits>

#include "mongo/base/error_codes.h"
#include "mongo/base/parse_number.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/bucket_unpacker.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/count.h"
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/idhack.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/projection_executor_utils.h"
#include "mongo/db/exec/record_store_fast_count.h"
#include "mongo/db/exec/return_key.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/shard_filter.h"
#include "mongo/db/exec/sort_key_generator.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/exec/timeseries_modify.h"
#include "mongo/db/exec/unpack_timeseries_bucket.h"
#include "mongo/db/exec/upsert_stage.h"
#include "mongo/db/index/columns_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/query/bind_input_params.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/classic_plan_cache.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/cqf_command_utils.h"
#include "mongo/db/query/cqf_get_executor.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/query_settings_decoration.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/sbe_cached_solution_planner.h"
#include "mongo/db/query/sbe_multi_planner.h"
#include "mongo/db/query/sbe_sub_planner.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/query/yield_policy_callbacks_impl.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/logv2/log.h"
#include "mongo/s/shard_key_pattern_query_util.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/duration.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

boost::intrusive_ptr<ExpressionContext> makeExpressionContextForGetExecutor(
    OperationContext* opCtx, const BSONObj& requestCollation, const NamespaceString& nss) {
    invariant(opCtx);

    auto expCtx = make_intrusive<ExpressionContext>(opCtx, nullptr, nss);
    if (!requestCollation.isEmpty()) {
        auto statusWithCollator = CollatorFactoryInterface::get(expCtx->opCtx->getServiceContext())
                                      ->makeFromBSON(requestCollation);
        expCtx->setCollator(uassertStatusOK(std::move(statusWithCollator)));
    }
    return expCtx;
}

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
namespace wcp = ::mongo::wildcard_planning;
// The body is below in the "count hack" section but getExecutor calls it.
bool turnIxscanIntoCount(QuerySolution* soln);
}  // namespace

bool isAnyComponentOfPathMultikey(const BSONObj& indexKeyPattern,
                                  bool isMultikey,
                                  const MultikeyPaths& indexMultikeyInfo,
                                  StringData path) {
    if (!isMultikey) {
        return false;
    }

    size_t keyPatternFieldIndex = 0;
    bool found = false;
    if (indexMultikeyInfo.empty()) {
        // There is no path-level multikey information available, so we must assume 'path' is
        // multikey.
        return true;
    }

    for (auto&& elt : indexKeyPattern) {
        if (elt.fieldNameStringData() == path) {
            found = true;
            break;
        }
        keyPatternFieldIndex++;
    }
    invariant(found);

    invariant(indexMultikeyInfo.size() > keyPatternFieldIndex);
    return !indexMultikeyInfo[keyPatternFieldIndex].empty();
}

IndexEntry indexEntryFromIndexCatalogEntry(OperationContext* opCtx,
                                           const CollectionPtr& collection,
                                           const IndexCatalogEntry& ice,
                                           const CanonicalQuery* canonicalQuery) {
    auto desc = ice.descriptor();
    invariant(desc);

    if (desc->isIdIndex()) {
        // _id indexes are guaranteed to be non-multikey. Determining whether the index is multikey
        // has a small cost associated with it, so we skip that here to make _id lookups faster.
        return {desc->keyPattern(),
                desc->getIndexType(),
                desc->version(),
                false, /* isMultikey */
                {},    /* MultikeyPaths */
                {},    /* multikey Pathset */
                desc->isSparse(),
                desc->unique(),
                IndexEntry::Identifier{desc->indexName()},
                ice.getFilterExpression(),
                desc->infoObj(),
                ice.getCollator(),
                nullptr /* wildcard projection */};
    }

    auto accessMethod = ice.accessMethod();
    invariant(accessMethod);

    const bool isMultikey = ice.isMultikey(opCtx, collection);

    const WildcardProjection* wildcardProjection = nullptr;
    std::set<FieldRef> multikeyPathSet;
    if (desc->getIndexType() == IndexType::INDEX_WILDCARD) {
        auto wam = static_cast<const WildcardAccessMethod*>(accessMethod);
        wildcardProjection = wam->getWildcardProjection();
        if (isMultikey) {
            MultikeyMetadataAccessStats mkAccessStats;

            if (canonicalQuery) {
                RelevantFieldIndexMap fieldIndexProps;
                QueryPlannerIXSelect::getFields(canonicalQuery->root(), &fieldIndexProps);
                stdx::unordered_set<std::string> projectedFields;
                for (auto&& [fieldName, _] : fieldIndexProps) {
                    if (projection_executor_utils::applyProjectionToOneField(
                            wildcardProjection->exec(), fieldName)) {
                        projectedFields.insert(fieldName);
                    }
                }

                multikeyPathSet =
                    getWildcardMultikeyPathSet(wam, opCtx, projectedFields, &mkAccessStats);
            } else {
                multikeyPathSet = getWildcardMultikeyPathSet(wam, opCtx, &mkAccessStats);
            }

            LOGV2_DEBUG(20920,
                        2,
                        "Multikey path metadata range index scan stats",
                        "index"_attr = desc->indexName(),
                        "numSeeks"_attr = mkAccessStats.keysExamined,
                        "keysExamined"_attr = mkAccessStats.keysExamined);
        }
    }

    return {desc->keyPattern(),
            desc->getIndexType(),
            desc->version(),
            isMultikey,
            // The fixed-size vector of multikey paths stored in the index catalog.
            ice.getMultikeyPaths(opCtx, collection),
            // The set of multikey paths from special metadata keys stored in the index itself.
            // Indexes that have these metadata keys do not store a fixed-size vector of multikey
            // metadata in the index catalog. Depending on the index type, an index uses one of
            // these mechanisms (or neither), but not both.
            std::move(multikeyPathSet),
            desc->isSparse(),
            desc->unique(),
            IndexEntry::Identifier{desc->indexName()},
            ice.getFilterExpression(),
            desc->infoObj(),
            ice.getCollator(),
            wildcardProjection};
}

ColumnIndexEntry columnIndexEntryFromIndexCatalogEntry(OperationContext* opCtx,
                                                       const CollectionPtr& collection,
                                                       const IndexCatalogEntry& ice) {

    auto desc = ice.descriptor();
    invariant(desc);

    auto accessMethod = ice.accessMethod();
    invariant(accessMethod);

    auto cam = static_cast<const ColumnStoreAccessMethod*>(accessMethod);
    const auto columnstoreProjection = cam->getColumnstoreProjection();

    return {desc->keyPattern(),
            desc->getIndexType(),
            desc->version(),
            desc->isSparse(),
            desc->unique(),
            ColumnIndexEntry::Identifier{desc->indexName()},
            ice.getFilterExpression(),
            ice.getCollator(),
            columnstoreProjection};
}

/**
 * If query supports index filters, filter params.indices according to any index filters that have
 * been configured. In addition, sets that there were indeed index filters applied.
 */
void applyIndexFilters(const CollectionPtr& collection,
                       const CanonicalQuery& canonicalQuery,
                       QueryPlannerParams* plannerParams) {

    const QuerySettings* querySettings =
        QuerySettingsDecoration::get(collection->getSharedDecorations());
    const auto key = canonicalQuery.encodeKeyForPlanCacheCommand();

    // Filter index catalog if index filters are specified for query.
    // Also, signal to planner that application hint should be ignored.
    if (boost::optional<AllowedIndicesFilter> allowedIndicesFilter =
            querySettings->getAllowedIndicesFilter(key)) {
        filterAllowedIndexEntries(*allowedIndicesFilter, &plannerParams->indices);
        plannerParams->indexFiltersApplied = true;
    }
}

namespace {
void fillOutIndexEntries(OperationContext* opCtx,
                         bool apiStrict,
                         const CanonicalQuery* canonicalQuery,
                         const CollectionPtr& collection,
                         std::vector<IndexEntry>& entries,
                         std::vector<ColumnIndexEntry>& columnEntries) {
    std::vector<const IndexCatalogEntry*> columnIndexes, plainIndexes;
    auto ii = collection->getIndexCatalog()->getIndexIterator(
        opCtx, IndexCatalog::InclusionPolicy::kReady);
    while (ii->more()) {
        const IndexCatalogEntry* ice = ii->next();

        // Indexes excluded from API version 1 should _not_ be used for planning if apiStrict is
        // set to true.
        auto indexType = ice->descriptor()->getIndexType();
        if (apiStrict &&
            (indexType == IndexType::INDEX_HAYSTACK || indexType == IndexType::INDEX_TEXT ||
             indexType == IndexType::INDEX_COLUMN || ice->descriptor()->isSparse()))
            continue;

        // Skip the addition of hidden indexes to prevent use in query planning.
        if (ice->descriptor()->hidden())
            continue;

        if (indexType == IndexType::INDEX_COLUMN) {
            columnIndexes.push_back(ice);
        } else {
            plainIndexes.push_back(ice);
        }
    }
    columnEntries.reserve(columnIndexes.size());
    for (auto ice : columnIndexes) {
        columnEntries.emplace_back(columnIndexEntryFromIndexCatalogEntry(opCtx, collection, *ice));
    }
    entries.reserve(plainIndexes.size());
    for (auto ice : plainIndexes) {
        entries.emplace_back(
            indexEntryFromIndexCatalogEntry(opCtx, collection, *ice, canonicalQuery));
    }
}
}  // namespace

CollectionStats fillOutCollectionStats(OperationContext* opCtx, const CollectionPtr& collection) {
    auto recordStore = collection->getRecordStore();
    CollectionStats stats;
    stats.noOfRecords = recordStore->numRecords(opCtx),
    stats.approximateDataSizeBytes = recordStore->dataSize(opCtx),
    stats.storageSizeBytes = recordStore->storageSize(opCtx);
    return stats;
}

void fillOutPlannerParams(OperationContext* opCtx,
                          const CollectionPtr& collection,
                          const CanonicalQuery* canonicalQuery,
                          QueryPlannerParams* plannerParams) {
    invariant(canonicalQuery);
    bool apiStrict = APIParameters::get(opCtx).getAPIStrict().value_or(false);

    // _id queries can skip checking the catalog for indices since they will always use the _id
    // index.
    if (!isIdHackEligibleQuery(collection, *canonicalQuery)) {
        // If it's not NULL, we may have indices. Access the catalog and fill out IndexEntry(s)
        fillOutIndexEntries(opCtx,
                            apiStrict,
                            canonicalQuery,
                            collection,
                            plannerParams->indices,
                            plannerParams->columnStoreIndexes);

        // If query supports index filters, filter params.indices by indices in query settings.
        // Ignore index filters when it is possible to use the id-hack.
        applyIndexFilters(collection, *canonicalQuery, plannerParams);
    }

    // We will not output collection scans unless there are no indexed solutions. NO_TABLE_SCAN
    // overrides this behavior by not outputting a collscan even if there are no indexed
    // solutions.
    if (storageGlobalParams.noTableScan.load()) {
        const auto& nss = canonicalQuery->nss();
        // There are certain cases where we ignore this restriction:
        bool ignore =
            canonicalQuery->getQueryObj().isEmpty() || nss.isSystem() || nss.isOnInternalDb();
        if (!ignore) {
            plannerParams->options |= QueryPlannerParams::NO_TABLE_SCAN;
        }
    }

    // If the caller wants a shard filter, make sure we're actually sharded.
    if (plannerParams->options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
        if (collection.isSharded()) {
            const auto& shardKeyPattern = collection.getShardKeyPattern();

            // If the shard key is specified exactly, the query is guaranteed to only target one
            // shard. Shards cannot own orphans for the key ranges they own, so there is no need
            // to include a shard filtering stage. By omitting the shard filter, it may be possible
            // to get a more efficient plan (for example, a COUNT_SCAN may be used if the query is
            // eligible).
            const BSONObj extractedKey = extractShardKeyFromQuery(shardKeyPattern, *canonicalQuery);

            if (extractedKey.isEmpty()) {
                plannerParams->shardKey = shardKeyPattern.toBSON();
            } else {
                plannerParams->options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
            }
        } else {
            // If there's no metadata don't bother w/the shard filter since we won't know what
            // the key pattern is anyway...
            plannerParams->options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
        }
    }

    if (internalQueryPlannerEnableIndexIntersection.load()) {
        plannerParams->options |= QueryPlannerParams::INDEX_INTERSECTION;
    }

    if (internalQueryEnumerationPreferLockstepOrEnumeration.load()) {
        plannerParams->options |= QueryPlannerParams::ENUMERATE_OR_CHILDREN_LOCKSTEP;
    }

    if (internalQueryPlannerGenerateCoveredWholeIndexScans.load()) {
        plannerParams->options |= QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    }

    if (shouldWaitForOplogVisibility(
            opCtx, collection, canonicalQuery->getFindCommandRequest().getTailable())) {
        plannerParams->options |= QueryPlannerParams::OPLOG_SCAN_WAIT_FOR_VISIBLE;
    }

    if (collection->isClustered()) {
        plannerParams->clusteredInfo = collection->getClusteredInfo();
        plannerParams->clusteredCollectionCollator = collection->getDefaultCollator();
    }

    if (!plannerParams->columnStoreIndexes.empty()) {
        // Fill out statistics needed for column scan query planning.
        plannerParams->collectionStats = fillOutCollectionStats(opCtx, collection);

        const auto kMB = 1024 * 1024;
        plannerParams->availableMemoryBytes =
            static_cast<long long>(ProcessInfo::getMemSizeMB()) * kMB;
    }
}

std::map<NamespaceString, SecondaryCollectionInfo> fillOutSecondaryCollectionsInformation(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const CanonicalQuery* canonicalQuery) {
    std::map<NamespaceString, SecondaryCollectionInfo> infoMap;
    bool apiStrict = APIParameters::get(opCtx).getAPIStrict().value_or(false);
    auto fillOutSecondaryInfo = [&](const NamespaceString& nss,
                                    const CollectionPtr& secondaryColl) {
        auto secondaryInfo = SecondaryCollectionInfo();
        if (secondaryColl) {

            fillOutIndexEntries(opCtx,
                                apiStrict,
                                canonicalQuery,
                                secondaryColl,
                                secondaryInfo.indexes,
                                secondaryInfo.columnIndexes);
            secondaryInfo.stats = fillOutCollectionStats(opCtx, secondaryColl);
        } else {
            secondaryInfo.exists = false;
        }
        infoMap.emplace(nss, std::move(secondaryInfo));
    };
    for (auto& [collName, secondaryColl] : collections.getSecondaryCollections()) {
        fillOutSecondaryInfo(collName, secondaryColl);
    }

    // In the event of a self $lookup, we must have an entry for the main collection in the map
    // of secondary collections.
    if (collections.hasMainCollection()) {
        const auto& mainColl = collections.getMainCollection();
        fillOutSecondaryInfo(mainColl->ns(), mainColl);
    }
    return infoMap;
}

void fillOutPlannerParams(OperationContext* opCtx,
                          const MultipleCollectionAccessor& collections,
                          const CanonicalQuery* canonicalQuery,
                          QueryPlannerParams* plannerParams) {
    fillOutPlannerParams(opCtx, collections.getMainCollection(), canonicalQuery, plannerParams);
    plannerParams->secondaryCollectionsInfo =
        fillOutSecondaryCollectionsInformation(opCtx, collections, canonicalQuery);
}

bool shouldWaitForOplogVisibility(OperationContext* opCtx,
                                  const CollectionPtr& collection,
                                  bool tailable) {

    // Only non-tailable cursors on the oplog are affected. Only forward cursors, not reverse
    // cursors, are affected, but this is checked when the cursor is opened.
    if (!collection->ns().isOplog() || tailable) {
        return false;
    }

    // Only primaries should require readers to wait for oplog visibility. In any other replication
    // state, readers read at the most visible oplog timestamp. The reason why readers on primaries
    // need to wait is because multiple optimes can be allocated for operations before their entries
    // are written to the storage engine. "Holes" will appear when an operation with a later optime
    // commits before an operation with an earlier optime, and readers should wait so that all data
    // is consistent.
    //
    // Secondaries can't wait for oplog visibility without the PBWM lock because it can introduce a
    // hang while a batch application is in progress. The wait is done while holding a global lock,
    // and the oplog visibility timestamp is updated at the end of every batch on a secondary,
    // signalling the wait to complete. If a replication worker had a global lock and temporarily
    // released it, a reader could acquire the lock to read the oplog. If the secondary reader were
    // to wait for the oplog visibility timestamp to be updated, it would wait for a replication
    // batch that would never complete because it couldn't reacquire its own lock, the global lock
    // held by the waiting reader.
    return repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx, "admin");
}

namespace {
/**
 * Struct to hold information about a query plan's cache info.
 */
struct PlanCacheInfo {
    boost::optional<uint32_t> planCacheKey;
    boost::optional<uint32_t> queryHash;
};

/**
 * Fills in the given information on the CurOp::OpDebug object, if it has not already been filled in
 * by an outer pipeline.
 */
void setOpDebugPlanCacheInfo(OperationContext* opCtx, const PlanCacheInfo& cacheInfo) {
    OpDebug& opDebug = CurOp::get(opCtx)->debug();
    if (!opDebug.queryHash && cacheInfo.queryHash) {
        opDebug.queryHash = *cacheInfo.queryHash;
    }
    if (!opDebug.planCacheKey && cacheInfo.planCacheKey) {
        opDebug.planCacheKey = *cacheInfo.planCacheKey;
    }
}

/**
 * A class to hold the result of preparation of the query to be executed using classic engine. This
 * result stores and provides the following information:
 *     - A QuerySolutions for the query. May be null in certain circumstances, where the constructed
 *       execution tree does not have an associated query solution.
 *     - A root PlanStage of the constructed execution tree.
 */
class ClassicPrepareExecutionResult {
public:
    void emplace(std::unique_ptr<PlanStage> root, std::unique_ptr<QuerySolution> solution) {
        invariant(!_root);
        invariant(!_solution);
        _root = std::move(root);
        _solution = std::move(solution);
    }

    std::string getPlanSummary() const {
        invariant(_root);
        auto explainer = plan_explainer_factory::make(_root.get());
        return explainer->getPlanSummary();
    }

    std::tuple<std::unique_ptr<PlanStage>, std::unique_ptr<QuerySolution>> extractResultData() {
        return std::make_tuple(std::move(_root), std::move(_solution));
    }
    void setRecoveredFromPlanCache(bool val) {
        _fromPlanCache = val;
    }
    bool isRecoveredFromPlanCache() {
        return _fromPlanCache;
    }

    PlanCacheInfo& planCacheInfo() {
        return _cacheInfo;
    }

private:
    std::unique_ptr<PlanStage> _root;
    std::unique_ptr<QuerySolution> _solution;
    bool _fromPlanCache{false};
    PlanCacheInfo _cacheInfo;
};

/**
 * A class to hold the result of preparation of the query to be executed using SBE engine. This
 * result stores and provides the following information:
 *     - A vector of QuerySolutions. Elements of the vector may be null, in certain circumstances
 *       where the constructed execution tree does not have an associated query solution.
 *     - A vector of PlanStages, representing the roots of the constructed execution trees (in the
 *       case when the query has multiple solutions, we may construct an execution tree for each
 *       solution and pick the best plan after multi-planning). Elements of this vector can never be
 *       null. The size of this vector must always be empty or match the size of 'querySolutions'
 *       vector. It will be empty in circumstances where we only construct query solutions and delay
 *       building execution trees, which is any time we are not using a cached plan.
 *     - A root node of the extension plan. The plan can be combined with a solution to create a
 *       larger plan after the winning solution is found. Can be null, meaning "no extension".
 *     - An optional decisionWorks value, which is populated when a solution was reconstructed from
 *       the PlanCache, and will hold the number of work cycles taken to decide on a winning plan
 *       when the plan was first cached. It used to decided whether cached solution runtime planning
 *       needs to be done or not.
 *     - A 'needSubplanning' flag indicating that the query contains rooted $or predicate and is
 *       eligible for runtime sub-planning.
 */
class SlotBasedPrepareExecutionResult {
public:
    using QuerySolutionVector = std::vector<std::unique_ptr<QuerySolution>>;
    using PlanStageVector =
        std::vector<std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>>;

    void emplace(std::unique_ptr<QuerySolution> solution) {
        // Only allow solutions to be added, execution trees will be generated later.
        tassert(7087100,
                "expected execution trees to be generated after query solutions",
                _roots.empty());
        _solutions.push_back(std::move(solution));
    }

    void emplace(std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> root) {
        _roots.push_back(std::move(root));
        // Make sure we store an empty QuerySolution instead of a nullptr or nothing.
        _solutions.push_back(std::make_unique<QuerySolution>());
    }

    std::string getPlanSummary() const {
        // We can report plan summary only if this result contains a single solution.
        tassert(7087101, "expected exactly one solution", _solutions.size() == 1);
        tassert(7087102, "expected at most one execution tree", _roots.size() <= 1);
        // We only need the query solution to build the explain summary.
        auto explainer = plan_explainer_factory::make(
            nullptr /* root */, nullptr /* data */, _solutions[0].get());
        return explainer->getPlanSummary();
    }

    std::pair<PlanStageVector, QuerySolutionVector> extractResultData() {
        return std::make_pair(std::move(_roots), std::move(_solutions));
    }

    const QuerySolutionVector& solutions() const {
        return _solutions;
    }

    const PlanStageVector& roots() const {
        return _roots;
    }

    boost::optional<size_t> decisionWorks() const {
        return _decisionWorks;
    }

    bool needsSubplanning() const {
        return _needSubplanning;
    }

    void setNeedsSubplanning(bool needsSubplanning) {
        _needSubplanning = needsSubplanning;
    }

    void setDecisionWorks(boost::optional<size_t> decisionWorks) {
        _decisionWorks = decisionWorks;
    }

    bool recoveredPinnedCacheEntry() const {
        return _recoveredPinnedCacheEntry;
    }

    void setRecoveredPinnedCacheEntry(bool pinnedEntry) {
        _recoveredPinnedCacheEntry = pinnedEntry;
    }

    void setRecoveredFromPlanCache(bool val) {
        _fromPlanCache = val;
    }

    bool isRecoveredFromPlanCache() const {
        return _fromPlanCache;
    }

    PlanCacheInfo& planCacheInfo() {
        return _cacheInfo;
    }

private:
    QuerySolutionVector _solutions;
    PlanStageVector _roots;
    boost::optional<size_t> _decisionWorks;
    bool _needSubplanning{false};
    bool _recoveredPinnedCacheEntry{false};
    bool _fromPlanCache{false};
    PlanCacheInfo _cacheInfo;
};

/**
 * A helper class to build and prepare a PlanStage tree for execution. This class contains common
 * logic to build and prepare an execution tree for the provided canonical query, and also provides
 * methods to build various specialized PlanStage trees when we either:
 *    * Do not build a QuerySolutionNode tree for the input query, and as such do not undergo the
 *      normal stage builder process.
 *    * We have a QuerySolutionNode tree (or multiple query solution trees), but must execute some
 *      custom logic in order to build the final execution tree.
 *
 * In most cases, the helper bypasses the final step of building the execution tree and returns only
 * the query solution(s) if the 'DeferExecutionTreeGeneration' flag is true.
 */
template <typename KeyType,
          typename PlanStageType,
          typename ResultType,
          bool DeferExecutionTreeGeneration>
class PrepareExecutionHelper {
public:
    PrepareExecutionHelper(OperationContext* opCtx,
                           CanonicalQuery* cq,
                           PlanYieldPolicy* yieldPolicy,
                           const QueryPlannerParams& plannerOptions)
        : _opCtx{opCtx},
          _cq{cq},
          _yieldPolicy{yieldPolicy},
          _result{std::make_unique<ResultType>()} {
        invariant(_cq);
        _plannerParams = plannerOptions;
    }

    /**
     * Returns a reference to the main collection that is targeted by this query.
     */
    virtual const CollectionPtr& getMainCollection() const = 0;

    StatusWith<std::unique_ptr<ResultType>> prepare() {
        const auto& mainColl = getMainCollection();

        ON_BLOCK_EXIT([&] { CurOp::get(_opCtx)->stopQueryPlanningTimer(); });
        if (!mainColl) {
            LOGV2_DEBUG(20921,
                        2,
                        "Collection does not exist. Using EOF plan",
                        logAttrs(_cq->nss()),
                        "canonicalQuery"_attr = redact(_cq->toStringShort()));

            auto solution = std::make_unique<QuerySolution>();
            solution->setRoot(std::make_unique<EofNode>());
            auto result = releaseResult();
            addSolutionToResult(result.get(), std::move(solution));
            return std::move(result);
        }

        // Tailable: If the query requests tailable the collection must be capped.
        if (_cq->getFindCommandRequest().getTailable() && !mainColl->isCapped()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "error processing query: " << _cq->toString()
                                        << " tailable cursor requested on non capped collection");
        }

        // If the canonical query does not have a user-specified collation and no one has given the
        // CanonicalQuery a collation already, set it from the collection default.
        if (_cq->getFindCommandRequest().getCollation().isEmpty() &&
            _cq->getCollator() == nullptr && mainColl->getDefaultCollator()) {
            _cq->setCollator(mainColl->getDefaultCollator()->clone());
        }

        auto planCacheKey = buildPlanCacheKey();
        getResult()->planCacheInfo().queryHash = planCacheKey.queryHash();
        getResult()->planCacheInfo().planCacheKey = planCacheKey.planCacheKeyHash();

        if (auto result = buildCachedPlan(planCacheKey)) {
            return {std::move(result)};
        }

        initializePlannerParamsIfNeeded();
        if (SubplanStage::needsSubplanning(*_cq)) {
            LOGV2_DEBUG(20924,
                        2,
                        "Running query as sub-queries",
                        "query"_attr = redact(_cq->toStringShort()));
            return buildSubPlan();
        }

        auto statusWithMultiPlanSolns = QueryPlanner::plan(*_cq, _plannerParams);

        if (!statusWithMultiPlanSolns.isOK()) {
            return statusWithMultiPlanSolns.getStatus().withContext(
                str::stream() << "error processing query: " << _cq->toString()
                              << " planner returned error");
        }

        auto solutions = std::move(statusWithMultiPlanSolns.getValue());
        // The planner should have returned an error status if there are no solutions.
        invariant(solutions.size() > 0);

        // See if one of our solutions is a fast count hack in disguise.
        if (_cq->isCountLike()) {
            for (size_t i = 0; i < solutions.size(); ++i) {
                if (turnIxscanIntoCount(solutions[i].get())) {
                    auto result = releaseResult();
                    addSolutionToResult(result.get(), std::move(solutions[i]));

                    LOGV2_DEBUG(20925,
                                2,
                                "Using fast count",
                                "query"_attr = redact(_cq->toStringShort()),
                                "planSummary"_attr = result->getPlanSummary());
                    return std::move(result);
                }
            }
        }

        if (1 == solutions.size()) {
            // Only one possible plan. Build the stages from the solution.
            auto result = releaseResult();
            solutions[0]->indexFilterApplied = _plannerParams.indexFiltersApplied;
            addSolutionToResult(result.get(), std::move(solutions[0]));

            LOGV2_DEBUG(20926,
                        2,
                        "Only one plan is available",
                        "query"_attr = redact(_cq->toStringShort()),
                        "planSummary"_attr = result->getPlanSummary());
            return std::move(result);
        }
        return buildMultiPlan(std::move(solutions));
    }

protected:
    static constexpr bool ShouldDeferExecutionTreeGeneration = DeferExecutionTreeGeneration;

    /**
     * Get the result object to be returned by this in-progress prepare() call.
     */
    ResultType* getResult() {
        tassert(7061700, "expected _result to not be null", _result);
        return _result.get();
    }

    /**
     * Release the result instance to be returned to the caller holding the result of the
     * prepare() call.
     */
    auto releaseResult() {
        return std::move(_result);
    }

    /**
     * Adds the query solution to the result object, additionally building the corresponding
     * execution tree if 'DeferExecutionTreeGeneration' is turned on.
     */
    void addSolutionToResult(ResultType* result, std::unique_ptr<QuerySolution> solution) {
        if constexpr (!DeferExecutionTreeGeneration) {
            auto root = buildExecutableTree(*solution);
            result->emplace(std::move(root), std::move(solution));
        } else {
            result->emplace(std::move(solution));
        }
    }

    /**
     * Fills out planner parameters if not already filled.
     */
    void initializePlannerParamsIfNeeded() {
        if (_plannerParamsInitialized) {
            return;
        }
        fillOutPlannerParams(_opCtx, getMainCollection(), _cq, &_plannerParams);

        _plannerParamsInitialized = true;
    }

    /**
     * Constructs a PlanStage tree from the given query 'solution'.
     */
    virtual PlanStageType buildExecutableTree(const QuerySolution& solution) const = 0;

    /**
     * Constructs the plan cache key.
     */
    virtual KeyType buildPlanCacheKey() const = 0;

    /**
     * Either constructs a PlanStage tree from a cached plan (if exists in the plan cache), or
     * constructs a "id hack" PlanStage tree. Returns nullptr if no cached plan or id hack plan can
     * be constructed.
     */
    virtual std::unique_ptr<ResultType> buildCachedPlan(const KeyType& planCacheKey) = 0;

    /**
     * Constructs a special PlanStage tree for rooted $or queries. Each clause of the $or is planned
     * individually, and then an overall query plan is created based on the winning plan from each
     * clause.
     *
     * If sub-planning is implemented as a standalone component, rather than as part of the
     * execution tree, this method can populate the result object with additional information
     * required to perform the sub-planning.
     */
    virtual std::unique_ptr<ResultType> buildSubPlan() = 0;

    /**
     * If the query have multiple solutions, this method either:
     *    * Constructs a special PlanStage tree to perform a multi-planning task and pick the best
     *      plan in runtime.
     *    * Or builds a PlanStage tree for each of the 'solutions' and stores them in the result
     *      object, if multi-planning is implemented as a standalone component.
     */
    virtual std::unique_ptr<ResultType> buildMultiPlan(
        std::vector<std::unique_ptr<QuerySolution>> solutions) = 0;

    OperationContext* _opCtx;
    CanonicalQuery* _cq;
    PlanYieldPolicy* _yieldPolicy;
    QueryPlannerParams _plannerParams;
    // Used to avoid filling out the planner params twice.
    bool _plannerParamsInitialized = false;
    // In-progress result value of the prepare() call.
    std::unique_ptr<ResultType> _result;
};

/**
 * A helper class to prepare a classic PlanStage tree for execution.
 */
class ClassicPrepareExecutionHelper final
    : public PrepareExecutionHelper<PlanCacheKey,
                                    std::unique_ptr<PlanStage>,
                                    ClassicPrepareExecutionResult,
                                    false /* DeferExecutionTreeGeneration */> {
public:
    ClassicPrepareExecutionHelper(OperationContext* opCtx,
                                  const CollectionPtr& collection,
                                  WorkingSet* ws,
                                  CanonicalQuery* cq,
                                  PlanYieldPolicy* yieldPolicy,
                                  const QueryPlannerParams& plannerOptions)
        : PrepareExecutionHelper{opCtx, std::move(cq), yieldPolicy, plannerOptions},
          _collection(collection),
          _ws{ws} {}

    const CollectionPtr& getMainCollection() const override {
        return _collection;
    }

protected:
    std::unique_ptr<PlanStage> buildExecutableTree(const QuerySolution& solution) const final {
        return stage_builder::buildClassicExecutableTree(_opCtx, _collection, *_cq, solution, _ws);
    }

    std::unique_ptr<ClassicPrepareExecutionResult> buildIdHackPlan() {
        if (!isIdHackEligibleQuery(_collection, *_cq))
            return nullptr;

        const IndexDescriptor* descriptor = _collection->getIndexCatalog()->findIdIndex(_opCtx);
        if (!descriptor)
            return nullptr;

        LOGV2_DEBUG(20922,
                    2,
                    "Using classic engine idhack",
                    "canonicalQuery"_attr = redact(_cq->toStringShort()));

        auto result = releaseResult();
        std::unique_ptr<PlanStage> stage =
            std::make_unique<IDHackStage>(_cq->getExpCtxRaw(), _cq, _ws, _collection, descriptor);

        // Might have to filter out orphaned docs.
        if (_plannerParams.options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
            stage = std::make_unique<ShardFilterStage>(
                _cq->getExpCtxRaw(),
                CollectionShardingState::assertCollectionLockedAndAcquire(_opCtx, _cq->nss())
                    ->getOwnershipFilter(
                        _opCtx,
                        CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup),
                _ws,
                std::move(stage));
        }

        const auto* cqProjection = _cq->getProj();

        // Add a SortKeyGeneratorStage if the query requested sortKey metadata.
        if (_cq->metadataDeps()[DocumentMetadataFields::kSortKey]) {
            stage = std::make_unique<SortKeyGeneratorStage>(
                _cq->getExpCtxRaw(), std::move(stage), _ws, _cq->getFindCommandRequest().getSort());
        }

        if (_cq->getFindCommandRequest().getReturnKey()) {
            // If returnKey was requested, add ReturnKeyStage to return only the index keys in
            // the resulting documents. If a projection was also specified, it will be ignored,
            // with the exception the $meta sortKey projection, which can be used along with the
            // returnKey.
            stage = std::make_unique<ReturnKeyStage>(
                _cq->getExpCtxRaw(),
                cqProjection
                    ? QueryPlannerCommon::extractSortKeyMetaFieldsFromProjection(*cqProjection)
                    : std::vector<FieldPath>{},
                _ws,
                std::move(stage));
        } else if (cqProjection) {
            // There might be a projection. The idhack stage will always fetch the full
            // document, so we don't support covered projections. However, we might use the
            // simple inclusion fast path.
            // Stuff the right data into the params depending on what proj impl we use.
            if (!cqProjection->isSimple()) {
                stage = std::make_unique<ProjectionStageDefault>(
                    _cq->getExpCtxRaw(),
                    _cq->getFindCommandRequest().getProjection(),
                    _cq->getProj(),
                    _ws,
                    std::move(stage));
            } else {
                stage = std::make_unique<ProjectionStageSimple>(
                    _cq->getExpCtxRaw(),
                    _cq->getFindCommandRequest().getProjection(),
                    _cq->getProj(),
                    _ws,
                    std::move(stage));
            }
        }

        result->emplace(std::move(stage), nullptr /* solution */);
        return result;
    }

    PlanCacheKey buildPlanCacheKey() const {
        return plan_cache_key_factory::make<PlanCacheKey>(*_cq, _collection);
    }

    std::unique_ptr<ClassicPrepareExecutionResult> buildCachedPlan(
        const PlanCacheKey& planCacheKey) final {
        initializePlannerParamsIfNeeded();

        // Before consulting the plan cache, check if we should short-circuit and construct a
        // find-by-_id plan.
        std::unique_ptr<ClassicPrepareExecutionResult> result = buildIdHackPlan();

        if (result) {
            return result;
        }

        if (shouldCacheQuery(*_cq)) {
            // Try to look up a cached solution for the query.
            if (auto cs = CollectionQueryInfo::get(_collection)
                              .getPlanCache()
                              ->getCacheEntryIfActive(planCacheKey)) {
                // We have a CachedSolution.  Have the planner turn it into a QuerySolution.
                auto statusWithQs = QueryPlanner::planFromCache(*_cq, _plannerParams, *cs);

                if (statusWithQs.isOK()) {
                    auto querySolution = std::move(statusWithQs.getValue());
                    if (_cq->isCountLike() && turnIxscanIntoCount(querySolution.get())) {
                        LOGV2_DEBUG(5968201,
                                    2,
                                    "Using fast count",
                                    "query"_attr = redact(_cq->toStringShort()));
                    }

                    result = releaseResult();
                    auto&& root = buildExecutableTree(*querySolution);

                    // Add a CachedPlanStage on top of the previous root.
                    //
                    // 'decisionWorks' is used to determine whether the existing cache entry should
                    // be evicted, and the query replanned.
                    result->emplace(std::make_unique<CachedPlanStage>(_cq->getExpCtxRaw(),
                                                                      _collection,
                                                                      _ws,
                                                                      _cq,
                                                                      _plannerParams,
                                                                      cs->decisionWorks.value(),
                                                                      std::move(root)),
                                    std::move(querySolution));
                    return result;
                }
            }
        }

        return nullptr;
    }

    std::unique_ptr<ClassicPrepareExecutionResult> buildSubPlan() final {
        auto result = releaseResult();
        result->emplace(std::make_unique<SubplanStage>(
                            _cq->getExpCtxRaw(), _collection, _ws, _plannerParams, _cq),
                        nullptr /* solution */);
        return result;
    }

    std::unique_ptr<ClassicPrepareExecutionResult> buildMultiPlan(
        std::vector<std::unique_ptr<QuerySolution>> solutions) final {
        // Many solutions. Create a MultiPlanStage to pick the best, update the cache,
        // and so on. The working set will be shared by all candidate plans.
        auto multiPlanStage =
            std::make_unique<MultiPlanStage>(_cq->getExpCtxRaw(), _collection, _cq);

        for (size_t ix = 0; ix < solutions.size(); ++ix) {
            solutions[ix]->indexFilterApplied = _plannerParams.indexFiltersApplied;

            auto&& nextPlanRoot = buildExecutableTree(*solutions[ix]);

            // Takes ownership of 'nextPlanRoot'.
            multiPlanStage->addPlan(std::move(solutions[ix]), std::move(nextPlanRoot), _ws);
        }

        auto result = releaseResult();
        result->emplace(std::move(multiPlanStage), nullptr /* solution */);
        return result;
    }

private:
    const CollectionPtr& _collection;
    WorkingSet* _ws;
};

/**
 * A helper class to prepare an SBE PlanStage tree for execution.
 */
class SlotBasedPrepareExecutionHelper final
    : public PrepareExecutionHelper<
          sbe::PlanCacheKey,
          std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>,
          SlotBasedPrepareExecutionResult,
          true /* DeferExecutionTreeGeneration */> {
public:
    using PrepareExecutionHelper::PrepareExecutionHelper;

    SlotBasedPrepareExecutionHelper(OperationContext* opCtx,
                                    const MultipleCollectionAccessor& collections,
                                    CanonicalQuery* cq,
                                    PlanYieldPolicy* yieldPolicy,
                                    size_t plannerOptions)
        : PrepareExecutionHelper{opCtx,
                                 std::move(cq),
                                 yieldPolicy,
                                 QueryPlannerParams{plannerOptions}},
          _collections(collections) {}

    const CollectionPtr& getMainCollection() const override {
        return _collections.getMainCollection();
    }

    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> buildExecutableTree(
        const QuerySolution& solution) const final {
        tassert(7087105,
                "template indicates execution tree generation deferred",
                !ShouldDeferExecutionTreeGeneration);
        return stage_builder::buildSlotBasedExecutableTree(
            _opCtx, _collections, *_cq, solution, _yieldPolicy);
    }

protected:
    sbe::PlanCacheKey buildPlanCacheKey() const {
        return plan_cache_key_factory::make(*_cq, _collections);
    }

    std::unique_ptr<SlotBasedPrepareExecutionResult> buildCachedPlan(
        const sbe::PlanCacheKey& planCacheKey) final {
        if (shouldCacheQuery(*_cq)) {
            auto&& planCache = sbe::getPlanCache(_opCtx);
            auto cacheEntry = planCache.getCacheEntryIfActive(planCacheKey);
            if (!cacheEntry) {
                return nullptr;
            }

            auto&& cachedPlan = std::move(cacheEntry->cachedPlan);
            auto root = std::move(cachedPlan->root);
            auto stageData = std::move(cachedPlan->planStageData);
            stageData.debugInfo = cacheEntry->debugInfo;

            auto result = releaseResult();
            result->setDecisionWorks(cacheEntry->decisionWorks);
            result->setRecoveredPinnedCacheEntry(cacheEntry->isPinned());
            result->emplace(std::make_pair(std::move(root), std::move(stageData)));
            result->setRecoveredFromPlanCache(true);
            return result;
        }

        return nullptr;
    }

    std::unique_ptr<SlotBasedPrepareExecutionResult> buildSubPlan() final {
        // Nothing to be done here, all planning and stage building will be done by a SubPlanner.
        auto result = releaseResult();
        result->setNeedsSubplanning(true);
        return result;
    }

    std::unique_ptr<SlotBasedPrepareExecutionResult> buildMultiPlan(
        std::vector<std::unique_ptr<QuerySolution>> solutions) final {
        auto result = releaseResult();
        for (size_t ix = 0; ix < solutions.size(); ++ix) {
            solutions[ix]->indexFilterApplied = _plannerParams.indexFiltersApplied;

            addSolutionToResult(result.get(), std::move(solutions[ix]));
        }
        return result;
    }

private:
    const MultipleCollectionAccessor& _collections;
};

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getClassicExecutor(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const QueryPlannerParams& plannerParams) {
    auto ws = std::make_unique<WorkingSet>();
    ClassicPrepareExecutionHelper helper{
        opCtx, collection, ws.get(), canonicalQuery.get(), nullptr, plannerParams};
    auto executionResult = helper.prepare();
    if (!executionResult.isOK()) {
        return executionResult.getStatus();
    }
    auto&& result = executionResult.getValue();
    auto&& [root, solution] = result->extractResultData();
    invariant(root);

    setOpDebugPlanCacheInfo(opCtx, result->planCacheInfo());

    // We must have a tree of stages in order to have a valid plan executor, but the query
    // solution may be null.
    return plan_executor_factory::make(std::move(canonicalQuery),
                                       std::move(ws),
                                       std::move(root),
                                       &collection,
                                       yieldPolicy,
                                       plannerParams.options,
                                       {},
                                       std::move(solution));
}

/**
 * Checks if the prepared execution plans require further planning in runtime to pick the best
 * plan based on the collected execution stats, and returns a 'RuntimePlanner' instance if such
 * planning needs to be done, or nullptr otherwise.
 */
std::unique_ptr<sbe::RuntimePlanner> makeRuntimePlannerIfNeeded(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    CanonicalQuery* canonicalQuery,
    size_t numSolutions,
    boost::optional<size_t> decisionWorks,
    bool needsSubplanning,
    PlanYieldPolicySBE* yieldPolicy,
    size_t plannerOptions,
    boost::optional<const stage_builder::PlanStageData&> planStageData) {
    // If we have multiple solutions, we always need to do the runtime planning.
    if (numSolutions > 1) {
        invariant(!needsSubplanning && !decisionWorks);
        QueryPlannerParams plannerParams;
        plannerParams.options = plannerOptions;
        fillOutPlannerParams(opCtx, collections, canonicalQuery, &plannerParams);

        return std::make_unique<sbe::MultiPlanner>(opCtx,
                                                   collections,
                                                   *canonicalQuery,
                                                   plannerParams,
                                                   PlanCachingMode::AlwaysCache,
                                                   yieldPolicy);
    }

    // If the query can be run as sub-queries, the needSubplanning flag will be set to true and
    // we'll need to create a runtime planner to build a composite solution and pick the best plan
    // for each sub-query.
    if (needsSubplanning) {
        invariant(numSolutions == 0);

        QueryPlannerParams plannerParams;
        plannerParams.options = plannerOptions;
        fillOutPlannerParams(opCtx, collections, canonicalQuery, &plannerParams);

        return std::make_unique<sbe::SubPlanner>(
            opCtx, collections, *canonicalQuery, plannerParams, yieldPolicy);
    }

    invariant(numSolutions == 1);

    // If we have a single solution and the plan is not pinned or plan contains a hash_lookup stage,
    // we will need to do the runtime planning to check if the cached plan still
    // performs efficiently, or requires re-planning.
    tassert(6693503, "PlanStageData must be present", planStageData);
    const bool hasHashLookup = !planStageData->foreignHashJoinCollections.empty();
    if (decisionWorks || hasHashLookup) {
        QueryPlannerParams plannerParams;
        plannerParams.options = plannerOptions;
        return std::make_unique<sbe::CachedSolutionPlanner>(
            opCtx, collections, *canonicalQuery, plannerParams, decisionWorks, yieldPolicy);
    }

    // Runtime planning is not required.
    return nullptr;
}

std::unique_ptr<PlanYieldPolicySBE> makeSbeYieldPolicy(
    OperationContext* opCtx,
    PlanYieldPolicy::YieldPolicy requestedYieldPolicy,
    const Yieldable* yieldable,
    NamespaceString nss) {
    return std::make_unique<PlanYieldPolicySBE>(requestedYieldPolicy,
                                                opCtx->getServiceContext()->getFastClockSource(),
                                                internalQueryExecYieldIterations.load(),
                                                Milliseconds{internalQueryExecYieldPeriodMS.load()},
                                                yieldable,
                                                std::make_unique<YieldPolicyCallbacksImpl>(nss));
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getSlotBasedExecutor(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy,
    const QueryPlannerParams& plannerParams,
    std::unique_ptr<SlotBasedPrepareExecutionResult> planningResult) {
    // Now that we know what executor we are going to use, fill in some opDebug information, unless
    // it has already been filled by an outer pipeline.
    setOpDebugPlanCacheInfo(opCtx, planningResult->planCacheInfo());

    // Analyze the provided query and build the list of candidate plans for it.
    auto nss = cq->nss();

    auto&& [roots, solutions] = planningResult->extractResultData();

    invariant(roots.empty() || roots.size() == solutions.size());
    if (roots.empty()) {
        // We might have execution trees already if we pulled the plan from the cache. If not, we
        // need to generate one for each solution.
        for (const auto& solution : solutions) {
            roots.emplace_back(stage_builder::buildSlotBasedExecutableTree(
                opCtx, collections, *cq, *solution, yieldPolicy.get()));
        }
    }

    // When query requires sub-planning, we may not get any executable plans.
    const auto planStageData = roots.empty()
        ? boost::none
        : boost::optional<const stage_builder::PlanStageData&>(roots[0].second);

    // In some circumstances (e.g. when have multiple candidate plans or using a cached one), we
    // might need to execute the plan(s) to pick the best one or to confirm the choice.
    if (auto runTimePlanner = makeRuntimePlannerIfNeeded(opCtx,
                                                         collections,
                                                         cq.get(),
                                                         solutions.size(),
                                                         planningResult->decisionWorks(),
                                                         planningResult->needsSubplanning(),
                                                         yieldPolicy.get(),
                                                         plannerParams.options,
                                                         planStageData)) {
        // Do the runtime planning and pick the best candidate plan.
        auto candidates = runTimePlanner->plan(std::move(solutions), std::move(roots));

        return plan_executor_factory::make(opCtx,
                                           std::move(cq),
                                           std::move(candidates),
                                           collections,
                                           plannerParams.options,
                                           std::move(nss),
                                           std::move(yieldPolicy));
    }
    // No need for runtime planning, just use the constructed plan stage tree.
    invariant(solutions.size() == 1);
    invariant(roots.size() == 1);
    auto&& [root, data] = roots[0];

    if (!planningResult->recoveredPinnedCacheEntry()) {
        if (!cq->pipeline().empty()) {
            // Need to extend the solution with the agg pipeline and rebuild the execution tree.
            solutions[0] = QueryPlanner::extendWithAggPipeline(
                *cq,
                std::move(solutions[0]),
                fillOutSecondaryCollectionsInformation(opCtx, collections, cq.get()));
            roots[0] = stage_builder::buildSlotBasedExecutableTree(
                opCtx, collections, *cq, *(solutions[0]), yieldPolicy.get());
        }

        plan_cache_util::updatePlanCache(opCtx, collections, *cq, *solutions[0], *root, data);
    }

    // Prepare the SBE tree for execution.
    stage_builder::prepareSlotBasedExecutableTree(
        opCtx, root.get(), &data, *cq, collections, yieldPolicy.get(), true);

    return plan_executor_factory::make(opCtx,
                                       std::move(cq),
                                       std::move(solutions[0]),
                                       std::move(roots[0]),
                                       {},
                                       plannerParams.options,
                                       std::move(nss),
                                       std::move(yieldPolicy),
                                       planningResult->isRecoveredFromPlanCache(),
                                       false /* generatedByBonsai */);
}

/**
 * Checks if the result of query planning is SBE compatible. If any of the query solutions in
 * 'planningResult' cannot currently be compiled to an SBE plan via the SBE stage builders, then we
 * will fall back to the classic engine.
 */
bool shouldPlanningResultUseSbe(const SlotBasedPrepareExecutionResult& planningResult) {
    // If we have an entry in the SBE plan cache, then we can use SBE.
    if (planningResult.isRecoveredFromPlanCache()) {
        return true;
    }

    const auto& solutions = planningResult.solutions();
    if (solutions.empty()) {
        // Query needs subplanning (plans are generated later, we don't have access yet). We can
        // proceed with using SBE in this case.
        invariant(planningResult.needsSubplanning());
        return true;
    }

    // Check that all query solutions are SBE compatible.
    return std::all_of(solutions.begin(), solutions.end(), [](const auto& solution) {
        // We must have a solution, otherwise we would have early exited.
        invariant(solution->root());
        return isQueryPlanSbeCompatible(solution.get());
    });
}

/**
 * Function which returns true if 'cq' uses features that are currently supported in SBE without
 * 'featureFlagSbeFull' being set; false otherwise.
 */
bool shouldUseRegularSbe(const CanonicalQuery& cq) {
    auto sbeCompatLevel = cq.getExpCtx()->sbeCompatibility;
    // We shouldn't get here if there are expressions in the query which are completely unsupported
    // by SBE.
    tassert(7248600,
            "Unexpected SBE compatibility value",
            sbeCompatLevel != SbeCompatibility::notCompatible);
    // The 'ExpressionContext' may indicate that there are expressions which are only supported in
    // SBE when 'featureFlagSbeFull' is set, or fully supported regardless of the value of the
    // feature flag. This function should only return true in the latter case.
    return cq.getExpCtx()->sbeCompatibility == SbeCompatibility::fullyCompatible;
}

/**
 * Attempts to create a slot-based executor for the query, if the query plan is eligible for SBE
 * execution. This function has three possible return values:
 *
 *  1. A plan executor. This is in the case where the query is SBE eligible and encounters no errors
 * in executor creation. This result is to be expected in the majority of cases.
 *  2. A non-OK status. This is when errors are encountered during executor creation.
 *  3. The canonical query. This is to return ownership of the 'canonicalQuery' argument in the case
 * where the query plan is not eligible for SBE execution but it is not an error case.
 */
StatusWith<stdx::variant<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>,
                         std::unique_ptr<CanonicalQuery>>>
attemptToGetSlotBasedExecutor(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    std::function<void(CanonicalQuery*, bool)> extractAndAttachPipelineStages,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const QueryPlannerParams& plannerParams) {
    if (extractAndAttachPipelineStages) {
        // Push down applicable pipeline stages and attach to the query, but don't remove from
        // the high-level pipeline object until we know for sure we will execute with SBE.
        extractAndAttachPipelineStages(canonicalQuery.get(), true /* attachOnly */);
    }

    // (Ignore FCV check): This is intentional because we always want to use this feature once the
    // feature flag is enabled.
    const bool sbeFull = feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCVUnsafe();
    const bool canUseRegularSbe = shouldUseRegularSbe(*canonicalQuery);

    // If 'canUseRegularSbe' is true, then only the subset of SBE which is currently on by default
    // is used by the query. If 'sbeFull' is true, then the server is configured to run any
    // SBE-compatible query using SBE, even if the query uses features that are not on in SBE by
    // default. Either way, try to construct an SBE plan executor.
    if (canUseRegularSbe || sbeFull) {
        // Create the SBE prepare execution helper and initialize the params for the planner. If
        // planning results in any 'QuerySolution' which cannot be handled by the SBE stage builder,
        // then we will fall back to the classic engine.
        auto sbeYieldPolicy = makeSbeYieldPolicy(
            opCtx, yieldPolicy, &collections.getMainCollection(), canonicalQuery->nss());
        SlotBasedPrepareExecutionHelper helper{
            opCtx, collections, canonicalQuery.get(), sbeYieldPolicy.get(), plannerParams.options};
        auto planningResultWithStatus = helper.prepare();
        if (!planningResultWithStatus.isOK()) {
            return planningResultWithStatus.getStatus();
        }

        if (shouldPlanningResultUseSbe(*planningResultWithStatus.getValue())) {
            if (extractAndAttachPipelineStages) {
                // We know now that we will use SBE, so we need to remove the pushed-down stages
                // from the original pipeline object.
                extractAndAttachPipelineStages(canonicalQuery.get(), false /* attachOnly */);
            }
            auto statusWithExecutor =
                getSlotBasedExecutor(opCtx,
                                     collections,
                                     std::move(canonicalQuery),
                                     std::move(sbeYieldPolicy),
                                     plannerParams,
                                     std::move(planningResultWithStatus.getValue()));
            if (statusWithExecutor.isOK()) {
                return std::move(statusWithExecutor.getValue());
            } else {
                return statusWithExecutor.getStatus();
            }
        }
    }

    // Either we did not meet the criteria for attempting SBE, or we attempted query planning and
    // determined that SBE should not be used. Reset any fields that may have been modified, and
    // fall back to classic engine.
    canonicalQuery->setPipeline({});
    canonicalQuery->setSbeCompatible(false);
    return std::move(canonicalQuery);
}

}  // namespace

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutor(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    std::function<void(CanonicalQuery*, bool)> extractAndAttachPipelineStages,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const QueryPlannerParams& plannerParams) {
    auto exec = [&]() {
        invariant(canonicalQuery);
        const auto& mainColl = collections.getMainCollection();
        canonicalQuery->setSbeCompatible(isQuerySbeCompatible(&mainColl, canonicalQuery.get()));

        if (isEligibleForBonsai(*canonicalQuery, opCtx, mainColl)) {
            optimizer::QueryHints queryHints = getHintsFromQueryKnobs();
            const bool fastIndexNullHandling = queryHints._fastIndexNullHandling;
            auto maybeExec = getSBEExecutorViaCascadesOptimizer(
                mainColl, std::move(queryHints), canonicalQuery.get());
            if (maybeExec) {
                auto exec = uassertStatusOK(
                    makeExecFromParams(std::move(canonicalQuery), std::move(*maybeExec)));
                return StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>(
                    std::move(exec));
            } else {
                const auto queryControl =
                    ServerParameterSet::getNodeParameterSet()->get<QueryFrameworkControl>(
                        "internalQueryFrameworkControl");
                tassert(7319400,
                        "Optimization failed either without tryBonsai set, or without a hint.",
                        queryControl->_data.get() == QueryFrameworkControlEnum::kTryBonsai &&
                            !canonicalQuery->getFindCommandRequest().getHint().isEmpty() &&
                            !fastIndexNullHandling);
            }
        }

        // Use SBE if 'canonicalQuery' is SBE compatible.
        if (!canonicalQuery->getForceClassicEngine() && canonicalQuery->isSbeCompatible()) {
            auto statusWithExecutor =
                attemptToGetSlotBasedExecutor(opCtx,
                                              collections,
                                              std::move(canonicalQuery),
                                              std::move(extractAndAttachPipelineStages),
                                              yieldPolicy,
                                              plannerParams);
            if (!statusWithExecutor.isOK()) {
                return StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>(
                    statusWithExecutor.getStatus());
            }
            auto& maybeExecutor = statusWithExecutor.getValue();
            if (stdx::holds_alternative<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>(
                    maybeExecutor)) {
                return StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>(
                    std::move(stdx::get<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>(
                        maybeExecutor)));
            } else {
                // The query is not eligible for SBE execution - reclaim the canonical query and
                // fall back to classic.
                tassert(7087103,
                        "return value must contain canonical query if not executor",
                        stdx::holds_alternative<std::unique_ptr<CanonicalQuery>>(maybeExecutor));
                canonicalQuery =
                    std::move(stdx::get<std::unique_ptr<CanonicalQuery>>(maybeExecutor));
            }
        }
        // Ensure that 'sbeCompatible' is set accordingly.
        canonicalQuery->setSbeCompatible(false);
        return getClassicExecutor(
            opCtx, mainColl, std::move(canonicalQuery), yieldPolicy, plannerParams);
    }();
    if (exec.isOK()) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->debug().queryFramework = exec.getValue()->getQueryFramework();
    }
    return exec;
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutor(
    OperationContext* opCtx,
    const CollectionPtr* collection,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    std::function<void(CanonicalQuery*, bool)> extractAndAttachPipelineStages,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    size_t plannerOptions) {
    MultipleCollectionAccessor multi{collection};

    return getExecutor(opCtx,
                       multi,
                       std::move(canonicalQuery),
                       std::move(extractAndAttachPipelineStages),
                       yieldPolicy,
                       QueryPlannerParams{plannerOptions});
}

//
// Find
//

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorFind(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    std::function<void(CanonicalQuery*, bool)> extractAndAttachPipelineStages,
    bool permitYield,
    QueryPlannerParams plannerParams) {

    auto yieldPolicy = (permitYield && !opCtx->inMultiDocumentTransaction())
        ? PlanYieldPolicy::YieldPolicy::YIELD_AUTO
        : PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY;

    if (OperationShardingState::isComingFromRouter(opCtx)) {
        plannerParams.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    }

    return getExecutor(opCtx,
                       collections,
                       std::move(canonicalQuery),
                       std::move(extractAndAttachPipelineStages),
                       yieldPolicy,
                       plannerParams);
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorFind(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    std::function<void(CanonicalQuery*, bool)> extractAndAttachPipelineStages,
    bool permitYield,
    size_t plannerOptions) {

    MultipleCollectionAccessor multi{*coll};
    return getExecutorFind(opCtx,
                           multi,
                           std::move(canonicalQuery),
                           std::move(extractAndAttachPipelineStages),
                           permitYield,
                           QueryPlannerParams{plannerOptions});
}

namespace {

/**
 * Attempts to construct and return the projection AST corresponding to 'projObj'. Illegal to call
 * if 'projObj' is empty.
 *
 * If 'allowPositional' is false, and the projection AST involves positional projection, returns a
 * non-OK status.
 *
 * Marks any metadata dependencies required by the projection on the given CanonicalQuery.
 */
StatusWith<std::unique_ptr<projection_ast::Projection>> makeProjection(const BSONObj& projObj,
                                                                       bool allowPositional,
                                                                       CanonicalQuery* cq) {
    invariant(!projObj.isEmpty());

    projection_ast::Projection proj =
        projection_ast::parseAndAnalyze(cq->getExpCtx(),
                                        projObj.getOwned(),
                                        cq->root(),
                                        cq->getQueryObj(),
                                        ProjectionPolicies::findProjectionPolicies());

    // ProjectionExec requires the MatchDetails from the query expression when the projection
    // uses the positional operator. Since the query may no longer match the newly-updated
    // document, we forbid this case.
    if (!allowPositional && proj.requiresMatchDetails()) {
        return {ErrorCodes::BadValue,
                "cannot use a positional projection and return the new document"};
    }

    cq->requestAdditionalMetadata(proj.metadataDeps());

    // $meta sortKey is not allowed to be projected in findAndModify commands.
    if (cq->metadataDeps()[DocumentMetadataFields::kSortKey]) {
        return {ErrorCodes::BadValue,
                "Cannot use a $meta sortKey projection in findAndModify commands."};
    }

    return std::make_unique<projection_ast::Projection>(proj);
}

}  // namespace

//
// Delete
//

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorDelete(
    OpDebug* opDebug,
    const CollectionPtr* coll,
    ParsedDelete* parsedDelete,
    boost::optional<ExplainOptions::Verbosity> verbosity,
    DeleteStageParams::DocumentCounter&& documentCounter) {
    const auto& collection = *coll;
    auto expCtx = parsedDelete->expCtx();
    OperationContext* opCtx = expCtx->opCtx;
    const DeleteRequest* request = parsedDelete->getRequest();

    const NamespaceString& nss(request->getNsString());
    if (!request->getGod()) {
        if (nss.isSystem() && opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
            uassert(12050,
                    "cannot delete from system namespace",
                    nss.isLegalClientSystemNS(serverGlobalParams.featureCompatibility));
        }
    }

    if (collection && collection->isCapped()) {
        expCtx->setIsCappedDelete();
    }

    // If the parsed delete does not have a user-specified collation, set it from the collection
    // default.
    if (collection && parsedDelete->getRequest()->getCollation().isEmpty() &&
        collection->getDefaultCollator()) {
        parsedDelete->setCollator(collection->getDefaultCollator()->clone());
    }

    if (collection && collection->isCapped() && opCtx->inMultiDocumentTransaction()) {
        // This check is duplicated from collection_internal::deleteDocument() for two reasons:
        // - Performing a remove on an empty capped collection would not call
        //   collection_internal::deleteDocument().
        // - We can avoid doing lookups on documents and erroring later when trying to delete them.
        return Status(
            ErrorCodes::IllegalOperation,
            str::stream()
                << "Cannot remove from a capped collection in a multi-document transaction: "
                << nss.ns());
    }

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::PrimarySteppedDown,
                      str::stream() << "Not primary while removing from " << nss.ns());
    }

    auto deleteStageParams = std::make_unique<DeleteStageParams>();
    deleteStageParams->isMulti = request->getMulti();
    deleteStageParams->fromMigrate = request->getFromMigrate();
    deleteStageParams->isExplain = request->getIsExplain();
    deleteStageParams->returnDeleted = request->getReturnDeleted();
    deleteStageParams->sort = request->getSort();
    deleteStageParams->opDebug = opDebug;
    deleteStageParams->stmtId = request->getStmtId();
    deleteStageParams->numStatsForDoc = std::move(documentCounter);

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    const auto policy = parsedDelete->yieldPolicy();

    if (!collection) {
        // Treat collections that do not exist as empty collections. Return a PlanExecutor which
        // contains an EOF stage.
        LOGV2_DEBUG(20927,
                    2,
                    "Collection does not exist. Using EOF stage",
                    logAttrs(nss),
                    "query"_attr = redact(request->getQuery()));
        return plan_executor_factory::make(expCtx,
                                           std::move(ws),
                                           std::make_unique<EOFStage>(expCtx.get()),
                                           &CollectionPtr::null,
                                           policy,
                                           false, /* whether we must return owned data */
                                           nss);
    }

    if (!parsedDelete->hasParsedQuery()) {

        // Only consider using the idhack if no hint was provided.
        if (request->getHint().isEmpty()) {
            // This is the idhack fast-path for getting a PlanExecutor without doing the work to
            // create a CanonicalQuery.
            const BSONObj& unparsedQuery = request->getQuery();

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
                LOGV2_DEBUG(20928, 2, "Using idhack", "query"_attr = redact(unparsedQuery));

                auto idHackStage = std::make_unique<IDHackStage>(
                    expCtx.get(), unparsedQuery["_id"].wrap(), ws.get(), collection, descriptor);
                std::unique_ptr<DeleteStage> root =
                    std::make_unique<DeleteStage>(expCtx.get(),
                                                  std::move(deleteStageParams),
                                                  ws.get(),
                                                  collection,
                                                  idHackStage.release());
                return plan_executor_factory::make(expCtx,
                                                   std::move(ws),
                                                   std::move(root),
                                                   &collection,
                                                   policy,
                                                   false /* whether owned BSON must be returned */);
            }
        }

        // If we're here then we don't have a parsed query, but we're also not eligible for
        // the idhack fast path. We need to force canonicalization now.
        Status cqStatus = parsedDelete->parseQueryToCQ();
        if (!cqStatus.isOK()) {
            return cqStatus;
        }
    }

    // This is the regular path for when we have a CanonicalQuery.
    std::unique_ptr<CanonicalQuery> cq(parsedDelete->releaseParsedQuery());

    uassert(ErrorCodes::InternalErrorNotSupported,
            "delete command is not eligible for bonsai",
            !isEligibleForBonsai(*cq, opCtx, collection));

    // Transfer the explain verbosity level into the expression context.
    cq->getExpCtx()->explain = verbosity;

    std::unique_ptr<projection_ast::Projection> projection;
    if (!request->getProj().isEmpty()) {
        invariant(request->getReturnDeleted());

        const bool allowPositional = true;
        auto projectionWithStatus = makeProjection(request->getProj(), allowPositional, cq.get());
        if (!projectionWithStatus.isOK()) {
            return projectionWithStatus.getStatus();
        }
        projection = std::move(projectionWithStatus.getValue());
    }

    // The underlying query plan must preserve the record id, since it will be needed in order to
    // identify the record to update.
    cq->setForceGenerateRecordId(true);

    const size_t defaultPlannerOptions = QueryPlannerParams::DEFAULT;
    ClassicPrepareExecutionHelper helper{
        opCtx, collection, ws.get(), cq.get(), nullptr, defaultPlannerOptions};
    auto executionResult = helper.prepare();

    if (!executionResult.isOK()) {
        return executionResult.getStatus();
    }
    auto [root, querySolution] = executionResult.getValue()->extractResultData();
    invariant(root);

    deleteStageParams->canonicalQuery = cq.get();

    // TODO (SERVER-64506): support change streams' pre- and post-images.
    // TODO (SERVER-66079): allow batched deletions in the config.* namespace.
    const bool batchDelete =
        feature_flags::gBatchMultiDeletes.isEnabled(serverGlobalParams.featureCompatibility) &&
        gBatchUserMultiDeletes.load() &&
        (opCtx->recoveryUnit()->getState() == RecoveryUnit::State::kInactive ||
         opCtx->recoveryUnit()->getState() == RecoveryUnit::State::kActiveNotInUnitOfWork) &&
        !opCtx->inMultiDocumentTransaction() && !opCtx->isRetryableWrite() &&
        !collection->isChangeStreamPreAndPostImagesEnabled() && !collection->ns().isConfigDB() &&
        deleteStageParams->isMulti && !deleteStageParams->fromMigrate &&
        !deleteStageParams->returnDeleted && deleteStageParams->sort.isEmpty() &&
        !deleteStageParams->numStatsForDoc;

    auto expCtxRaw = cq->getExpCtxRaw();
    if (parsedDelete->isEligibleForArbitraryTimeseriesDelete()) {
        // Checks if the delete is on a time-series collection and cannot run on bucket documents
        // directly.
        root = std::make_unique<TimeseriesModifyStage>(
            expCtxRaw,
            std::move(deleteStageParams),
            ws.get(),
            std::move(root),
            collection,
            BucketUnpacker(*collection->getTimeseriesOptions()),
            parsedDelete->releaseResidualExpr());
    } else if (batchDelete) {
        root = std::make_unique<BatchedDeleteStage>(expCtxRaw,
                                                    std::move(deleteStageParams),
                                                    std::make_unique<BatchedDeleteStageParams>(),
                                                    ws.get(),
                                                    collection,
                                                    root.release());
    } else {
        root = std::make_unique<DeleteStage>(
            expCtxRaw, std::move(deleteStageParams), ws.get(), collection, root.release());
    }

    if (projection) {
        root = std::make_unique<ProjectionStageDefault>(
            cq->getExpCtx(), request->getProj(), projection.get(), ws.get(), std::move(root));
    }

    // We must have a tree of stages in order to have a valid plan executor, but the query
    // solution may be null.
    return plan_executor_factory::make(std::move(cq),
                                       std::move(ws),
                                       std::move(root),
                                       &collection,
                                       policy,
                                       defaultPlannerOptions,
                                       NamespaceString(),
                                       std::move(querySolution));
}

//
// Update
//

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorUpdate(
    OpDebug* opDebug,
    const CollectionPtr* coll,
    ParsedUpdate* parsedUpdate,
    boost::optional<ExplainOptions::Verbosity> verbosity,
    UpdateStageParams::DocumentCounter&& documentCounter) {
    const auto& collection = *coll;

    auto expCtx = parsedUpdate->expCtx();
    OperationContext* opCtx = expCtx->opCtx;

    const UpdateRequest* request = parsedUpdate->getRequest();
    UpdateDriver* driver = parsedUpdate->getDriver();

    const NamespaceString& nss = request->getNamespaceString();

    if (nss.isSystem() && opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
        uassert(10156,
                str::stream() << "cannot update a system namespace: " << nss.ns(),
                nss.isLegalClientSystemNS(serverGlobalParams.featureCompatibility));
    }

    // If there is no collection and this is an upsert, callers are supposed to create
    // the collection prior to calling this method. Explain, however, will never do
    // collection or database creation.
    if (!collection && request->isUpsert()) {
        invariant(request->explain());
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
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::PrimarySteppedDown,
                      str::stream() << "Not primary while performing update on " << nss.ns());
    }

    const auto policy = parsedUpdate->yieldPolicy();

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    UpdateStageParams updateStageParams(request, driver, opDebug, std::move(documentCounter));

    // If the collection doesn't exist, then return a PlanExecutor for a no-op EOF plan. We have
    // should have already enforced upstream that in this case either the upsert flag is false, or
    // we are an explain. If the collection doesn't exist, we're not an explain, and the upsert flag
    // is true, we expect the caller to have created the collection already.
    if (!collection) {
        LOGV2_DEBUG(20929,
                    2,
                    "Collection does not exist. Using EOF stage",
                    logAttrs(nss),
                    "query"_attr = redact(request->getQuery()));
        return plan_executor_factory::make(expCtx,
                                           std::move(ws),
                                           std::make_unique<EOFStage>(expCtx.get()),
                                           &CollectionPtr::null,
                                           policy,
                                           false, /* whether owned BSON must be returned */
                                           nss);
    }

    // Pass index information to the update driver, so that it can determine for us whether the
    // update affects indices.
    const auto& updateIndexData = CollectionQueryInfo::get(collection).getIndexKeys(opCtx);
    driver->refreshIndexKeys(&updateIndexData);

    if (!parsedUpdate->hasParsedQuery()) {

        // Only consider using the idhack if no hint was provided.
        if (request->getHint().isEmpty()) {
            // This is the idhack fast-path for getting a PlanExecutor without doing the work
            // to create a CanonicalQuery.
            const BSONObj& unparsedQuery = request->getQuery();

            const IndexDescriptor* descriptor = collection->getIndexCatalog()->findIdIndex(opCtx);

            const bool hasCollectionDefaultCollation = CollatorInterface::collatorsMatch(
                expCtx->getCollator(), collection->getDefaultCollator());

            if (descriptor && CanonicalQuery::isSimpleIdQuery(unparsedQuery) &&
                request->getProj().isEmpty() && hasCollectionDefaultCollation) {
                LOGV2_DEBUG(20930, 2, "Using idhack", "query"_attr = redact(unparsedQuery));

                // Working set 'ws' is discarded. InternalPlanner::updateWithIdHack() makes its own
                // WorkingSet.
                return InternalPlanner::updateWithIdHack(opCtx,
                                                         &collection,
                                                         updateStageParams,
                                                         descriptor,
                                                         unparsedQuery["_id"].wrap(),
                                                         policy);
            }
        }

        // If we're here then we don't have a parsed query, but we're also not eligible for
        // the idhack fast path. We need to force canonicalization now.
        Status cqStatus = parsedUpdate->parseQueryToCQ();
        if (!cqStatus.isOK()) {
            return cqStatus;
        }
    }

    // This is the regular path for when we have a CanonicalQuery.
    std::unique_ptr<CanonicalQuery> cq(parsedUpdate->releaseParsedQuery());

    uassert(ErrorCodes::InternalErrorNotSupported,
            "update command is not eligible for bonsai",
            !isEligibleForBonsai(*cq, opCtx, collection));

    std::unique_ptr<projection_ast::Projection> projection;
    if (!request->getProj().isEmpty()) {
        invariant(request->shouldReturnAnyDocs());

        // If the plan stage is to return the newly-updated version of the documents, then it
        // is invalid to use a positional projection because the query expression need not
        // match the array element after the update has been applied.
        const bool allowPositional = request->shouldReturnOldDocs();
        auto projectionWithStatus = makeProjection(request->getProj(), allowPositional, cq.get());
        if (!projectionWithStatus.isOK()) {
            return projectionWithStatus.getStatus();
        }
        projection = std::move(projectionWithStatus.getValue());
    }

    // The underlying query plan must preserve the record id, since it will be needed in order to
    // identify the record to update.
    cq->setForceGenerateRecordId(true);

    const size_t defaultPlannerOptions = QueryPlannerParams::DEFAULT;
    ClassicPrepareExecutionHelper helper{
        opCtx, collection, ws.get(), cq.get(), nullptr, defaultPlannerOptions};
    auto executionResult = helper.prepare();

    if (!executionResult.isOK()) {
        return executionResult.getStatus();
    }
    auto [root, querySolution] = executionResult.getValue()->extractResultData();
    invariant(root);

    updateStageParams.canonicalQuery = cq.get();
    const bool isUpsert = updateStageParams.request->isUpsert();
    root = (isUpsert
                ? std::make_unique<UpsertStage>(
                      cq->getExpCtxRaw(), updateStageParams, ws.get(), collection, root.release())
                : std::make_unique<UpdateStage>(
                      cq->getExpCtxRaw(), updateStageParams, ws.get(), collection, root.release()));

    if (projection) {
        root = std::make_unique<ProjectionStageDefault>(
            cq->getExpCtx(), request->getProj(), projection.get(), ws.get(), std::move(root));
    }

    // We must have a tree of stages in order to have a valid plan executor, but the query
    // solution may be null. Takes ownership of all args other than 'collection' and 'opCtx'
    return plan_executor_factory::make(std::move(cq),
                                       std::move(ws),
                                       std::move(root),
                                       &collection,
                                       policy,
                                       defaultPlannerOptions,
                                       NamespaceString(),
                                       std::move(querySolution));
}

//
// Count hack
//

namespace {

/**
 * If 'isn' represents a non-multikey index and its bounds contain a single null interval, return
 * its position. If 'isn' represents a multikey index and its bounds contain a single null and
 * empty array interval, return its position. Otherwise return boost::none.
 */
boost::optional<size_t> boundsHasExactlyOneNullOrNullAndEmptyInterval(const IndexScanNode* isn) {
    boost::optional<size_t> nullFieldNo;
    for (size_t fieldNo = 0; fieldNo < isn->bounds.fields.size(); ++fieldNo) {
        const OrderedIntervalList& oil = isn->bounds.fields[fieldNo];

        auto isNullInterval = IndexBoundsBuilder::isNullInterval(oil);
        auto isNullAndEmptyArrayInterval = IndexBoundsBuilder::isNullAndEmptyArrayInterval(oil);

        // Return boost::none if we have multiple null intervals.
        if ((isNullInterval || isNullAndEmptyArrayInterval) && nullFieldNo) {
            return boost::none;
        }

        if ((isNullInterval && !isn->index.multikey) ||
            (isNullAndEmptyArrayInterval && isn->index.multikey)) {
            nullFieldNo = fieldNo;
        }
    }
    return nullFieldNo;
}

/**
 * Returns 'true' if the provided solution 'soln' can be rewritten to use a fast counting stage.
 * Mutates the tree in 'soln->root'.
 *
 * Otherwise, returns 'false'.
 */
bool turnIxscanIntoCount(QuerySolution* soln) {
    QuerySolutionNode* root = soln->root();

    // Root should be an ixscan or fetch w/o any filters.
    if (!(STAGE_FETCH == root->getType() || STAGE_IXSCAN == root->getType())) {
        return false;
    }

    if (STAGE_FETCH == root->getType() && nullptr != root->filter.get()) {
        return false;
    }

    // If the root is a fetch, its child should be an ixscan
    if (STAGE_FETCH == root->getType() && STAGE_IXSCAN != root->children[0]->getType()) {
        return false;
    }

    IndexScanNode* isn = (STAGE_FETCH == root->getType())
        ? static_cast<IndexScanNode*>(root->children[0].get())
        : static_cast<IndexScanNode*>(root);

    // No filters allowed and side-stepping isSimpleRange for now.  TODO: do we ever see
    // isSimpleRange here?  because we could well use it.  I just don't think we ever do see
    // it.

    if (nullptr != isn->filter.get() || isn->bounds.isSimpleRange) {
        return false;
    }

    // Make sure the bounds are OK.
    BSONObj startKey;
    bool startKeyInclusive;
    BSONObj endKey;
    bool endKeyInclusive;

    auto makeCountScan = [&isn](BSONObj& csnStartKey,
                                bool startKeyInclusive,
                                BSONObj& csnEndKey,
                                bool endKeyInclusive) {
        // Since count scans return no data, they are always forward scans. Index scans, on the
        // other hand, may need to scan the index in reverse order in order to obtain a sort. If the
        // index scan direction is backwards, then we need to swap the start and end of the count
        // scan bounds.
        if (isn->direction < 0) {
            csnStartKey.swap(csnEndKey);
            std::swap(startKeyInclusive, endKeyInclusive);
        }

        auto csn = std::make_unique<CountScanNode>(isn->index);
        csn->startKey = csnStartKey;
        csn->startKeyInclusive = startKeyInclusive;
        csn->endKey = csnEndKey;
        csn->endKeyInclusive = endKeyInclusive;
        return csn;
    };

    if (!IndexBoundsBuilder::isSingleInterval(
            isn->bounds, &startKey, &startKeyInclusive, &endKey, &endKeyInclusive)) {
        // If we have exactly one null interval, we should split the bounds and try to construct
        // two COUNT_SCAN stages joined by an OR stage. If we have exactly one null and empty array
        // interval, we should do the same with three COUNT_SCAN stages. If we had multiple such
        // intervals, we would need at least 2^N count scans for N intervals, meaning this would
        // quickly explode to a point where it would just be more efficient to use a single index
        // scan. Consequently, we draw the line at one such interval.
        if (auto nullFieldNo = boundsHasExactlyOneNullOrNullAndEmptyInterval(isn)) {
            OrderedIntervalList undefinedPointOil, nullPointOil;
            undefinedPointOil.intervals.push_back(IndexBoundsBuilder::kUndefinedPointInterval);
            nullPointOil.intervals.push_back(IndexBoundsBuilder::kNullPointInterval);

            tassert(5506501,
                    "The index of the null interval is invalid",
                    *nullFieldNo < isn->bounds.fields.size());
            auto makeNullBoundsCountScan =
                [&](OrderedIntervalList& oil) -> std::unique_ptr<QuerySolutionNode> {
                std::swap(isn->bounds.fields[*nullFieldNo], oil);
                ON_BLOCK_EXIT([&] { std::swap(isn->bounds.fields[*nullFieldNo], oil); });

                BSONObj startKey, endKey;
                bool startKeyInclusive, endKeyInclusive;
                if (IndexBoundsBuilder::isSingleInterval(
                        isn->bounds, &startKey, &startKeyInclusive, &endKey, &endKeyInclusive)) {
                    return makeCountScan(startKey, startKeyInclusive, endKey, endKeyInclusive);
                }

                return nullptr;
            };

            auto undefinedCsn = makeNullBoundsCountScan(undefinedPointOil);

            if (undefinedCsn) {
                // If undefinedCsn is non-null, then we should also be able to successfully generate
                // a count scan for the null interval case and for the empty array interval case.
                auto nullCsn = makeNullBoundsCountScan(nullPointOil);
                tassert(5506500, "Invalid null bounds COUNT_SCAN", nullCsn);

                auto csns = makeVector(std::move(undefinedCsn), std::move(nullCsn));
                auto orn = std::make_unique<OrNode>();
                orn->addChildren(std::move(csns));

                if (isn->index.multikey) {
                    // For a multikey index, add the third COUNT_SCAN stage for empty array values.
                    OrderedIntervalList emptyArrayPointOil;
                    emptyArrayPointOil.intervals.push_back(
                        IndexBoundsBuilder::kEmptyArrayPointInterval);
                    auto emptyArrayCsn = makeNullBoundsCountScan(emptyArrayPointOil);
                    tassert(6001000, "Invalid empty array bounds COUNT_SCAN", emptyArrayCsn);

                    orn->addChildren(makeVector(std::move(emptyArrayCsn)));
                } else {
                    // Note that there is no need to deduplicate when the optimization is not
                    // applied to multikey indexes.
                    orn->dedup = false;
                }
                soln->setRoot(std::move(orn));

                return true;
            }
        }
        return false;
    }

    // Make the count node that we replace the fetch + ixscan with.
    auto csn = makeCountScan(startKey, startKeyInclusive, endKey, endKeyInclusive);
    // Takes ownership of 'cn' and deletes the old root.
    soln->setRoot(std::move(csn));
    return true;
}

/**
 * Returns true if indices contains an index that can be used with DistinctNode (the "fast distinct
 * hack" node, which can be used only if there is an empty query predicate).  Sets indexOut to the
 * array index of PlannerParams::indices.  Look for the index for the fewest fields.  Criteria for
 * suitable index is that the index should be of type BTREE or HASHED and the index cannot be a
 * partial index.
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
    int minFields = std::numeric_limits<int>::max();
    for (size_t i = 0; i < indices.size(); ++i) {
        // Skip indices with non-matching collator.
        if (!CollatorInterface::collatorsMatch(indices[i].collator, collator)) {
            continue;
        }
        // Skip partial indices.
        if (indices[i].filterExpr) {
            continue;
        }
        // Skip indices where the first key is not 'field'.
        auto firstIndexField = indices[i].keyPattern.firstElement();
        if (firstIndexField.fieldNameStringData() != StringData(field)) {
            continue;
        }
        // Skip the index if the first key is a "plugin" such as "hashed", "2dsphere", and so on.
        if (!firstIndexField.isNumber()) {
            continue;
        }
        // Compound hashed indexes can use distinct scan if the first field is 1 or -1. For the
        // other special indexes, the 1 or -1 index fields may be stored as a function of the data
        // rather than the raw data itself. Storing f(d) instead of 'd' precludes the distinct_scan
        // due to the possibility that f(d1) == f(d2).  Therefore, after fetching the base data,
        // either d1 or d2 would be incorrectly missing from the result set.
        auto indexPluginName = IndexNames::findPluginName(indices[i].keyPattern);
        switch (IndexNames::nameToType(indexPluginName)) {
            case IndexType::INDEX_BTREE:
            case IndexType::INDEX_HASHED:
                break;
            default:
                // All other index types are not eligible.
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

}  // namespace

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorCount(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const CollectionPtr* coll,
    const CountCommandRequest& request,
    bool explain,
    const NamespaceString& nss) {
    const auto& collection = *coll;

    OperationContext* opCtx = expCtx->opCtx;
    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    auto findCommand = std::make_unique<FindCommandRequest>(nss);

    findCommand->setFilter(request.getQuery());
    auto collation = request.getCollation().value_or(BSONObj());
    findCommand->setCollation(collation);
    findCommand->setHint(request.getHint());

    auto statusWithCQ = CanonicalQuery::canonicalize(
        opCtx,
        std::move(findCommand),
        explain,
        expCtx,
        collection ? static_cast<const ExtensionsCallback&>(
                         ExtensionsCallbackReal(opCtx, &collection->ns()))
                   : static_cast<const ExtensionsCallback&>(ExtensionsCallbackNoop()),
        MatchExpressionParser::kAllowAllSpecialFeatures,
        ProjectionPolicies::findProjectionPolicies(),
        {} /* empty pipeline */,
        true /* isCountLike */);

    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }
    std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    const auto yieldPolicy = opCtx->inMultiDocumentTransaction()
        ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
        : PlanYieldPolicy::YieldPolicy::YIELD_AUTO;

    const auto skip = request.getSkip().value_or(0);
    const auto limit = request.getLimit().value_or(0);

    uassert(ErrorCodes::InternalErrorNotSupported,
            "count command is not eligible for bonsai",
            !isEligibleForBonsai(*cq, opCtx, collection));

    if (!collection) {
        // Treat collections that do not exist as empty collections. Note that the explain reporting
        // machinery always assumes that the root stage for a count operation is a CountStage, so in
        // this case we put a CountStage on top of an EOFStage.
        std::unique_ptr<PlanStage> root = std::make_unique<CountStage>(
            expCtx.get(), collection, limit, skip, ws.get(), new EOFStage(expCtx.get()));
        return plan_executor_factory::make(expCtx,
                                           std::move(ws),
                                           std::move(root),
                                           &CollectionPtr::null,
                                           yieldPolicy,
                                           false, /* whether we must return owned BSON */
                                           nss);
    }

    // If the query is empty, then we can determine the count by just asking the collection
    // for its number of records. This is implemented by the CountStage, and we don't need
    // to create a child for the count stage in this case.
    //
    // If there is a hint, then we can't use a trival count plan as described above.
    const bool isEmptyQueryPredicate =
        cq->root()->matchType() == MatchExpression::AND && cq->root()->numChildren() == 0;
    const bool useRecordStoreCount = isEmptyQueryPredicate && request.getHint().isEmpty();

    if (useRecordStoreCount) {
        std::unique_ptr<PlanStage> root =
            std::make_unique<RecordStoreFastCountStage>(expCtx.get(), collection, skip, limit);
        return plan_executor_factory::make(expCtx,
                                           std::move(ws),
                                           std::move(root),
                                           &CollectionPtr::null,
                                           yieldPolicy,
                                           false, /* whether we must returned owned BSON */
                                           nss);
    }

    size_t plannerOptions = QueryPlannerParams::DEFAULT;

    if (OperationShardingState::isComingFromRouter(opCtx)) {
        plannerOptions |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    }

    ClassicPrepareExecutionHelper helper{
        opCtx, collection, ws.get(), cq.get(), nullptr, plannerOptions};
    auto executionResult = helper.prepare();
    if (!executionResult.isOK()) {
        return executionResult.getStatus();
    }

    auto [root, querySolution] = executionResult.getValue()->extractResultData();
    invariant(root);

    // Make a CountStage to be the new root.
    root = std::make_unique<CountStage>(
        expCtx.get(), collection, limit, skip, ws.get(), root.release());
    // We must have a tree of stages in order to have a valid plan executor, but the query
    // solution may be NULL. Takes ownership of all args other than 'collection' and 'opCtx'

    return plan_executor_factory::make(std::move(cq),
                                       std::move(ws),
                                       std::move(root),
                                       coll,
                                       yieldPolicy,
                                       plannerOptions,
                                       NamespaceString(),
                                       std::move(querySolution));
}

//
// Distinct hack
//

bool turnIxscanIntoDistinctIxscan(QuerySolution* soln,
                                  const std::string& field,
                                  bool strictDistinctOnly,
                                  bool flipDistinctScanDirection) {
    auto root = soln->root();

    // We can attempt to convert a plan if it follows one of these patterns (starting from the
    // root):
    //   1. PROJECT=>FETCH=>IXSCAN
    //   2. FETCH=>IXSCAN
    //   3. PROJECT=>IXSCAN
    QuerySolutionNode* projectNode = nullptr;
    IndexScanNode* indexScanNode = nullptr;
    FetchNode* fetchNode = nullptr;

    switch (root->getType()) {
        case STAGE_PROJECTION_DEFAULT:
        case STAGE_PROJECTION_COVERED:
        case STAGE_PROJECTION_SIMPLE:
            projectNode = root;
            break;
        case STAGE_FETCH:
            fetchNode = static_cast<FetchNode*>(root);
            break;
        default:
            return false;
    }

    if (!fetchNode && (STAGE_FETCH == root->children[0]->getType())) {
        fetchNode = static_cast<FetchNode*>(root->children[0].get());
    }

    if (fetchNode && (STAGE_IXSCAN == fetchNode->children[0]->getType())) {
        indexScanNode = static_cast<IndexScanNode*>(fetchNode->children[0].get());
    } else if (projectNode && (STAGE_IXSCAN == projectNode->children[0]->getType())) {
        indexScanNode = static_cast<IndexScanNode*>(projectNode->children[0].get());
    }

    if (!indexScanNode) {
        return false;
    }

    // If the fetch has a filter, we're out of luck. We can't skip all keys with a given value,
    // since one of them may key a document that passes the filter.
    if (fetchNode && fetchNode->filter) {
        return false;
    }

    if (indexScanNode->index.type == IndexType::INDEX_WILDCARD) {
        // If the query is on a field other than the distinct key, we may have generated a $** plan
        // which does not actually contain the distinct key field.
        if (field != std::next(indexScanNode->index.keyPattern.begin())->fieldName()) {
            return false;
        }
        // If the query includes object bounds, we cannot turn this IXSCAN into a DISTINCT_SCAN.
        // Wildcard indexes contain multiple keys per object, one for each subpath in ascending
        // (Path, Value, RecordId) order. If the distinct fields in two successive documents are
        // objects with the same leaf path values but in different field order, e.g. {a: 1, b: 2}
        // and {b: 2, a: 1}, we would therefore only return the first document and skip the other.
        if (wcp::isWildcardObjectSubpathScan(indexScanNode)) {
            return false;
        }
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

    if (strictDistinctOnly) {
        // If the "distinct" field is not the first field in the index bounds then the only way we
        // can guarantee that we'll never see duplicate values for the distinct field is to make
        // sure every field before the distinct field has equality bounds. For example, a
        // DISTINCT_SCAN on 'b' over the {a: 1, b: 1} index will scan a particular 'b' value
        // multiple times if that 'b' value exists in documents with different 'a' values. The
        // equality bounds on 'a' prevent the scan from seeing duplicate 'b' values by ensuring the
        // scan is limited to a single value for the 'a' field.
        for (size_t i = 0; i < static_cast<size_t>(fieldNo); ++i) {
            invariant(i < indexScanNode->bounds.size());
            if (indexScanNode->bounds.fields[i].intervals.size() != 1 ||
                !indexScanNode->bounds.fields[i].intervals[0].isPoint()) {
                return false;
            }
        }
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
    auto distinctNode = std::make_unique<DistinctNode>(indexScanNode->index);
    distinctNode->direction =
        flipDistinctScanDirection ? -indexScanNode->direction : indexScanNode->direction;
    distinctNode->bounds =
        flipDistinctScanDirection ? indexScanNode->bounds.reverse() : indexScanNode->bounds;
    distinctNode->queryCollator = indexScanNode->queryCollator;
    distinctNode->fieldNo = fieldNo;

    if (fetchNode) {
        // If the original plan had PROJECT and FETCH stages, we can get rid of the PROJECT
        // transforming the plan from PROJECT=>FETCH=>IXSCAN to FETCH=>DISTINCT_SCAN.
        if (projectNode) {
            invariant(projectNode == root);
            invariant(fetchNode == root->children[0].get());
            invariant(STAGE_FETCH == root->children[0]->getType());
            invariant(STAGE_IXSCAN == root->children[0]->children[0]->getType());
            // Make the fetch the new root. This destroys the project stage.
            soln->setRoot(std::move(root->children[0]));
        }

        // Attach the distinct node in the index scan's place.
        fetchNode->children[0] = std::move(distinctNode);
    } else {
        // There is no fetch node. The PROJECT=>IXSCAN tree should become PROJECT=>DISTINCT_SCAN.
        invariant(projectNode == root);
        invariant(STAGE_IXSCAN == root->children[0]->getType());

        // Attach the distinct node in the index scan's place.
        root->children[0] = std::move(distinctNode);
    }

    return true;
}

namespace {

// Get the list of indexes that include the "distinct" field.
QueryPlannerParams fillOutPlannerParamsForDistinct(OperationContext* opCtx,
                                                   const CollectionPtr& collection,
                                                   size_t plannerOptions,
                                                   const ParsedDistinct& parsedDistinct,
                                                   bool flipDistinctScanDirection) {
    QueryPlannerParams plannerParams;
    plannerParams.options = QueryPlannerParams::NO_TABLE_SCAN | plannerOptions;

    // If the caller did not request a "strict" distinct scan then we may choose a plan which
    // unwinds arrays and treats each element in an array as its own key.
    const bool mayUnwindArrays = !(plannerOptions & QueryPlannerParams::STRICT_DISTINCT_ONLY);
    auto ii = collection->getIndexCatalog()->getIndexIterator(
        opCtx, IndexCatalog::InclusionPolicy::kReady);
    auto query = parsedDistinct.getQuery()->getFindCommandRequest().getFilter();
    while (ii->more()) {
        const IndexCatalogEntry* ice = ii->next();
        const IndexDescriptor* desc = ice->descriptor();

        // Skip the addition of hidden indexes to prevent use in query planning.
        if (desc->hidden())
            continue;
        if (desc->keyPattern().hasField(parsedDistinct.getKey())) {
            // This handles regular fields of Compound Wildcard Indexes as well.
            if (flipDistinctScanDirection && ice->isMultikey(opCtx, collection)) {
                // This ParsedDistinct was generated as a result of transforming a $group with $last
                // accumulators using the GroupFromFirstTransformation. We cannot use a
                // DISTINCT_SCAN if $last is being applied to an indexed field which is multikey,
                // even if the 'parsedDistinct' key does not include multikey paths. This is because
                // changing the sort direction also changes the comparison semantics for arrays,
                // which means that flipping the scan may not exactly flip the order that we see
                // documents in. In the case of using DISTINCT_SCAN for $group, that would mean that
                // $first of the flipped scan may not be the same document as $last from the user's
                // requested sort order.
                continue;
            }
            if (!mayUnwindArrays &&
                isAnyComponentOfPathMultikey(desc->keyPattern(),
                                             ice->isMultikey(opCtx, collection),
                                             ice->getMultikeyPaths(opCtx, collection),
                                             parsedDistinct.getKey())) {
                // If the caller requested "strict" distinct that does not "pre-unwind" arrays,
                // then an index which is multikey on the distinct field may not be used. This is
                // because when indexing an array each element gets inserted individually. Any plan
                // which involves scanning the index will have effectively "unwound" all arrays.
                continue;
            }

            plannerParams.indices.push_back(indexEntryFromIndexCatalogEntry(
                opCtx, collection, *ice, parsedDistinct.getQuery()));
        } else if (desc->getIndexType() == IndexType::INDEX_WILDCARD && !query.isEmpty()) {
            // Check whether the $** projection captures the field over which we are distinct-ing.
            auto* proj = static_cast<const WildcardAccessMethod*>(ice->accessMethod())
                             ->getWildcardProjection()
                             ->exec();
            if (projection_executor_utils::applyProjectionToOneField(proj,
                                                                     parsedDistinct.getKey())) {
                plannerParams.indices.push_back(indexEntryFromIndexCatalogEntry(
                    opCtx, collection, *ice, parsedDistinct.getQuery()));
            }

            // It is not necessary to do any checks about 'mayUnwindArrays' in this case, because:
            // 1) If there is no predicate on the distinct(), a wildcard indices may not be used.
            // 2) distinct() _with_ a predicate may not be answered with a DISTINCT_SCAN on _any_
            // multikey index.

            // So, we will not distinct scan a wildcard index that's multikey on the distinct()
            // field, regardless of the value of 'mayUnwindArrays'.
        }
    }

    const CanonicalQuery* canonicalQuery = parsedDistinct.getQuery();
    const BSONObj& hint = canonicalQuery->getFindCommandRequest().getHint();

    applyIndexFilters(collection, *canonicalQuery, &plannerParams);

    // If there exists an index filter, we ignore all hints. Else, we only keep the index specified
    // by the hint. Since we cannot have an index with name $natural, that case will clear the
    // plannerParams.indices.
    if (!plannerParams.indexFiltersApplied && !hint.isEmpty()) {
        std::vector<IndexEntry> temp =
            QueryPlannerIXSelect::findIndexesByHint(hint, plannerParams.indices);
        temp.swap(plannerParams.indices);
    }

    return plannerParams;
}

/**
 * A simple DISTINCT_SCAN has an empty query and no sort, so we just need to find a suitable index
 * that has the "distinct" field as the first component of its key pattern.
 *
 * If a suitable solution is found, this function will create and return a new executor. In order to
 * do so, it releases the CanonicalQuery from the 'parsedDistinct' input. If no solution is found,
 * the return value is StatusOK with a nullptr value, and the 'parsedDistinct' CanonicalQuery
 * remains valid. This function may also return a failed status code, in which case the caller
 * should assume that the 'parsedDistinct' CanonicalQuery is no longer valid.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorForSimpleDistinct(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    const QueryPlannerParams& plannerParams,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    ParsedDistinct* parsedDistinct) {
    const auto& collection = *coll;

    invariant(parsedDistinct->getQuery());
    auto collator = parsedDistinct->getQuery()->getCollator();

    // If there's no query, we can just distinct-scan one of the indices. Not every index in
    // plannerParams.indices may be suitable. Refer to getDistinctNodeIndex().
    size_t distinctNodeIndex = 0;
    if (!parsedDistinct->getQuery()->getFindCommandRequest().getFilter().isEmpty() ||
        parsedDistinct->getQuery()->getSortPattern() ||
        !getDistinctNodeIndex(
            plannerParams.indices, parsedDistinct->getKey(), collator, &distinctNodeIndex)) {
        // Not a "simple" DISTINCT_SCAN or no suitable index was found.
        return {nullptr};
    }

    auto dn = std::make_unique<DistinctNode>(plannerParams.indices[distinctNodeIndex]);
    dn->direction = 1;
    IndexBoundsBuilder::allValuesBounds(
        dn->index.keyPattern, &dn->bounds, dn->index.collator != nullptr);
    dn->queryCollator = collator;
    dn->fieldNo = 0;

    // An index with a non-simple collation requires a FETCH stage.
    std::unique_ptr<QuerySolutionNode> solnRoot = std::move(dn);
    if (plannerParams.indices[distinctNodeIndex].collator) {
        if (!solnRoot->fetched()) {
            auto fetch = std::make_unique<FetchNode>();
            fetch->children.push_back(std::move(solnRoot));
            solnRoot = std::move(fetch);
        }
    }

    QueryPlannerParams params;

    auto soln = QueryPlannerAnalysis::analyzeDataAccess(
        *parsedDistinct->getQuery(), params, std::move(solnRoot));
    invariant(soln);

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    auto&& root = stage_builder::buildClassicExecutableTree(
        opCtx, collection, *parsedDistinct->getQuery(), *soln, ws.get());

    auto exec = plan_executor_factory::make(parsedDistinct->releaseQuery(),
                                            std::move(ws),
                                            std::move(root),
                                            coll,
                                            yieldPolicy,
                                            plannerParams.options,
                                            NamespaceString(),
                                            std::move(soln));
    if (exec.isOK()) {
        LOGV2_DEBUG(20931,
                    2,
                    "Using fast distinct",
                    "query"_attr = redact(exec.getValue()->getCanonicalQuery()->toStringShort()),
                    "planSummary"_attr = exec.getValue()->getPlanExplainer().getPlanSummary());
    }

    return exec;
}

// Checks each solution in the 'solutions' std::vector to see if one includes an IXSCAN that can be
// rewritten as a DISTINCT_SCAN, assuming we want distinct scan behavior on the getKey() property of
// the 'parsedDistinct' argument.
//
// If a suitable solution is found, this function will create and return a new executor. In order to
// do so, it releases the CanonicalQuery from the 'parsedDistinct' input. If no solution is found,
// the return value is StatusOK with a nullptr value, and the 'parsedDistinct' CanonicalQuery
// remains valid. This function may also return a failed status code, in which case the caller
// should assume that the 'parsedDistinct' CanonicalQuery is no longer valid.
//
// See the declaration of turnIxscanIntoDistinctIxscan() for an explanation of the
// 'strictDistinctOnly' parameter.
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
getExecutorDistinctFromIndexSolutions(OperationContext* opCtx,
                                      const CollectionPtr* coll,
                                      std::vector<std::unique_ptr<QuerySolution>> solutions,
                                      PlanYieldPolicy::YieldPolicy yieldPolicy,
                                      ParsedDistinct* parsedDistinct,
                                      bool flipDistinctScanDirection,
                                      size_t plannerOptions) {
    const auto& collection = *coll;
    const bool strictDistinctOnly = (plannerOptions & QueryPlannerParams::STRICT_DISTINCT_ONLY);

    // We look for a solution that has an ixscan we can turn into a distinctixscan
    for (size_t i = 0; i < solutions.size(); ++i) {
        if (turnIxscanIntoDistinctIxscan(solutions[i].get(),
                                         parsedDistinct->getKey(),
                                         strictDistinctOnly,
                                         flipDistinctScanDirection)) {
            // Build and return the SSR over solutions[i].
            std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
            std::unique_ptr<QuerySolution> currentSolution = std::move(solutions[i]);
            auto&& root = stage_builder::buildClassicExecutableTree(
                opCtx, collection, *parsedDistinct->getQuery(), *currentSolution, ws.get());

            auto exec = plan_executor_factory::make(parsedDistinct->releaseQuery(),
                                                    std::move(ws),
                                                    std::move(root),
                                                    coll,
                                                    yieldPolicy,
                                                    plannerOptions,
                                                    NamespaceString(),
                                                    std::move(currentSolution));
            if (exec.isOK()) {
                LOGV2_DEBUG(
                    20932,
                    2,
                    "Using fast distinct",
                    "query"_attr = redact(exec.getValue()->getCanonicalQuery()->toStringShort()),
                    "planSummary"_attr = exec.getValue()->getPlanExplainer().getPlanSummary());
            }

            return exec;
        }
    }

    // Indicate that, although there was no error, we did not find a DISTINCT_SCAN solution.
    return {nullptr};
}

/**
 * Makes a clone of 'cq' but without any projection, then runs getExecutor on the clone.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorWithoutProjection(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    const CanonicalQuery* cq,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    size_t plannerOptions) {
    const auto& collection = *coll;

    auto findCommand = std::make_unique<FindCommandRequest>(cq->getFindCommandRequest());
    findCommand->setProjection(BSONObj());

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    const ExtensionsCallbackReal extensionsCallback(opCtx, &collection->ns());

    auto cqWithoutProjection = uassertStatusOKWithContext(
        CanonicalQuery::canonicalize(opCtx,
                                     std::move(findCommand),
                                     cq->getExplain(),
                                     expCtx,
                                     extensionsCallback,
                                     MatchExpressionParser::kAllowAllSpecialFeatures),
        "Unable to canonicalize query");

    return getExecutor(opCtx,
                       coll,
                       std::move(cqWithoutProjection),
                       nullptr /* extractAndAttachPipelineStages */,
                       yieldPolicy,
                       plannerOptions);
}
}  // namespace

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorDistinct(
    const CollectionPtr* coll,
    size_t plannerOptions,
    ParsedDistinct* parsedDistinct,
    bool flipDistinctScanDirection) {
    const auto& collection = *coll;

    auto expCtx = parsedDistinct->getQuery()->getExpCtx();
    OperationContext* opCtx = expCtx->opCtx;
    const auto yieldPolicy = opCtx->inMultiDocumentTransaction()
        ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
        : PlanYieldPolicy::YieldPolicy::YIELD_AUTO;

    // Assert that not eligible for bonsai
    uassert(ErrorCodes::InternalErrorNotSupported,
            "distinct command is not eligible for bonsai",
            !isEligibleForBonsai(*parsedDistinct->getQuery(), opCtx, collection));

    if (!collection) {
        // Treat collections that do not exist as empty collections.
        return plan_executor_factory::make(parsedDistinct->releaseQuery(),
                                           std::make_unique<WorkingSet>(),
                                           std::make_unique<EOFStage>(expCtx.get()),
                                           coll,
                                           yieldPolicy,
                                           plannerOptions);
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

    auto plannerParams = fillOutPlannerParamsForDistinct(
        opCtx, collection, plannerOptions, *parsedDistinct, flipDistinctScanDirection);

    // If there are no suitable indices for the distinct hack bail out now into regular planning
    // with no projection.
    if (plannerParams.indices.empty()) {
        if (plannerOptions & QueryPlannerParams::STRICT_DISTINCT_ONLY) {
            // STRICT_DISTINCT_ONLY indicates that we should not return any plan if we can't return
            // a DISTINCT_SCAN plan.
            return {nullptr};
        } else {
            // Note that, when not in STRICT_DISTINCT_ONLY mode, the caller doesn't care about the
            // projection, only that the planner does not produce a FETCH if it's possible to cover
            // the fields in the projection. That's definitely not possible in this case, so we
            // dispense with the projection.
            return getExecutorWithoutProjection(
                opCtx, coll, parsedDistinct->getQuery(), yieldPolicy, plannerOptions);
        }
    }

    //
    // If we're here, we have an index that includes the field we're distinct-ing over.
    //

    auto executorWithStatus =
        getExecutorForSimpleDistinct(opCtx, coll, plannerParams, yieldPolicy, parsedDistinct);
    if (!executorWithStatus.isOK() || executorWithStatus.getValue()) {
        // We either got a DISTINCT plan or a fatal error.
        return executorWithStatus;
    } else {
        // A "simple" DISTINCT plan wasn't possible, but we can try again with the QueryPlanner.
    }

    // Ask the QueryPlanner for a list of solutions that scan one of the indexes from
    // fillOutPlannerParamsForDistinct() (i.e., the indexes that include the distinct field).
    auto statusWithMultiPlanSolns = QueryPlanner::plan(*parsedDistinct->getQuery(), plannerParams);
    if (!statusWithMultiPlanSolns.isOK()) {
        if (plannerOptions & QueryPlannerParams::STRICT_DISTINCT_ONLY) {
            return {nullptr};
        } else {
            return getExecutor(opCtx,
                               coll,
                               parsedDistinct->releaseQuery(),
                               nullptr /* extractAndAttachPipelineStages */,
                               yieldPolicy,
                               plannerOptions);
        }
    }
    auto solutions = std::move(statusWithMultiPlanSolns.getValue());

    // See if any of the solutions can be rewritten using a DISTINCT_SCAN. Note that, if the
    // STRICT_DISTINCT_ONLY flag is not set, we may get a DISTINCT_SCAN plan that filters out some
    // but not all duplicate values of the distinct field, meaning that the output from this
    // executor will still need deduplication.
    executorWithStatus = getExecutorDistinctFromIndexSolutions(opCtx,
                                                               coll,
                                                               std::move(solutions),
                                                               yieldPolicy,
                                                               parsedDistinct,
                                                               flipDistinctScanDirection,
                                                               plannerOptions);
    if (!executorWithStatus.isOK() || executorWithStatus.getValue()) {
        // We either got a DISTINCT plan or a fatal error.
        return executorWithStatus;
    } else if (!(plannerOptions & QueryPlannerParams::STRICT_DISTINCT_ONLY)) {
        // We did not find a solution that we could convert to a DISTINCT_SCAN, so we fall back to
        // regular planning. Note that, when not in STRICT_DISTINCT_ONLY mode, the caller doesn't
        // care about the projection, only that the planner does not produce a FETCH if it's
        // possible to cover the fields in the projection. That's definitely not possible in this
        // case, so we dispense with the projection.
        return getExecutorWithoutProjection(
            opCtx, coll, parsedDistinct->getQuery(), yieldPolicy, plannerOptions);
    } else {
        // We did not find a solution that we could convert to DISTINCT_SCAN, and the
        // STRICT_DISTINCT_ONLY prohibits us from using any other kind of plan, so we return
        // nullptr.
        return {nullptr};
    }
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getCollectionScanExecutor(
    OperationContext* opCtx,
    const CollectionPtr& yieldableCollection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    CollectionScanDirection scanDirection,
    const boost::optional<RecordId>& resumeAfterRecordId) {
    auto isForward = scanDirection == CollectionScanDirection::kForward;
    auto direction = isForward ? InternalPlanner::FORWARD : InternalPlanner::BACKWARD;
    return InternalPlanner::collectionScan(
        opCtx, &yieldableCollection, yieldPolicy, direction, resumeAfterRecordId);
}

}  // namespace mongo
