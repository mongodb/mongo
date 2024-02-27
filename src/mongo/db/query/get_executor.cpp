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

#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>
#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <cstdint>
#include <iterator>
#include <limits>
#include <mutex>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/batched_delete_stage.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/count.h"
#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/idhack.h"
#include "mongo/db/exec/index_path_projection.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/record_store_fast_count.h"
#include "mongo/db/exec/return_key.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/shard_filter.h"
#include "mongo/db/exec/sort_key_generator.h"
#include "mongo/db/exec/spool.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/db/exec/timeseries_modify.h"
#include "mongo/db/exec/timeseries_upsert.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/exec/upsert_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/classic_plan_cache.h"
#include "mongo/db/query/classic_runtime_planner_for_sbe/planner_interface.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/cqf_command_utils.h"
#include "mongo/db/query/cqf_fast_paths.h"
#include "mongo/db/query/cqf_get_executor.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/index_hint.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/interval.h"
#include "mongo/db/query/interval_evaluation_tree.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/explain_interface.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/plan_executor_express.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/db/query/projection.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/projection_policies.h"
#include "mongo/db/query/query_decorations.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_settings_decoration.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/sbe_cached_solution_planner.h"
#include "mongo/db/query/sbe_multi_planner.h"
#include "mongo/db/query/sbe_plan_cache.h"
#include "mongo/db/query/sbe_runtime_planner.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_sub_planner.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/query/yield_policy_callbacks_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/db/yieldable.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_key_pattern_query_util.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/synchronized_value.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

boost::intrusive_ptr<ExpressionContext> makeExpressionContextForGetExecutor(
    OperationContext* opCtx,
    const BSONObj& requestCollation,
    const NamespaceString& nss,
    boost::optional<ExplainOptions::Verbosity> verbosity) {
    invariant(opCtx);

    auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                    nullptr /* collator */,
                                                    nss,
                                                    boost::none /* runtimeConstants */,
                                                    boost::none /* letParameters */,
                                                    false /* allowDiskUse */,
                                                    true /* mayDbProfile */,
                                                    verbosity);
    if (!requestCollation.isEmpty()) {
        auto statusWithCollator = CollatorFactoryInterface::get(expCtx->opCtx->getServiceContext())
                                      ->makeFromBSON(requestCollation);
        expCtx->setCollator(uassertStatusOK(std::move(statusWithCollator)));
    }
    return expCtx;
}

namespace {
namespace wcp = ::mongo::wildcard_planning;
// The body is below in the "count hack" section but getExecutor calls it.
bool turnIxscanIntoCount(QuerySolution* soln);
}  // namespace

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

    std::tuple<std::unique_ptr<PlanStage>, std::unique_ptr<QuerySolution>> extractResultData() {
        return std::make_tuple(std::move(_root), std::move(_solution));
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

    void setCachedPlanHash(boost::optional<size_t> cachedPlanHash) {
        _cachedPlanHash = cachedPlanHash;
    }

    boost::optional<size_t> cachedPlanHash() const {
        return _cachedPlanHash;
    }

private:
    std::unique_ptr<PlanStage> _root;
    std::unique_ptr<QuerySolution> _solution;
    bool _fromPlanCache{false};
    PlanCacheInfo _cacheInfo;
    // If there is a matching cache entry, this is the hash of the cached plan.
    boost::optional<size_t> _cachedPlanHash;
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

    void setCachedPlanHash(boost::optional<size_t> cachedPlanHash) {
        _cachedPlanHash = cachedPlanHash;
    }

    boost::optional<size_t> cachedPlanHash() const {
        return _cachedPlanHash;
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
    // If there is a matching cache entry, this is the hash of that plan.
    boost::optional<size_t> _cachedPlanHash;
};

// Shorter namespace alias to keep names from getting too long.
namespace crp_sbe = classic_runtime_planner_for_sbe;

/**
 * A class to hold the result of preparation of the query to be executed using SBE engine with
 * Classic runtime planning. This result stores and provides the following information:
 *   - crp_sbe::PlannerInterface instance that will be used to pick the best plan and get
 * corresponding PlanExecutor.
 *   - PlanCacheInfo for the query.
 */
class SbeWithClassicRuntimePlanningResult {
public:
    PlanCacheInfo& planCacheInfo() {
        return _cacheInfo;
    }

    void setCachedPlanHash(boost::optional<size_t> cachedPlanHash) {
        // SbeWithClassicRuntimePlanningPrepareExecutionHelper passes cached plan hash to the
        // runtime planner.
    }

    std::unique_ptr<crp_sbe::PlannerInterface> runtimePlanner;

private:
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
 */
template <typename KeyType, typename ResultType>
class PrepareExecutionHelper {
public:
    PrepareExecutionHelper(OperationContext* opCtx,
                           const MultipleCollectionAccessor& collections,
                           CanonicalQuery* cq,
                           const QueryPlannerParams& plannerParams)
        : _opCtx{opCtx},
          _collections{collections},
          _cq{cq},
          _providedPlannerParams{plannerParams},
          _result{std::make_unique<ResultType>()} {
        invariant(_cq);
        _plannerParams = plannerParams;
    }

    /**
     * When the instance of this class goes out of scope the trials for multiplanner are completed.
     */
    virtual ~PrepareExecutionHelper() {
        if (_opCtx) {
            if (auto curOp = CurOp::get(_opCtx)) {

                LOGV2_DEBUG(8276400,
                            4,
                            "Stopping the planningTime timer",
                            "query"_attr = redact(_cq->toStringShort()));
                curOp->stopQueryPlanningTimer();
            }
        }
    }

    StatusWith<std::unique_ptr<ResultType>> prepare() {
        const auto& mainColl = getCollections().getMainCollection();

        if (!mainColl) {
            LOGV2_DEBUG(20921,
                        2,
                        "Collection does not exist. Using EOF plan",
                        logAttrs(_cq->nss()),
                        "canonicalQuery"_attr = redact(_cq->toStringShort()));

            auto solution = std::make_unique<QuerySolution>();
            solution->setRoot(std::make_unique<EofNode>());
            if (std::is_same_v<KeyType, sbe::PlanCacheKey>) {
                planCacheCounters.incrementSbeSkippedCounter();
            } else {
                planCacheCounters.incrementClassicSkippedCounter();
            }
            return buildSingleSolutionPlan(std::move(solution));
        }

        // Tailable: If the query requests tailable the collection must be capped.
        if (_cq->getFindCommandRequest().getTailable() && !mainColl->isCapped()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "error processing query: " << _cq->toStringForErrorMsg()
                                        << " tailable cursor requested on non capped collection");
        }

        // If the canonical query does not have a user-specified collation and no one has given the
        // CanonicalQuery a collation already, set it from the collection default.
        if (_cq->getFindCommandRequest().getCollation().isEmpty() &&
            _cq->getCollator() == nullptr && mainColl->getDefaultCollator()) {
            _cq->setCollator(mainColl->getDefaultCollator()->clone());
        }

        // Before consulting the plan cache, check if we should short-circuit and construct a
        // find-by-_id plan.
        if (auto result = buildIdHackPlan()) {
            return {std::move(result)};
        }

        auto planCacheKey = buildPlanCacheKey();
        getResult()->planCacheInfo().queryHash = planCacheKey.queryHash();
        getResult()->planCacheInfo().planCacheKey = planCacheKey.planCacheKeyHash();

        // In each plan cache entry, we store the hash of the cached plan. We use this to indicate
        // whether a plan is cached in explain, by matching the QuerySolution hash to the cached
        // hash.
        boost::optional<size_t> cachedPlanHash = boost::none;
        if (auto cacheResult = buildCachedPlan(planCacheKey); cacheResult) {
            return {std::move(cacheResult)};
        }

        // If we are processing an explain, get the cached plan hash if there is one. This is
        // used for the "isCached" field.
        if (MONGO_unlikely(_cq->isExplainAndCacheIneligible())) {
            cachedPlanHash = getCachedPlanHash(planCacheKey);
        }

        auto result = finishPrepare(/* ignoreQuerySettings*/ false);
        const bool shouldRetryWithoutQuerySettings =
            !result.isOK() && _plannerParams.querySettingsApplied;
        if (shouldRetryWithoutQuerySettings) {
            restorePlannerParams();
            result = finishPrepare(/* ignoreQuerySettings */ true);
        }

        // Set the cachedPlanHash on the result.
        if (result.isOK()) {
            result.getValue()->setCachedPlanHash(cachedPlanHash);
        }
        return result;
    }

    StatusWith<std::unique_ptr<ResultType>> finishPrepare(bool ignoreQuerySettings) {
        initializePlannerParamsIfNeeded(ignoreQuerySettings);
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
                str::stream() << "error processing query: " << _cq->toStringForErrorMsg()
                              << " planner returned error");
        }

        auto solutions = std::move(statusWithMultiPlanSolns.getValue());
        // The planner should have returned an error status if there are no solutions.
        invariant(solutions.size() > 0);

        // See if one of our solutions is a fast count hack in disguise.
        if (_cq->isCountLike()) {
            for (size_t i = 0; i < solutions.size(); ++i) {
                if (turnIxscanIntoCount(solutions[i].get())) {
                    LOGV2_DEBUG(
                        20925, 2, "Using fast count", "query"_attr = redact(_cq->toStringShort()));
                    return buildSingleSolutionPlan(std::move(solutions[i]));
                }
            }
        }

        // Force multiplanning (and therefore caching) if forcePlanCache is set. We could
        // manually update the plan cache instead without multiplanning but this is simpler.
        if (1 == solutions.size() && !_cq->getExpCtxRaw()->forcePlanCache) {
            // Only one possible plan. Build the stages from the solution.
            solutions[0]->indexFilterApplied = _plannerParams.indexFiltersApplied;
            return buildSingleSolutionPlan(std::move(solutions[0]));
        }
        return buildMultiPlan(std::move(solutions));
    }

protected:
    const MultipleCollectionAccessor& getCollections() const {
        return _collections;
    }

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
     * Fills out planner parameters if not already filled.
     */
    void initializePlannerParamsIfNeeded(bool ignoreQuerySettings = false) {
        if (_plannerParamsInitialized) {
            return;
        }

        _plannerParams = _providedPlannerParams;
        _plannerParams.fillOutPlannerParams(_opCtx, *_cq, getCollections(), ignoreQuerySettings);
        _plannerParamsInitialized = true;
    }

    void restorePlannerParams() {
        _plannerParamsInitialized = false;
    }

    /**
     * Attempts to build a special cased fast-path query plan for a find-by-_id query. Returns
     * nullptr if this optimization does not apply.
     */
    virtual std::unique_ptr<ResultType> buildIdHackPlan() = 0;

    /**
     * Constructs the plan cache key.
     */
    virtual KeyType buildPlanCacheKey() const = 0;

    /**
     * If there is only one available query solution, builds a PlanStage tree for it.
     */
    virtual std::unique_ptr<ResultType> buildSingleSolutionPlan(
        std::unique_ptr<QuerySolution> solution) = 0;

    /**
     * Either constructs a PlanStage tree from a cached plan (if exists in the plan cache), or
     * constructs a "id hack" PlanStage tree. Returns nullptr if no cached plan or id hack plan can
     * be constructed.
     */
    virtual std::unique_ptr<ResultType> buildCachedPlan(const KeyType& planCacheKey) = 0;

    // If there is a matching cache entry, retrieves the hash of the cached plan. Otherwise returns
    // boost::none.
    virtual boost::optional<size_t> getCachedPlanHash(const KeyType& planCacheKey) = 0;

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
    const MultipleCollectionAccessor& _collections;
    CanonicalQuery* _cq;
    QueryPlannerParams _plannerParams;
    // Used to avoid filling out the planner params twice.
    bool _plannerParamsInitialized = false;
    // Used for restoring initialized planner parameters to their original values if needed.
    const QueryPlannerParams _providedPlannerParams;
    // In-progress result value of the prepare() call.
    std::unique_ptr<ResultType> _result;
};

/**
 * A helper class to prepare a classic PlanStage tree for execution.
 */
class ClassicPrepareExecutionHelper final
    : public PrepareExecutionHelper<PlanCacheKey, ClassicPrepareExecutionResult> {
public:
    ClassicPrepareExecutionHelper(OperationContext* opCtx,
                                  const MultipleCollectionAccessor& collections,
                                  WorkingSet* ws,
                                  CanonicalQuery* cq,
                                  const QueryPlannerParams& plannerParams)
        : PrepareExecutionHelper{opCtx, collections, std::move(cq), plannerParams}, _ws{ws} {}

private:
    std::unique_ptr<PlanStage> buildExecutableTree(const QuerySolution& solution) const {
        return stage_builder::buildClassicExecutableTree(
            _opCtx, getCollections().getMainCollectionPtrOrAcquisition(), *_cq, solution, _ws);
    }

    std::unique_ptr<ClassicPrepareExecutionResult> buildIdHackPlan() final {
        initializePlannerParamsIfNeeded();

        const auto& mainCollection = getCollections().getMainCollection();
        if (!isIdHackEligibleQuery(
                mainCollection, _cq->getFindCommandRequest(), _cq->getCollator())) {
            return nullptr;
        }

        const IndexDescriptor* descriptor = mainCollection->getIndexCatalog()->findIdIndex(_opCtx);
        if (!descriptor) {
            return nullptr;
        }

        LOGV2_DEBUG(20922,
                    2,
                    "Using classic engine idhack",
                    "canonicalQuery"_attr = redact(_cq->toStringShort()));
        planCacheCounters.incrementClassicSkippedCounter();
        auto result = releaseResult();
        const auto& mainCollectionOrAcquisition =
            getCollections().getMainCollectionPtrOrAcquisition();
        std::unique_ptr<PlanStage> stage = std::make_unique<IDHackStage>(
            _cq->getExpCtxRaw(), _cq, _ws, mainCollectionOrAcquisition, descriptor);

        // Might have to filter out orphaned docs.
        if (_plannerParams.options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
            auto shardFilterer = mainCollectionOrAcquisition.getShardingFilter(_opCtx);
            invariant(shardFilterer,
                      "Attempting to use shard filter when there's no shard filter available for "
                      "the collection");

            stage = std::make_unique<ShardFilterStage>(
                _cq->getExpCtxRaw(), std::move(*shardFilterer), _ws, std::move(stage));
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

    std::unique_ptr<ClassicPrepareExecutionResult> buildSingleSolutionPlan(
        std::unique_ptr<QuerySolution> solution) final {
        auto result = releaseResult();
        auto root = buildExecutableTree(*solution);
        result->emplace(std::move(root), std::move(solution));

        LOGV2_DEBUG(
            8523402, 2, "Only one plan is available", "query"_attr = redact(_cq->toStringShort()));
        return result;
    }

    PlanCacheKey buildPlanCacheKey() const {
        return plan_cache_key_factory::make<PlanCacheKey>(*_cq,
                                                          getCollections().getMainCollection());
    }

    std::unique_ptr<ClassicPrepareExecutionResult> buildCachedPlan(
        const PlanCacheKey& planCacheKey) final {
        initializePlannerParamsIfNeeded();

        if (shouldCacheQuery(*_cq)) {
            const auto& collectionsAccessor = getCollections();
            const auto& mainCollectionOrAcquisition =
                collectionsAccessor.getMainCollectionPtrOrAcquisition();

            // Try to look up a cached solution for the query.
            if (auto cs = CollectionQueryInfo::get(collectionsAccessor.getMainCollection())
                              .getPlanCache()
                              ->getCacheEntryIfActive(planCacheKey)) {
                planCacheCounters.incrementClassicHitsCounter();

                // We have a CachedSolution.  Have the planner turn it into a QuerySolution.
                auto statusWithQs =
                    QueryPlanner::planFromCache(*_cq, _plannerParams, *cs->cachedPlan.get());

                if (statusWithQs.isOK()) {
                    auto querySolution = std::move(statusWithQs.getValue());
                    if (_cq->isCountLike() && turnIxscanIntoCount(querySolution.get())) {
                        LOGV2_DEBUG(5968201,
                                    2,
                                    "Using fast count",
                                    "query"_attr = redact(_cq->toStringShort()));
                    }

                    auto result = releaseResult();
                    auto&& root = buildExecutableTree(*querySolution);

                    // Add a CachedPlanStage on top of the previous root.
                    //
                    // 'decisionWorks' is used to determine whether the existing cache entry should
                    // be evicted, and the query replanned.
                    result->emplace(std::make_unique<CachedPlanStage>(_cq->getExpCtxRaw(),
                                                                      mainCollectionOrAcquisition,
                                                                      _ws,
                                                                      _cq,
                                                                      _plannerParams,
                                                                      cs->decisionWorks.value(),
                                                                      std::move(root)),
                                    std::move(querySolution));
                    return result;
                }
            }
            planCacheCounters.incrementClassicMissesCounter();
        } else {
            planCacheCounters.incrementClassicSkippedCounter();
        }

        return nullptr;
    }

    boost::optional<size_t> getCachedPlanHash(const PlanCacheKey& planCacheKey) final {
        if (auto cs = CollectionQueryInfo::get(getCollections().getMainCollection())
                          .getPlanCache()
                          ->getCacheEntryIfActive(planCacheKey)) {
            return cs->cachedPlan->solutionHash;
        }
        return boost::none;
    }

    std::unique_ptr<ClassicPrepareExecutionResult> buildSubPlan() final {
        auto result = releaseResult();
        result->emplace(
            std::make_unique<SubplanStage>(_cq->getExpCtxRaw(),
                                           getCollections().getMainCollectionPtrOrAcquisition(),
                                           _ws,
                                           _plannerParams,
                                           _cq),
            nullptr /* solution */);
        return result;
    }

    std::unique_ptr<ClassicPrepareExecutionResult> buildMultiPlan(
        std::vector<std::unique_ptr<QuerySolution>> solutions) final {
        // Many solutions. Create a MultiPlanStage to pick the best, update the cache,
        // and so on. The working set will be shared by all candidate plans.
        auto multiPlanStage = std::make_unique<MultiPlanStage>(
            _cq->getExpCtxRaw(), getCollections().getMainCollectionPtrOrAcquisition(), _cq);

        for (auto&& solution : solutions) {
            solution->indexFilterApplied = _plannerParams.indexFiltersApplied;

            auto&& nextPlanRoot = buildExecutableTree(*solution);

            // Takes ownership of 'nextPlanRoot'.
            multiPlanStage->addPlan(std::move(solution), std::move(nextPlanRoot), _ws);
        }

        auto result = releaseResult();
        result->emplace(std::move(multiPlanStage), nullptr /* solution */);
        return result;
    }

    WorkingSet* _ws;
};

/**
 * A helper class to prepare an SBE PlanStage tree for execution. This is not used when
 * featureFlagClassicRuntimePlanningForSbe is enabled. Can be deleted if we delete SBE runtime
 * planners.
 */
class SlotBasedPrepareExecutionHelper final
    : public PrepareExecutionHelper<sbe::PlanCacheKey, SlotBasedPrepareExecutionResult> {
public:
    using PrepareExecutionHelper::PrepareExecutionHelper;

    SlotBasedPrepareExecutionHelper(OperationContext* opCtx,
                                    const MultipleCollectionAccessor& collections,
                                    CanonicalQuery* cq,
                                    PlanYieldPolicy* yieldPolicy,
                                    const QueryPlannerParams& plannerParams)
        : PrepareExecutionHelper{opCtx, collections, cq, plannerParams},
          _yieldPolicy(yieldPolicy) {}

private:
    std::unique_ptr<SlotBasedPrepareExecutionResult> buildIdHackPlan() final {
        // TODO SERVER-66437 SBE is not currently used for IDHACK plans.
        return nullptr;
    }

    std::unique_ptr<SlotBasedPrepareExecutionResult> buildSingleSolutionPlan(
        std::unique_ptr<QuerySolution> solution) final {
        auto result = releaseResult();
        result->emplace(std::move(solution));

        LOGV2_DEBUG(
            8523401, 2, "Only one plan is available", "query"_attr = redact(_cq->toStringShort()));
        return result;
    }

    sbe::PlanCacheKey buildPlanCacheKey() const {
        return plan_cache_key_factory::make(
            *_cq, _collections, canonical_query_encoder::Optimizer::kSbeStageBuilders);
    }

    std::unique_ptr<SlotBasedPrepareExecutionResult> buildCachedPlan(
        const sbe::PlanCacheKey& planCacheKey) final {
        if (shouldCacheQuery(*_cq)) {
            auto&& planCache = sbe::getPlanCache(_opCtx);
            auto cacheEntry = planCache.getCacheEntryIfActive(planCacheKey);
            if (!cacheEntry) {
                planCacheCounters.incrementSbeMissesCounter();
                return nullptr;
            }
            planCacheCounters.incrementSbeHitsCounter();

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
        } else {
            planCacheCounters.incrementSbeSkippedCounter();
        }

        return nullptr;
    }

    boost::optional<size_t> getCachedPlanHash(const sbe::PlanCacheKey& planCacheKey) final {
        auto&& planCache = sbe::getPlanCache(_opCtx);
        if (auto cacheEntry = planCache.getCacheEntryIfActive(planCacheKey); cacheEntry) {
            return cacheEntry->cachedPlan->solutionHash;
        }
        return boost::none;
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
        for (auto&& solution : solutions) {
            solution->indexFilterApplied = _plannerParams.indexFiltersApplied;
            result->emplace(std::move(solution));
        }
        return result;
    }

    PlanYieldPolicy* _yieldPolicy;
};

/**
 * A helper class to initialize classic_runtime_planner_for_sbe::PlannerInterface. This
 * PlannerInterface can subsequently be used to prepare a SBE PlanStage tree using the Classic
 * runtime planners.
 */
class SbeWithClassicRuntimePlanningPrepareExecutionHelper final
    : public PrepareExecutionHelper<sbe::PlanCacheKey, SbeWithClassicRuntimePlanningResult> {
public:
    SbeWithClassicRuntimePlanningPrepareExecutionHelper(
        OperationContext* opCtx,
        const MultipleCollectionAccessor& collections,
        std::unique_ptr<WorkingSet> ws,
        std::unique_ptr<CanonicalQuery> cq,
        PlanYieldPolicy::YieldPolicy policy,
        std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy,
        QueryPlannerParams plannerParams)
        : PrepareExecutionHelper{opCtx, collections, cq.get(), plannerParams},
          _ws{std::move(ws)},
          _ownedCq{std::move(cq)},
          _yieldPolicy{policy},
          _sbeYieldPolicy{std::move(sbeYieldPolicy)} {}

private:
    crp_sbe::PlannerData makePlannerData() {
        return crp_sbe::PlannerData{.cq = std::move(_ownedCq),
                                    .sbeYieldPolicy = std::move(_sbeYieldPolicy),
                                    .workingSet = std::move(_ws),
                                    .collections = _collections,
                                    .plannerParams = std::move(_plannerParams),
                                    .cachedPlanHash = _cachedPlanHash};
    }

    std::unique_ptr<SbeWithClassicRuntimePlanningResult> buildIdHackPlan() final {
        // We expect idhack queries to always use the classic engine.
        return nullptr;
    }

    sbe::PlanCacheKey buildPlanCacheKey() const {
        return plan_cache_key_factory::make(
            *_cq, _collections, canonical_query_encoder::Optimizer::kSbeStageBuilders);
    }

    std::unique_ptr<SbeWithClassicRuntimePlanningResult> buildSingleSolutionPlan(
        std::unique_ptr<QuerySolution> solution) final {
        auto result = releaseResult();
        result->runtimePlanner = std::make_unique<crp_sbe::SingleSolutionPassthroughPlanner>(
            _opCtx, makePlannerData(), std::move(solution));
        return result;
    }

    std::unique_ptr<SbeWithClassicRuntimePlanningResult> buildCachedPlan(
        const sbe::PlanCacheKey& planCacheKey) final {
        if (shouldCacheQuery(*_cq)) {
            auto&& planCache = sbe::getPlanCache(_opCtx);
            auto cacheEntry = planCache.getCacheEntryIfActive(planCacheKey);
            if (!cacheEntry) {
                planCacheCounters.incrementSbeMissesCounter();
                return nullptr;
            }
            planCacheCounters.incrementSbeHitsCounter();

            auto result = releaseResult();
            result->runtimePlanner = std::make_unique<crp_sbe::CachedPlanner>(
                _opCtx, makePlannerData(), std::move(cacheEntry));
            return result;
        }

        planCacheCounters.incrementSbeSkippedCounter();
        return nullptr;
    }

    boost::optional<size_t> getCachedPlanHash(const sbe::PlanCacheKey& planCacheKey) final {
        if (_cachedPlanHash) {
            return _cachedPlanHash;
        }
        auto&& planCache = sbe::getPlanCache(_opCtx);
        if (auto cacheEntry = planCache.getCacheEntryIfActive(planCacheKey); cacheEntry) {
            _cachedPlanHash = cacheEntry->cachedPlan->solutionHash;
            return _cachedPlanHash;
        }
        return boost::none;
    };

    std::unique_ptr<SbeWithClassicRuntimePlanningResult> buildSubPlan() final {
        auto result = releaseResult();
        result->runtimePlanner =
            std::make_unique<crp_sbe::SubPlanner>(_opCtx, makePlannerData(), _yieldPolicy);
        return result;
    }

    std::unique_ptr<SbeWithClassicRuntimePlanningResult> buildMultiPlan(
        std::vector<std::unique_ptr<QuerySolution>> solutions) final {
        for (auto&& solution : solutions) {
            solution->indexFilterApplied = _plannerParams.indexFiltersApplied;
        }

        if (solutions.size() > 1) {
            auto result = releaseResult();
            result->runtimePlanner = std::make_unique<crp_sbe::MultiPlanner>(
                _opCtx, makePlannerData(), _yieldPolicy, std::move(solutions));
            return result;
        } else {
            return buildSingleSolutionPlan(std::move(solutions[0]));
        }
    }

    std::unique_ptr<WorkingSet> _ws;
    std::unique_ptr<CanonicalQuery> _ownedCq;

    // When using the classic multi-planner for SBE, we need both classic and SBE yield policy to
    // support yielding during trial period in classic engine.
    PlanYieldPolicy::YieldPolicy _yieldPolicy;
    std::unique_ptr<PlanYieldPolicySBE> _sbeYieldPolicy;

    // If there is a matching cache entry, this is the hash of that plan.
    boost::optional<size_t> _cachedPlanHash;
};

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getClassicExecutor(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    QueryPlannerParams plannerParams) {
    auto ws = std::make_unique<WorkingSet>();
    ClassicPrepareExecutionHelper helper{
        opCtx, collections, ws.get(), canonicalQuery.get(), plannerParams};
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
                                       collections.getMainCollectionPtrOrAcquisition(),
                                       yieldPolicy,
                                       plannerParams.options,
                                       {},
                                       std::move(solution),
                                       result->cachedPlanHash());
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
    QueryPlannerParams plannerParams,
    boost::optional<const stage_builder::PlanStageData&> planStageData) {
    // If we have multiple solutions, we always need to do the runtime planning.
    if (numSolutions > 1) {
        invariant(!needsSubplanning && !decisionWorks);

        // TODO: SERVER-86174 Avoid unnecessary fillOutPlannerParams() and
        // fillOutSecondaryCollectionsInformation() planner param calls.
        // The SBE PrepareExecutionHelper could have already filled out the planner params, so this
        // could be redundant.
        plannerParams.fillOutPlannerParams(
            opCtx, *canonicalQuery, collections, false /* ignoreQuerySettings */);
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

        // TODO: SERVER-86174 Avoid unnecessary fillOutPlannerParams() and
        // fillOutSecondaryCollectionsInformation() planner param calls.
        // The SBE PrepareExecutionHelper could have already filled out the planner params, so this
        // could be redundant.
        plannerParams.fillOutPlannerParams(
            opCtx, *canonicalQuery, collections, false /* ignoreQuerySettings */);
        return std::make_unique<sbe::SubPlanner>(
            opCtx, collections, *canonicalQuery, plannerParams, yieldPolicy);
    }

    invariant(numSolutions == 1);

    // If we have a single solution and the plan is not pinned or plan contains a hash_lookup stage,
    // we will need to do the runtime planning to check if the cached plan still
    // performs efficiently, or requires re-planning.
    tassert(6693503, "PlanStageData must be present", planStageData);
    const bool hasHashLookup = !planStageData->staticData->foreignHashJoinCollections.empty();
    if (decisionWorks || hasHashLookup) {
        return std::make_unique<sbe::CachedSolutionPlanner>(
            opCtx, collections, *canonicalQuery, plannerParams, decisionWorks, yieldPolicy);
    }

    // Runtime planning is not required.
    return nullptr;
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
getSlotBasedExecutorWithClassicRuntimePlanning(OperationContext* opCtx,
                                               const MultipleCollectionAccessor& collections,
                                               std::unique_ptr<CanonicalQuery> canonicalQuery,
                                               PlanYieldPolicy::YieldPolicy yieldPolicy,
                                               std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy,
                                               QueryPlannerParams plannerParams) {
    SbeWithClassicRuntimePlanningPrepareExecutionHelper helper{
        opCtx,
        collections,
        std::make_unique<WorkingSet>(),
        std::move(canonicalQuery),
        yieldPolicy,
        std::move(sbeYieldPolicy),
        std::move(plannerParams),
    };
    auto planningResultWithStatus = helper.prepare();
    if (!planningResultWithStatus.isOK()) {
        return planningResultWithStatus.getStatus();
    }
    setOpDebugPlanCacheInfo(opCtx, planningResultWithStatus.getValue()->planCacheInfo());
    auto* planner = planningResultWithStatus.getValue()->runtimePlanner.get();
    return planner->plan();
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
getSlotBasedExecutorWithSbeRuntimePlanning(OperationContext* opCtx,
                                           const MultipleCollectionAccessor& collections,
                                           std::unique_ptr<CanonicalQuery> cq,
                                           std::unique_ptr<PlanYieldPolicySBE> yieldPolicy,
                                           QueryPlannerParams plannerParams) {
    SlotBasedPrepareExecutionHelper helper{
        opCtx, collections, cq.get(), yieldPolicy.get(), plannerParams};
    auto planningResultWithStatus = helper.prepare();
    if (!planningResultWithStatus.isOK()) {
        return planningResultWithStatus.getStatus();
    }
    auto planningResult = std::move(planningResultWithStatus.getValue());

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
                                                         plannerParams,
                                                         planStageData)) {
        // Do the runtime planning and pick the best candidate plan.
        auto candidates = runTimePlanner->plan(std::move(solutions), std::move(roots));
        return plan_executor_factory::make(opCtx,
                                           std::move(cq),
                                           std::move(candidates),
                                           collections,
                                           plannerParams.options,
                                           std::move(nss),
                                           std::move(yieldPolicy),
                                           planningResult->cachedPlanHash());
    }

    // No need for runtime planning, just use the constructed plan stage tree.
    invariant(solutions.size() == 1);
    invariant(roots.size() == 1);
    auto&& [root, data] = roots[0];

    if (!planningResult->recoveredPinnedCacheEntry()) {
        if (!cq->cqPipeline().empty()) {
            // Need to extend the solution with the agg pipeline and rebuild the execution tree.
            // TODO: SERVER-86174 Avoid unnecessary fillOutPlannerParams() and
            // fillOutSecondaryCollectionsInformation() planner param calls.
            plannerParams.fillOutPlannerParams(
                opCtx, *cq.get(), collections, false /* ignoreQuerySettings */);
            solutions[0] = QueryPlanner::extendWithAggPipeline(
                *cq, std::move(solutions[0]), plannerParams.secondaryCollectionsInfo);
            roots[0] = stage_builder::buildSlotBasedExecutableTree(
                opCtx, collections, *cq, *(solutions[0]), yieldPolicy.get());
        }

        plan_cache_util::updatePlanCache(opCtx, collections, *cq, *solutions[0], *root, data);
    }

    auto remoteCursors = cq->getExpCtx()->explain
        ? nullptr
        : search_helpers::getSearchRemoteCursors(cq->cqPipeline());
    auto remoteExplains = cq->getExpCtx()->explain
        ? search_helpers::getSearchRemoteExplains(cq->getExpCtxRaw(), cq->cqPipeline())
        : nullptr;

    // Prepare the SBE tree for execution.
    stage_builder::prepareSlotBasedExecutableTree(opCtx,
                                                  root.get(),
                                                  &data,
                                                  *cq,
                                                  collections,
                                                  yieldPolicy.get(),
                                                  planningResult->isRecoveredFromPlanCache(),
                                                  remoteCursors.get());

    return plan_executor_factory::make(opCtx,
                                       std::move(cq),
                                       nullptr /*pipeline*/,
                                       std::move(solutions[0]),
                                       std::move(roots[0]),
                                       {},
                                       plannerParams.options,
                                       std::move(nss),
                                       std::move(yieldPolicy),
                                       planningResult->isRecoveredFromPlanCache(),
                                       planningResult->cachedPlanHash(),
                                       false /* generatedByBonsai */,
                                       {} /* optCounterInfo */,
                                       std::move(remoteCursors),
                                       std::move(remoteExplains));
}  // getSlotBasedExecutor

/**
 * Function which returns true if 'cq' uses features that are currently supported in SBE without
 * 'featureFlagSbeFull' being set; false otherwise.
 */
bool shouldUseRegularSbe(OperationContext* opCtx, const CanonicalQuery& cq, const bool sbeFull) {
    // When featureFlagSbeFull is not enabled, we cannot use SBE unless 'trySbeEngine' is enabled or
    // if 'trySbeRestricted' is enabled, and we have eligible pushed down stages in the cq pipeline.
    auto& queryKnob = QueryKnobConfiguration::decoration(opCtx);
    if (!queryKnob.canPushDownFullyCompatibleStages() && cq.cqPipeline().empty()) {
        return false;
    }

    // Return true if all the expressions in the CanonicalQuery's filter and projection are SBE
    // compatible.
    SbeCompatibility minRequiredCompatibility =
        getMinRequiredSbeCompatibility(queryKnob.getInternalQueryFrameworkControlForOp(), sbeFull);
    return cq.getExpCtx()->sbeCompatibility >= minRequiredCompatibility;
}

bool shouldAttemptSBE(const CanonicalQuery* canonicalQuery) {
    if (!canonicalQuery->isSbeCompatible()) {
        return false;
    }

    // If query settings engine version is set, use it to determine which engine should be used.
    if (auto queryFramework = canonicalQuery->getExpCtx()->getQuerySettings().getQueryFramework()) {
        return *queryFramework == QueryFrameworkControlEnum::kTrySbeEngine;
    }

    return !QueryKnobConfiguration::decoration(canonicalQuery->getOpCtx())
                .isForceClassicEngineEnabled();
}
}  // namespace

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorFind(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    QueryPlannerParams plannerParams,
    Pipeline* pipeline,
    bool needsMerge,
    QueryMetadataBitSet unavailableMetadata) {
    if (OperationShardingState::isComingFromRouter(opCtx)) {
        plannerParams.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    }

    auto exec = [&]() -> StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> {
        invariant(canonicalQuery);
        const auto& mainColl = collections.getMainCollection();
        const auto& canonicalQueryRef = *canonicalQuery.get();
        if (isExpressEligible(opCtx, mainColl, canonicalQueryRef)) {
            plannerParams.fillOutPlannerParams(
                opCtx, canonicalQueryRef, collections, true /* ignoreQuerySettings */);
            PlanExecutor::Deleter planExDeleter(opCtx);
            boost::optional<ScopedCollectionFilter> collFilter = boost::none;
            VariantCollectionPtrOrAcquisition collOrAcq =
                collections.getMainCollectionPtrOrAcquisition();

            planCacheCounters.incrementClassicSkippedCounter();

            if (plannerParams.options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
                collFilter = collOrAcq.getShardingFilter(opCtx);
                invariant(
                    collFilter,
                    "Attempting to use shard filter when there's no shard filter available for "
                    "the collection");
            }

            bool isClusteredOnId = plannerParams.clusteredInfo
                ? clustered_util::isClusteredOnId(plannerParams.clusteredInfo)
                : false;
            std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec(
                new PlanExecutorExpress(
                    opCtx, std::move(canonicalQuery), collOrAcq, collFilter, isClusteredOnId),
                planExDeleter);
            return StatusWith{std::move(exec)};
        }

        canonicalQuery->setSbeCompatible(isQuerySbeCompatible(&mainColl, canonicalQuery.get()));

        auto eligibility = determineBonsaiEligibility(opCtx, mainColl, *canonicalQuery);
        if (isEligibleForBonsaiUnderFrameworkControl(
                opCtx, canonicalQuery->getExplain().has_value(), eligibility)) {
            optimizer::QueryHints queryHints = getHintsFromQueryKnobs();
            const bool fastIndexNullHandling = queryHints._fastIndexNullHandling;

            auto maybeExec = [&] {
                // If the query is eligible for a fast path, use the fast path plan instead of
                // invoking the optimizer.
                if (auto fastPathExec = optimizer::fast_path::tryGetSBEExecutorViaFastPath(
                        collections, canonicalQuery.get())) {
                    return fastPathExec;
                }

                return getSBEExecutorViaCascadesOptimizer(
                    collections, std::move(queryHints), eligibility, canonicalQuery.get());
            }();
            if (maybeExec) {
                auto exec = uassertStatusOK(makeExecFromParams(std::move(canonicalQuery),
                                                               nullptr /*pipeline*/,
                                                               collections,
                                                               std::move(*maybeExec)));
                return std::move(exec);
            } else {
                auto queryControl = QueryKnobConfiguration::decoration(opCtx)
                                        .getInternalQueryFrameworkControlForOp();
                tassert(7319400,
                        "Optimization failed either with forceBonsai set, or without a hint.",
                        queryControl != QueryFrameworkControlEnum::kForceBonsai &&
                            !canonicalQuery->getFindCommandRequest().getHint().isEmpty() &&
                            !fastIndexNullHandling);
            }
        }

        if (shouldAttemptSBE(canonicalQuery.get())) {
            // Add the stages that are candidates for SBE lowering from the 'pipeline' into the
            // 'canonicalQuery'. This must be done _before_ checking shouldUseRegularSbe() or
            // creating SlotBasedPrepareExecutionHelper because both inspect the pipline on the
            // canonical query.
            attachPipelineStages(collections, pipeline, needsMerge, canonicalQuery.get());

            const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
            const bool sbeFull = feature_flags::gFeatureFlagSbeFull.isEnabled(fcvSnapshot);
            const bool canUseRegularSbe = shouldUseRegularSbe(opCtx, *canonicalQuery, sbeFull);

            if (canUseRegularSbe || sbeFull) {
                auto sbeYieldPolicy = PlanYieldPolicySBE::make(
                    opCtx, yieldPolicy, collections, canonicalQuery->nss());
                finalizePipelineStages(pipeline, unavailableMetadata, canonicalQuery.get());
                if (feature_flags::gFeatureFlagClassicRuntimePlanningForSbe.isEnabled(
                        fcvSnapshot)) {
                    return getSlotBasedExecutorWithClassicRuntimePlanning(opCtx,
                                                                          collections,
                                                                          std::move(canonicalQuery),
                                                                          yieldPolicy,
                                                                          std::move(sbeYieldPolicy),
                                                                          std::move(plannerParams));
                } else {
                    return getSlotBasedExecutorWithSbeRuntimePlanning(opCtx,
                                                                      collections,
                                                                      std::move(canonicalQuery),
                                                                      std::move(sbeYieldPolicy),
                                                                      std::move(plannerParams));
                }
            }
        }

        // If we are here, it means the query cannot run in SBE and we should fallback to classic.
        canonicalQuery->setSbeCompatible(false);
        return getClassicExecutor(
            opCtx, collections, std::move(canonicalQuery), yieldPolicy, std::move(plannerParams));
    }();

    if (exec.isOK()) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->debug().queryFramework = exec.getValue()->getQueryFramework();
    }
    return exec;
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getSearchMetadataExecutorSBE(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const NamespaceString& nss,
    const CanonicalQuery& cq,
    executor::TaskExecutorCursor metadataCursor) {
    // For metadata executor, we always have only one remote cursor, any id will work.
    const size_t metadataCursorId = 0;
    auto remoteCursors = std::make_unique<RemoteCursorMap>();
    remoteCursors->insert(
        {metadataCursorId,
         std::make_unique<executor::TaskExecutorCursor>(std::move(metadataCursor))});

    auto sbeYieldPolicy =
        PlanYieldPolicySBE::make(opCtx, PlanYieldPolicy::YieldPolicy::YIELD_AUTO, collections, nss);
    auto root = stage_builder::buildSearchMetadataExecutorSBE(
        opCtx, cq, metadataCursorId, remoteCursors.get(), sbeYieldPolicy.get());
    return plan_executor_factory::make(opCtx,
                                       nullptr /* cq */,
                                       nullptr /*pipeline*/,
                                       nullptr /* solution */,
                                       std::move(root),
                                       nullptr /* optimizerData */,
                                       {} /* plannerOptions */,
                                       cq.nss(),
                                       std::move(sbeYieldPolicy),
                                       false /* planIsFromCache */,
                                       boost::none /* cachedPlanHash */,
                                       false /* generatedByBonsai */,
                                       {} /* optCounterInfo */,
                                       std::move(remoteCursors));
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
                                        cq->getPrimaryMatchExpression(),
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
    CollectionAcquisition coll,
    ParsedDelete* parsedDelete,
    boost::optional<ExplainOptions::Verbosity> verbosity) {
    const auto& collectionPtr = coll.getCollectionPtr();

    auto expCtx = parsedDelete->expCtx();
    OperationContext* opCtx = expCtx->opCtx;
    const DeleteRequest* request = parsedDelete->getRequest();

    const NamespaceString& nss(request->getNsString());

    if (collectionPtr && collectionPtr->isCapped()) {
        expCtx->setIsCappedDelete();
    }

    if (collectionPtr && collectionPtr->isCapped() && opCtx->inMultiDocumentTransaction()) {
        // This check is duplicated from collection_internal::deleteDocument() for two reasons:
        // - Performing a remove on an empty capped collection would not call
        //   collection_internal::deleteDocument().
        // - We can avoid doing lookups on documents and erroring later when trying to delete them.
        return Status(
            ErrorCodes::IllegalOperation,
            str::stream()
                << "Cannot remove from a capped collection in a multi-document transaction: "
                << nss.toStringForErrorMsg());
    }

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::PrimarySteppedDown,
                      str::stream()
                          << "Not primary while removing from " << nss.toStringForErrorMsg());
    }

    auto deleteStageParams = std::make_unique<DeleteStageParams>();
    deleteStageParams->isMulti = request->getMulti();
    deleteStageParams->fromMigrate = request->getFromMigrate();
    deleteStageParams->isExplain = request->getIsExplain();
    deleteStageParams->returnDeleted = request->getReturnDeleted();
    deleteStageParams->sort = request->getSort();
    deleteStageParams->opDebug = opDebug;
    deleteStageParams->stmtId = request->getStmtId();

    if (parsedDelete->isRequestToTimeseries() &&
        !parsedDelete->isEligibleForArbitraryTimeseriesDelete()) {
        deleteStageParams->numStatsForDoc = timeseries::numMeasurementsForBucketCounter(
            collectionPtr->getTimeseriesOptions()->getTimeField());
    }

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    const auto policy = parsedDelete->yieldPolicy();

    if (!collectionPtr) {
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
                                           coll,
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

            const IndexDescriptor* descriptor =
                collectionPtr->getIndexCatalog()->findIdIndex(opCtx);

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
                CollatorInterface::collatorsMatch(collator.get(),
                                                  collectionPtr->getDefaultCollator());

            if (descriptor && CanonicalQuery::isSimpleIdQuery(unparsedQuery) &&
                request->getProj().isEmpty() && hasCollectionDefaultCollation) {
                LOGV2_DEBUG(20928, 2, "Using idhack", "query"_attr = redact(unparsedQuery));

                auto idHackStage = std::make_unique<IDHackStage>(
                    expCtx.get(), unparsedQuery["_id"].wrap(), ws.get(), coll, descriptor);
                std::unique_ptr<DeleteStage> root =
                    std::make_unique<DeleteStage>(expCtx.get(),
                                                  std::move(deleteStageParams),
                                                  ws.get(),
                                                  coll,
                                                  idHackStage.release());
                return plan_executor_factory::make(expCtx,
                                                   std::move(ws),
                                                   std::move(root),
                                                   coll,
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
            !isEligibleForBonsai(opCtx, collectionPtr, *cq));

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

    MultipleCollectionAccessor collectionsAccessor(coll);
    ClassicPrepareExecutionHelper helper{
        opCtx, collectionsAccessor, ws.get(), cq.get(), QueryPlannerParams::DEFAULT};
    auto executionResult = helper.prepare();

    if (!executionResult.isOK()) {
        return executionResult.getStatus();
    }
    auto [root, querySolution] = executionResult.getValue()->extractResultData();
    invariant(root);

    deleteStageParams->canonicalQuery = cq.get();

    // TODO (SERVER-64506): support change streams' pre- and post-images.
    // TODO (SERVER-66079): allow batched deletions in the config.* namespace.
    const bool batchDelete = gBatchUserMultiDeletes.load() &&
        (shard_role_details::getRecoveryUnit(opCtx)->getState() == RecoveryUnit::State::kInactive ||
         shard_role_details::getRecoveryUnit(opCtx)->getState() ==
             RecoveryUnit::State::kActiveNotInUnitOfWork) &&
        !opCtx->inMultiDocumentTransaction() && !opCtx->isRetryableWrite() &&
        !collectionPtr->isChangeStreamPreAndPostImagesEnabled() &&
        !collectionPtr->ns().isConfigDB() && deleteStageParams->isMulti &&
        !deleteStageParams->fromMigrate && !deleteStageParams->returnDeleted &&
        deleteStageParams->sort.isEmpty() && !deleteStageParams->numStatsForDoc;

    auto expCtxRaw = cq->getExpCtxRaw();
    if (parsedDelete->isEligibleForArbitraryTimeseriesDelete()) {
        // Checks if the delete is on a time-series collection and cannot run on bucket documents
        // directly.
        root = std::make_unique<TimeseriesModifyStage>(
            expCtxRaw,
            TimeseriesModifyParams(deleteStageParams.get()),
            ws.get(),
            std::move(root),
            coll,
            timeseries::BucketUnpacker(*collectionPtr->getTimeseriesOptions()),
            parsedDelete->releaseResidualExpr());
    } else if (batchDelete) {
        root = std::make_unique<BatchedDeleteStage>(expCtxRaw,
                                                    std::move(deleteStageParams),
                                                    std::make_unique<BatchedDeleteStageParams>(),
                                                    ws.get(),
                                                    coll,
                                                    root.release());
    } else {
        root = std::make_unique<DeleteStage>(
            expCtxRaw, std::move(deleteStageParams), ws.get(), coll, root.release());
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
                                       coll,
                                       policy,
                                       QueryPlannerParams::DEFAULT,
                                       NamespaceString::kEmpty,
                                       std::move(querySolution),
                                       executionResult.getValue()->cachedPlanHash());
}

//
// Update
//

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorUpdate(
    OpDebug* opDebug,
    CollectionAcquisition coll,
    ParsedUpdate* parsedUpdate,
    boost::optional<ExplainOptions::Verbosity> verbosity) {
    const auto& collectionPtr = coll.getCollectionPtr();

    auto expCtx = parsedUpdate->expCtx();
    OperationContext* opCtx = expCtx->opCtx;

    const UpdateRequest* request = parsedUpdate->getRequest();
    UpdateDriver* driver = parsedUpdate->getDriver();

    const NamespaceString& nss = request->getNamespaceString();

    // If there is no collection and this is an upsert, callers are supposed to create
    // the collection prior to calling this method. Explain, however, will never do
    // collection or database creation.
    if (!coll.exists() && request->isUpsert()) {
        invariant(request->explain());
    }

    // If this is a user-issued update, then we want to return an error: you cannot perform
    // writes on a secondary. If this is an update to a secondary from the replication system,
    // however, then we make an exception and let the write proceed.
    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::PrimarySteppedDown,
                      str::stream() << "Not primary while performing update on "
                                    << nss.toStringForErrorMsg());
    }

    const auto policy = parsedUpdate->yieldPolicy();

    auto documentCounter = [&] {
        if (parsedUpdate->isRequestToTimeseries() &&
            !parsedUpdate->isEligibleForArbitraryTimeseriesUpdate()) {
            return timeseries::numMeasurementsForBucketCounter(
                collectionPtr->getTimeseriesOptions()->getTimeField());
        }
        return UpdateStageParams::DocumentCounter{};
    }();

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    UpdateStageParams updateStageParams(request, driver, opDebug, std::move(documentCounter));

    // If the collection doesn't exist, then return a PlanExecutor for a no-op EOF plan. We have
    // should have already enforced upstream that in this case either the upsert flag is false, or
    // we are an explain. If the collection doesn't exist, we're not an explain, and the upsert flag
    // is true, we expect the caller to have created the collection already.
    if (!coll.exists()) {
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

    if (!parsedUpdate->hasParsedQuery()) {
        // Only consider using the idhack if no hint was provided.
        if (request->getHint().isEmpty()) {
            // This is the idhack fast-path for getting a PlanExecutor without doing the work
            // to create a CanonicalQuery.
            const BSONObj& unparsedQuery = request->getQuery();

            const IndexDescriptor* descriptor =
                collectionPtr->getIndexCatalog()->findIdIndex(opCtx);

            const bool hasCollectionDefaultCollation = CollatorInterface::collatorsMatch(
                expCtx->getCollator(), collectionPtr->getDefaultCollator());

            if (descriptor && CanonicalQuery::isSimpleIdQuery(unparsedQuery) &&
                request->getProj().isEmpty() && hasCollectionDefaultCollation) {
                LOGV2_DEBUG(20930, 2, "Using idhack", "query"_attr = redact(unparsedQuery));

                // Working set 'ws' is discarded. InternalPlanner::updateWithIdHack() makes its own
                // WorkingSet.
                return InternalPlanner::updateWithIdHack(opCtx,
                                                         coll,
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
            !isEligibleForBonsai(opCtx, collectionPtr, *cq));

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

    MultipleCollectionAccessor collectionsAccessor(coll);
    ClassicPrepareExecutionHelper helper{
        opCtx, collectionsAccessor, ws.get(), cq.get(), QueryPlannerParams::DEFAULT};
    auto executionResult = helper.prepare();

    if (!executionResult.isOK()) {
        return executionResult.getStatus();
    }
    auto [root, querySolution] = executionResult.getValue()->extractResultData();
    invariant(root);

    updateStageParams.canonicalQuery = cq.get();
    const bool isUpsert = updateStageParams.request->isUpsert();
    if (parsedUpdate->isEligibleForArbitraryTimeseriesUpdate()) {
        if (request->isMulti()) {
            // If this is a multi-update, we need to spool the data before beginning to apply
            // updates, in order to avoid the Halloween problem.
            root = std::make_unique<SpoolStage>(cq->getExpCtxRaw(), ws.get(), std::move(root));
        }
        if (isUpsert) {
            root = std::make_unique<TimeseriesUpsertStage>(
                cq->getExpCtxRaw(),
                TimeseriesModifyParams(&updateStageParams),
                ws.get(),
                std::move(root),
                coll,
                timeseries::BucketUnpacker(*collectionPtr->getTimeseriesOptions()),
                parsedUpdate->releaseResidualExpr(),
                parsedUpdate->releaseOriginalExpr(),
                *request);
        } else {
            root = std::make_unique<TimeseriesModifyStage>(
                cq->getExpCtxRaw(),
                TimeseriesModifyParams(&updateStageParams),
                ws.get(),
                std::move(root),
                coll,
                timeseries::BucketUnpacker(*collectionPtr->getTimeseriesOptions()),
                parsedUpdate->releaseResidualExpr(),
                parsedUpdate->releaseOriginalExpr());
        }
    } else if (isUpsert) {
        root = std::make_unique<UpsertStage>(
            cq->getExpCtxRaw(), updateStageParams, ws.get(), coll, root.release());
    } else {
        root = std::make_unique<UpdateStage>(
            cq->getExpCtxRaw(), updateStageParams, ws.get(), coll, root.release());
    }

    if (projection) {
        root = std::make_unique<ProjectionStageDefault>(
            cq->getExpCtx(), request->getProj(), projection.get(), ws.get(), std::move(root));
    }

    // We must have a tree of stages in order to have a valid plan executor, but the query
    // solution may be null. Takes ownership of all args other than 'collection' and 'opCtx'
    return plan_executor_factory::make(std::move(cq),
                                       std::move(ws),
                                       std::move(root),
                                       coll,
                                       policy,
                                       QueryPlannerParams::DEFAULT,
                                       NamespaceString::kEmpty,
                                       std::move(querySolution),
                                       executionResult.getValue()->cachedPlanHash());
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
                                bool endKeyInclusive,
                                std::vector<interval_evaluation_tree::IET> iets) {
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
        csn->iets = std::move(iets);
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
            tassert(5506501,
                    "The index of the null interval is invalid",
                    *nullFieldNo < isn->bounds.fields.size());
            auto nullFieldName = isn->bounds.fields[*nullFieldNo].name;
            OrderedIntervalList undefinedPointOil(nullFieldName), nullPointOil(nullFieldName);
            undefinedPointOil.intervals.push_back(IndexBoundsBuilder::kUndefinedPointInterval);
            nullPointOil.intervals.push_back(IndexBoundsBuilder::kNullPointInterval);

            auto makeNullBoundsCountScan =
                [&](OrderedIntervalList& oil) -> std::unique_ptr<QuerySolutionNode> {
                std::swap(isn->bounds.fields[*nullFieldNo], oil);
                ON_BLOCK_EXIT([&] { std::swap(isn->bounds.fields[*nullFieldNo], oil); });

                BSONObj startKey, endKey;
                bool startKeyInclusive, endKeyInclusive;
                if (IndexBoundsBuilder::isSingleInterval(
                        isn->bounds, &startKey, &startKeyInclusive, &endKey, &endKeyInclusive)) {
                    // Build a new IET list based on the rewritten index bounds.
                    std::vector<interval_evaluation_tree::IET> iets = isn->iets;
                    if (!isn->iets.empty()) {
                        tassert(8423396,
                                "IETs and index bounds field must have same size.",
                                iets.size() == isn->bounds.fields.size());
                        iets[*nullFieldNo] = interval_evaluation_tree::IET::make<
                            interval_evaluation_tree::ConstNode>(isn->bounds.fields[*nullFieldNo]);
                    }
                    return makeCountScan(
                        startKey, startKeyInclusive, endKey, endKeyInclusive, std::move(iets));
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
                    OrderedIntervalList emptyArrayPointOil(nullFieldName);
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
    auto csn = makeCountScan(startKey, startKeyInclusive, endKey, endKeyInclusive, isn->iets);
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
    const NamespaceString& nss) {
    const auto& collection = *coll;

    OperationContext* opCtx = expCtx->opCtx;
    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    auto findCommand = std::make_unique<FindCommandRequest>(nss);

    findCommand->setFilter(request.getQuery());
    auto collation = request.getCollation().value_or(BSONObj());
    findCommand->setCollation(collation);
    findCommand->setHint(request.getHint());

    auto& extensionsCallback = collection
        ? static_cast<const ExtensionsCallback&>(ExtensionsCallbackReal(opCtx, &collection->ns()))
        : static_cast<const ExtensionsCallback&>(ExtensionsCallbackNoop());
    auto statusWithCQ = CanonicalQuery::make(
        {.expCtx = expCtx,
         .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand),
                                               .extensionsCallback = std::move(extensionsCallback),
                                               .allowedFeatures =
                                                   MatchExpressionParser::kAllowAllSpecialFeatures},
         .isCountLike = true});
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }

    auto cq = std::move(statusWithCQ.getValue());

    const auto yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO;

    const auto skip = request.getSkip().value_or(0);
    const auto limit = request.getLimit().value_or(0);

    uassert(ErrorCodes::InternalErrorNotSupported,
            "count command is not eligible for bonsai",
            !isEligibleForBonsai(opCtx, collection, *cq));

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
        cq->getPrimaryMatchExpression()->matchType() == MatchExpression::AND &&
        cq->getPrimaryMatchExpression()->numChildren() == 0;
    const bool useRecordStoreCount = isEmptyQueryPredicate && request.getHint().isEmpty();

    if (useRecordStoreCount) {
        std::unique_ptr<PlanStage> root =
            std::make_unique<RecordStoreFastCountStage>(expCtx.get(), &collection, skip, limit);
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

    MultipleCollectionAccessor collectionsAccessor(coll);
    ClassicPrepareExecutionHelper helper{
        opCtx, collectionsAccessor, ws.get(), cq.get(), plannerOptions};
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
                                       NamespaceString::kEmpty,
                                       std::move(querySolution),
                                       executionResult.getValue()->cachedPlanHash());
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

std::unique_ptr<QuerySolution> createDistinctScanSolution(
    const CanonicalDistinct& canonicalDistinct,
    const QueryPlannerParams& plannerParams,
    bool flipDistinctScanDirection) {
    const auto& canonicalQuery = *canonicalDistinct.getQuery();
    if (canonicalQuery.getFindCommandRequest().getFilter().isEmpty() &&
        !canonicalQuery.getSortPattern()) {
        // If a query has neither a filter nor a sort, the query planner won't attempt to use an
        // index for it even if the index could provide the distinct semantics on the key from the
        // 'canonicalDistinct'. So, we create the solution "manually" from a suitable index.
        // The direction of the index doesn't matter in this case.
        size_t distinctNodeIndex = 0;
        auto collator = canonicalQuery.getCollator();
        if (getDistinctNodeIndex(
                plannerParams.indices, canonicalDistinct.getKey(), collator, &distinctNodeIndex)) {
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

            // While on this path there are no sort or filter, the solution still needs to create
            // the projection and 'analyzeDataAccess()' would do that. NB: whether other aspects of
            // data access are important, it's hard to say, this code has been like this since long
            // ago (and it has always passed in new 'QueryPlannerParams').
            auto soln = QueryPlannerAnalysis::analyzeDataAccess(
                canonicalQuery, QueryPlannerParams(), std::move(solnRoot));
            uassert(8404000, "Failed to finalize a DISTINCT_SCAN plan", soln);
            return soln;
        }
    } else {
        // Ask the QueryPlanner for a list of solutions that scan one of the indexes from
        // 'plannerParams' (i.e., the indexes that include the distinct field). Then try to convert
        // one of these plans to a DISTINCT_SCAN.
        auto multiPlanSolns = QueryPlanner::plan(canonicalQuery, plannerParams);
        if (multiPlanSolns.isOK()) {
            auto& solutions = multiPlanSolns.getValue();
            const bool strictDistinctOnly =
                (plannerParams.options & QueryPlannerParams::STRICT_DISTINCT_ONLY);

            for (size_t i = 0; i < solutions.size(); ++i) {
                if (turnIxscanIntoDistinctIxscan(solutions[i].get(),
                                                 canonicalDistinct.getKey(),
                                                 strictDistinctOnly,
                                                 flipDistinctScanDirection)) {
                    // The first suitable distinct scan is as good as any other.
                    return std::move(solutions[i]);
                }
            }
        }
    }
    return nullptr;  // no suitable solution has been found
}
}  // namespace

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> tryGetExecutorDistinct(
    const MultipleCollectionAccessor& collections,
    size_t plannerOptions,
    CanonicalDistinct& canonicalDistinct,
    bool flipDistinctScanDirection) {
    const auto& collectionPtr = collections.getMainCollection();
    if (!collectionPtr) {
        // The caller should create EOF plan for the appropriate engine.
        return {ErrorCodes::NoQueryExecutionPlans, "No viable DISTINCT_SCAN plan"};
    }

    const auto& canonicalQuery = *canonicalDistinct.getQuery();
    auto* opCtx = canonicalQuery.getExpCtx()->opCtx;

    uassert(ErrorCodes::InternalErrorNotSupported,
            "distinct command is not eligible for bonsai",
            !isEligibleForBonsai(opCtx, collectionPtr, canonicalQuery));

    auto getQuerySolution = [&](bool ignoreQuerySettings) -> std::unique_ptr<QuerySolution> {
        QueryPlannerParams plannerParams(
            QueryPlannerParams::ArgsForDistinct{opCtx,
                                                canonicalDistinct,
                                                collections,
                                                plannerOptions,
                                                flipDistinctScanDirection,
                                                ignoreQuerySettings});

        // Can't create a DISTINCT_SCAN stage if no suitable indexes are present.
        if (plannerParams.indices.empty()) {
            return nullptr;
        }
        return createDistinctScanSolution(
            canonicalDistinct, plannerParams, flipDistinctScanDirection);
    };
    auto soln = getQuerySolution(/* ignoreQuerySettings */ false);
    if (!soln) {
        // Try again this time without query settings applied.
        soln = getQuerySolution(/* ignoreQuerySettings */ true);
    }
    if (!soln) {
        return {ErrorCodes::NoQueryExecutionPlans, "No viable DISTINCT_SCAN plan"};
    }

    // Convert the solution into an executable tree.
    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    auto collPtrOrAcq = collections.getMainCollectionPtrOrAcquisition();
    auto&& root = stage_builder::buildClassicExecutableTree(
        opCtx, collPtrOrAcq, canonicalQuery, *soln, ws.get());

    auto exec = plan_executor_factory::make(canonicalDistinct.releaseQuery(),
                                            std::move(ws),
                                            std::move(root),
                                            collPtrOrAcq,
                                            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                            plannerOptions,
                                            NamespaceString::kEmpty,
                                            std::move(soln));
    if (exec.isOK()) {
        LOGV2_DEBUG(20932,
                    2,
                    "Using fast distinct",
                    "query"_attr = redact(exec.getValue()->getCanonicalQuery()->toStringShort()));
    }

    return exec;
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

bool isExpressEligible(OperationContext* opCtx,
                       const CollectionPtr& coll,
                       const CanonicalQuery& cq) {
    auto findCommandReq = cq.getFindCommandRequest();
    return (coll && (cq.getProj() == nullptr || cq.getProj()->isSimple()) &&
            isIdHackEligibleQuery(coll, findCommandReq, cq.getExpCtx()->getCollator()) &&
            !findCommandReq.getReturnKey() && !findCommandReq.getBatchSize() &&
            (coll->getIndexCatalog()->haveIdIndex(opCtx) ||
             clustered_util::isClusteredOnId(coll->getClusteredInfo())));
}
}  // namespace mongo
