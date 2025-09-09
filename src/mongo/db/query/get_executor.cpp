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

#include "mongo/db/query/compiler/ce/exact/exact_cardinality.h"
#include "mongo/db/query/compiler/ce/exact/exact_cardinality_impl.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"

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
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/classic/cached_plan.h"
#include "mongo/db/exec/classic/count.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/eof.h"
#include "mongo/db/exec/classic/multi_plan_rate_limiter.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/record_store_fast_count.h"
#include "mongo/db/exec/classic/sort_key_generator.h"
#include "mongo/db/exec/classic/subplan.h"
#include "mongo/db/exec/classic/update_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/express/plan_executor_express.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/planner_interface.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/physical_model/query_solution/eof_node_type.h"
#include "mongo/db/query/distinct_access.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knob_configuration.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_planner_params_diagnostic_printer.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

boost::intrusive_ptr<ExpressionContext> makeExpressionContextForGetExecutor(
    OperationContext* opCtx,
    const BSONObj& requestCollation,
    const NamespaceString& nss,
    boost::optional<ExplainOptions::Verbosity> verbosity) {
    invariant(opCtx);
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .ns(nss)
                      .mayDbProfile(true)
                      .explain(verbosity)
                      .build();
    if (!requestCollation.isEmpty()) {
        auto statusWithCollator =
            CollatorFactoryInterface::get(expCtx->getOperationContext()->getServiceContext())
                ->makeFromBSON(requestCollation);
        expCtx->setCollator(uassertStatusOK(std::move(statusWithCollator)));
    }
    return expCtx;
}

namespace {
/**
 * Struct to hold information about a query plan's cache info.
 */
struct PlanCacheInfo {
    boost::optional<uint32_t> planCacheKey;
    boost::optional<uint32_t> planCacheShapeHash;
};

/**
 * Fills in the given information on the CurOp::OpDebug object, if it has not already been filled in
 * by an outer pipeline.
 */
void setOpDebugPlanCacheInfo(OperationContext* opCtx, const PlanCacheInfo& cacheInfo) {
    OpDebug& opDebug = CurOp::get(opCtx)->debug();
    if (!opDebug.planCacheShapeHash && cacheInfo.planCacheShapeHash) {
        opDebug.planCacheShapeHash = *cacheInfo.planCacheShapeHash;
    }
    if (!opDebug.planCacheKey && cacheInfo.planCacheKey) {
        opDebug.planCacheKey = *cacheInfo.planCacheKey;
    }
}

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

    auto extractResultData() {
        tassert(8617400,
                "expected '_plannerParams' to be initialized when extracting the result",
                static_cast<bool>(_plannerParams));
        return std::make_tuple(std::move(_roots), std::move(_solutions), std::move(_plannerParams));
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

    void setQueryPlannerParams(std::unique_ptr<QueryPlannerParams> plannerParams) {
        _plannerParams = std::move(plannerParams);
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
    std::unique_ptr<QueryPlannerParams> _plannerParams;
};

// Shorter namespace alias to keep names from getting too long.
namespace crp_classic = classic_runtime_planner;
namespace crp_sbe = classic_runtime_planner_for_sbe;

class ClassicRuntimePlannerResult {
public:
    PlanCacheInfo& planCacheInfo() {
        return _cacheInfo;
    }

    void setCachedPlanHash(boost::optional<size_t> cachedPlanHash) {
        // SbeWithClassicRuntimePlanningPrepareExecutionHelper passes cached plan hash to the
        // runtime planner.
    }

    std::unique_ptr<crp_classic::ClassicPlannerInterface> runtimePlanner;

private:
    PlanCacheInfo _cacheInfo;
};

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

    std::unique_ptr<PlannerInterface> runtimePlanner;

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
 *
 *  TODO SERVER-87752 Refactor 'PrepareExecutionHelper' to better handle result types.
 */
template <typename KeyType, typename ResultType>
class PrepareExecutionHelper {
public:
    PrepareExecutionHelper(OperationContext* opCtx,
                           const MultipleCollectionAccessor& collections,
                           PlanYieldPolicy::YieldPolicy yieldPolicy,
                           CanonicalQuery* cq,
                           std::unique_ptr<QueryPlannerParams> plannerParams)
        : _opCtx{opCtx},
          _collections{collections},
          _yieldPolicy{yieldPolicy},
          _cq{cq},
          _plannerParams(std::move(plannerParams)),
          _result{std::make_unique<ResultType>()} {
        invariant(_cq);
        if (shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(2))) {
            _queryStringForDebugLog = _cq->toStringShort();
        }
    }

    StatusWith<std::unique_ptr<ResultType>> prepare() {
        const auto& mainColl = getCollections().getMainCollection();

        if (!mainColl) {
            LOGV2_DEBUG(20921,
                        2,
                        "Collection does not exist. Using EOF plan",
                        logAttrs(_cq->nss()),
                        "canonicalQuery"_attr = redact(_queryStringForDebugLog));

            auto solution = std::make_unique<QuerySolution>();
            solution->setRoot(std::make_unique<EofNode>(eof_node::EOFType::NonExistentNamespace));
            if (std::is_same_v<KeyType, sbe::PlanCacheKey>) {
                planCacheCounters.incrementSbeSkippedCounter();
            } else {
                planCacheCounters.incrementClassicSkippedCounter();
            }
            return buildSingleSolutionPlan(std::move(solution),
                                           QueryPlanner::CostBasedRankerResult{});
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
        getResult()->planCacheInfo().planCacheShapeHash = planCacheKey.planCacheShapeHash();
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

        // Set the cachedPlanHash on the result.
        auto result = finishPrepare();
        if (result.isOK()) {
            result.getValue()->setCachedPlanHash(cachedPlanHash);
        }
        // Check for interrupt after running the sophisticated planners in finishPrepare()
        if (auto interruptCheck = _cq->getOpCtx()->checkForInterruptNoAssert();
            !interruptCheck.isOK()) {
            return interruptCheck;
        }
        return result;
    }

    StatusWith<std::unique_ptr<ResultType>> finishPrepare() {
        if (SubplanStage::needsSubplanning(*_cq)) {
            LOGV2_DEBUG(20924,
                        2,
                        "Running query as sub-queries",
                        "query"_attr = redact(_queryStringForDebugLog));

            // Forced plan solution hash doesn't make sense to be accessed in QueryPlanner::plan()
            // during subplanning. It would need to be applicable to all branches.
            uassert(ErrorCodes::IllegalOperation,
                    "Use of forcedPlanSolutionHash not permitted for rooted $or queries.",
                    !_cq->getForcedPlanSolutionHash());

            return buildSubPlan();
        }

        std::vector<std::unique_ptr<QuerySolution>> solutions;
        QueryPlanner::CostBasedRankerResult cbrResult;
        auto rankerMode = _plannerParams->planRankerMode;
        if (rankerMode != QueryPlanRankerModeEnum::kMultiPlanning) {
            using namespace cost_based_ranker;
            std::unique_ptr<ce::SamplingEstimator> samplingEstimator{nullptr};
            std::unique_ptr<ce::ExactCardinalityEstimator> exactCardinality{nullptr};
            if (rankerMode == QueryPlanRankerModeEnum::kSamplingCE ||
                rankerMode == QueryPlanRankerModeEnum::kAutomaticCE) {
                auto samplingMode = _cq->getExpCtx()
                                        ->getQueryKnobConfiguration()
                                        .getInternalQuerySamplingCEMethod();
                samplingEstimator = std::make_unique<ce::SamplingEstimatorImpl>(
                    _cq->getOpCtx(),
                    getCollections(),
                    _yieldPolicy,
                    samplingMode == SamplingCEMethodEnum::kRandom
                        ? ce::SamplingEstimatorImpl::SamplingStyle::kRandom
                        : ce::SamplingEstimatorImpl::SamplingStyle::kChunk,
                    CardinalityEstimate{
                        CardinalityType{
                            _plannerParams->mainCollectionInfo.collStats->getCardinality()},
                        EstimationSource::Metadata},
                    _cq->getExpCtx()->getQueryKnobConfiguration().getConfidenceInterval(),
                    samplingMarginOfError.load(),
                    internalQueryNumChunksForChunkBasedSampling.load());
            } else if (rankerMode == QueryPlanRankerModeEnum::kExactCE) {
                exactCardinality = std::make_unique<ce::ExactCardinalityImpl>(
                    getCollections().getMainCollection(), *_cq, _opCtx);
            }

            // Populating the 'topLevelSampleFields' requires 2 steps:
            //  1. Extract the set of top level fields from the filter, sort and project
            //  components of the CanonicalQuery.
            //  2. Extract the fields of the relevant indexes from the plan() function by passing in
            //  the pointer to 'topLevelSampleFieldNames' as an output parameter.
            auto topLevelSampleFieldNames =
                ce::extractTopLevelFieldsFromMatchExpression(_cq->getPrimaryMatchExpression());
            auto statusWithMultiPlanSolns =
                QueryPlanner::plan(*_cq, *_plannerParams, topLevelSampleFieldNames);
            if (!statusWithMultiPlanSolns.isOK()) {
                return statusWithMultiPlanSolns.getStatus().withContext(
                    str::stream() << "error processing query: " << _cq->toStringForErrorMsg()
                                  << " planner returned error");
            }
            if (samplingEstimator) {
                // If we do not have any fields that we want to sample then we just include all the
                // fields in the sample. This can occur if we encounter a find all query with no
                // project or sort specified.
                // TODO: SERVER-108819 We can skip generating the sample entirely in this case and
                // instead use collection cardinality.
                samplingEstimator->generateSample(
                    topLevelSampleFieldNames.empty()
                        ? ce::ProjectionParams{ce::NoProjection{}}
                        : ce::TopLevelFieldsProjection{std::move(topLevelSampleFieldNames)});
            }
            auto statusWithCBRSolns =
                QueryPlanner::planWithCostBasedRanking(*_cq,
                                                       *_plannerParams,
                                                       samplingEstimator.get(),
                                                       exactCardinality.get(),
                                                       std::move(statusWithMultiPlanSolns));
            if (!statusWithCBRSolns.isOK()) {
                return statusWithCBRSolns.getStatus();
            }
            solutions = std::move(statusWithCBRSolns.getValue().solutions);
            cbrResult = std::move(statusWithCBRSolns.getValue());
        } else {
            auto statusWithMultiPlanSolns = QueryPlanner::plan(*_cq, *_plannerParams);
            if (!statusWithMultiPlanSolns.isOK()) {
                return statusWithMultiPlanSolns.getStatus().withContext(
                    str::stream() << "error processing query: " << _cq->toStringForErrorMsg()
                                  << " planner returned error");
            }
            solutions = std::move(statusWithMultiPlanSolns.getValue());
        }

        // The planner should have returned an error status if there are no solutions.
        invariant(solutions.size() > 0);

        // See if one of our solutions is a fast count hack in disguise.
        if (_cq->isCountLike()) {
            for (size_t i = 0; i < solutions.size(); ++i) {
                if (QueryPlannerAnalysis::turnIxscanIntoCount(solutions[i].get())) {
                    LOGV2_DEBUG(20925,
                                2,
                                "Using fast count",
                                "query"_attr = redact(_queryStringForDebugLog));
                    return buildSingleSolutionPlan(std::move(solutions[i]), std::move(cbrResult));
                }
            }
        }

        // Force multiplanning (and therefore caching) if forcePlanCache is set. We could
        // manually update the plan cache instead without multiplanning but this is simpler.
        if (1 == solutions.size() && !_cq->getExpCtxRaw()->getForcePlanCache() &&
            !internalQueryPlannerUseMultiplannerForSingleSolutions) {
            // Only one possible plan. Build the stages from the solution.
            solutions[0]->indexFilterApplied = _plannerParams->indexFiltersApplied;
            return buildSingleSolutionPlan(std::move(solutions[0]), std::move(cbrResult));
        }
        return buildMultiPlan(std::move(solutions), std::move(cbrResult));
    }

    const QueryPlannerParams& getPlannerParams() {
        return *_plannerParams.get();
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
        std::unique_ptr<QuerySolution> solution, QueryPlanner::CostBasedRankerResult cbrResult) = 0;

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
        std::vector<std::unique_ptr<QuerySolution>> solutions,
        QueryPlanner::CostBasedRankerResult cbrResult) = 0;

    /**
     * Helper for getting the QuerySolution hash from the plan caches.
     */
    boost::optional<size_t> getPlanHashFromClassicCache(const PlanCacheKey& key) {
        if (auto cs = CollectionQueryInfo::get(getCollections().getMainCollection())
                          .getPlanCache()
                          ->getCacheEntryIfActive(key)) {
            return cs->cachedPlan->solutionHash;
        }
        return boost::none;
    }

    OperationContext* _opCtx;
    const MultipleCollectionAccessor& _collections;
    PlanYieldPolicy::YieldPolicy _yieldPolicy;
    CanonicalQuery* _cq;
    // Stored as a smart pointer for memory safety reasons. Storing a reference would be even
    // faster, but also more prone to memory errors. Storing a direct value would incur copying
    // costs when std::move()ing, since QueryPlannerParams is just a big aggregated structure.
    std::unique_ptr<QueryPlannerParams> _plannerParams;
    // In-progress result value of the prepare() call.
    std::unique_ptr<ResultType> _result;

    // Cached result of CanonicalQuery::toStringShort(). Only populated when logging verbosity is
    // high enough to enable messages that need it.
    std::string _queryStringForDebugLog;
};

class ClassicPrepareExecutionHelper final
    : public PrepareExecutionHelper<PlanCacheKey, ClassicRuntimePlannerResult> {
public:
    ClassicPrepareExecutionHelper(OperationContext* opCtx,
                                  const MultipleCollectionAccessor& collections,
                                  std::unique_ptr<WorkingSet> ws,
                                  CanonicalQuery* cq,
                                  PlanYieldPolicy::YieldPolicy yieldPolicy,
                                  std::unique_ptr<QueryPlannerParams> plannerParams)
        : PrepareExecutionHelper{opCtx, collections, yieldPolicy, cq, std::move(plannerParams)},
          _ws{std::move(ws)} {}

private:
    using CachedSolutionPair =
        std::pair<std::unique_ptr<CachedSolution>, std::unique_ptr<QuerySolution>>;

    PlannerData makePlannerData() {
        return PlannerData{_opCtx,
                           _cq,
                           std::move(_ws),
                           _collections,
                           std::move(_plannerParams),
                           _yieldPolicy,
                           _cachedPlanHash};
    }

    std::unique_ptr<ClassicRuntimePlannerResult> buildIdHackPlan() final {
        const auto& mainCollection = getCollections().getMainCollection();
        if (!isIdHackEligibleQuery(mainCollection, *_cq)) {
            return nullptr;
        }

        const IndexDescriptor* descriptor = mainCollection->getIndexCatalog()->findIdIndex(_opCtx);
        if (!descriptor) {
            return nullptr;
        }

        LOGV2_DEBUG(20922,
                    2,
                    "Using classic engine idhack",
                    "canonicalQuery"_attr = redact(_queryStringForDebugLog));
        planCacheCounters.incrementClassicSkippedCounter();
        fastPathQueryCounters.incrementIdHackQueryCounter();
        auto result = releaseResult();
        result->runtimePlanner =
            std::make_unique<crp_classic::IdHackPlanner>(makePlannerData(), descriptor);
        return result;
    }

    std::unique_ptr<ClassicRuntimePlannerResult> buildSingleSolutionPlan(
        std::unique_ptr<QuerySolution> solution,
        QueryPlanner::CostBasedRankerResult cbrResult) final {
        auto result = releaseResult();
        result->runtimePlanner = std::make_unique<crp_classic::SingleSolutionPassthroughPlanner>(
            makePlannerData(), std::move(solution), std::move(cbrResult));
        return result;
    }

    PlanCacheKey buildPlanCacheKey() const override {
        return plan_cache_key_factory::make<PlanCacheKey>(*_cq,
                                                          getCollections().getMainCollection());
    }

    std::unique_ptr<ClassicRuntimePlannerResult> buildCachedPlan(
        const PlanCacheKey& planCacheKey) final {
        if (!shouldCacheQuery(*_cq)) {
            planCacheCounters.incrementClassicSkippedCounter();
            return nullptr;
        }

        auto cachedSolutionPair = retrievePlanFromCache(planCacheKey);

        const bool noCachedPlan = !cachedSolutionPair.has_value();
        const bool cachedPlanIsNotForClassic = !noCachedPlan &&
            cachedSolutionPair->first->decisionReadsOrWorks &&
            !std::holds_alternative<NumWorks>(
                cachedSolutionPair->first->decisionReadsOrWorks->data);

        // If cachedSolutionPair is empty or the stored entry uses NumReads instead of NumWorks, we
        // cannot use it.
        if (noCachedPlan || cachedPlanIsNotForClassic) {
            planCacheCounters.incrementClassicMissesCounter();
            return nullptr;
        }

        planCacheCounters.incrementClassicHitsCounter();
        auto result = releaseResult();
        auto [cachedSolution, querySolution] = std::move(*cachedSolutionPair);
        result->runtimePlanner = std::make_unique<crp_classic::CachedPlanner>(
            makePlannerData(), std::move(cachedSolution), std::move(querySolution));
        return result;
    }

    boost::optional<size_t> getCachedPlanHash(const PlanCacheKey& planCacheKey) final {
        if (_cachedPlanHash) {
            return _cachedPlanHash;
        }

        _cachedPlanHash = getPlanHashFromClassicCache(planCacheKey);
        return _cachedPlanHash;
    }

    std::unique_ptr<ClassicRuntimePlannerResult> buildSubPlan() final {
        auto result = releaseResult();
        result->runtimePlanner = std::make_unique<crp_classic::SubPlanner>(makePlannerData());
        return result;
    }

    std::unique_ptr<ClassicRuntimePlannerResult> buildMultiPlan(
        std::vector<std::unique_ptr<QuerySolution>> solutions,
        QueryPlanner::CostBasedRankerResult cbrResult) final {
        auto result = releaseResult();
        result->runtimePlanner = std::make_unique<crp_classic::MultiPlanner>(
            makePlannerData(), std::move(solutions), std::move(cbrResult));
        return result;
    }

    boost::optional<CachedSolutionPair> retrievePlanFromCache(const PlanCacheKey& planCacheKey) {
        auto cs = CollectionQueryInfo::get(getCollections().getMainCollection())
                      .getPlanCache()
                      ->getCacheEntryIfActive(planCacheKey);
        if (!cs) {
            return boost::none;
        }

        // We have a CachedSolution.  Have the planner turn it into a QuerySolution.
        auto statusWithQs =
            QueryPlanner::planFromCache(*_cq, *_plannerParams, *cs->cachedPlan.get());
        if (!statusWithQs.isOK()) {
            return boost::none;
        }

        std::unique_ptr<QuerySolution> querySolution = std::move(statusWithQs.getValue());
        if (_cq->isCountLike()) {
            const bool usedFastCount =
                QueryPlannerAnalysis::turnIxscanIntoCount(querySolution.get());
            if (usedFastCount) {
                LOGV2_DEBUG(
                    5968201, 2, "Using fast count", "query"_attr = redact(_queryStringForDebugLog));
            }
        }

        return {std::make_pair(std::move(cs), std::move(querySolution))};
    }

    std::unique_ptr<WorkingSet> _ws;
    boost::optional<size_t> _cachedPlanHash;
};

/**
 * Base class for SBE with classic runtime planning prepare execution helper.
 *
 *                PrepareExecutionHelper
 *                /                     \
 *   ClassicPrepareExecutionHelper    SbeWithClassicRuntimePlanningPrepareExecutionHelperBase
 *                                     /                                                   |
 *                 SbeWithClassicRuntimePlanningAndClassicCachePrepareExecutionHelper      |
 *                                                                                         |
 *                                 SbeWithClassicRuntimePlanningAndSbeCachePrepareExecutionHelper
 */
template <class CacheKey, class RuntimePlanningResult>
class SbeWithClassicRuntimePlanningPrepareExecutionHelperBase
    : public PrepareExecutionHelper<CacheKey, RuntimePlanningResult> {
public:
    SbeWithClassicRuntimePlanningPrepareExecutionHelperBase(
        OperationContext* opCtx,
        const MultipleCollectionAccessor& collections,
        std::unique_ptr<WorkingSet> ws,
        CanonicalQuery* cq,
        PlanYieldPolicy::YieldPolicy policy,
        std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy,
        std::unique_ptr<QueryPlannerParams> plannerParams,
        bool useSbePlanCache)
        : PrepareExecutionHelper<CacheKey, RuntimePlanningResult>(
              opCtx, collections, policy, cq, std::move(plannerParams)),
          _ws{std::move(ws)},
          _sbeYieldPolicy{std::move(sbeYieldPolicy)},
          _useSbePlanCache{useSbePlanCache} {}

protected:
    crp_sbe::PlannerDataForSBE makePlannerData() {
        // Use of 'this->' is necessary since some compilers have trouble resolving member
        // variables in templated parent class.
        return crp_sbe::PlannerDataForSBE{this->_opCtx,
                                          this->_cq,
                                          std::move(_ws),
                                          this->_collections,
                                          std::move(this->_plannerParams),
                                          this->_yieldPolicy,
                                          _cachedPlanHash,
                                          std::move(_sbeYieldPolicy),
                                          _useSbePlanCache};
    }

    std::unique_ptr<SbeWithClassicRuntimePlanningResult> buildIdHackPlan() final {
        // We expect idhack queries to always use the classic engine.
        return nullptr;
    }

    std::unique_ptr<SbeWithClassicRuntimePlanningResult> buildSingleSolutionPlan(
        std::unique_ptr<QuerySolution> solution, QueryPlanner::CostBasedRankerResult) final {
        // TODO SERVER-92589: Support CBR with SBE plans
        auto result = this->releaseResult();
        result->runtimePlanner = std::make_unique<crp_sbe::SingleSolutionPassthroughPlanner>(
            makePlannerData(), std::move(solution));
        return result;
    }

    std::unique_ptr<SbeWithClassicRuntimePlanningResult> buildSubPlan() final {
        auto result = this->releaseResult();
        result->runtimePlanner = std::make_unique<crp_sbe::SubPlanner>(makePlannerData());
        return result;
    }

    std::unique_ptr<SbeWithClassicRuntimePlanningResult> buildMultiPlan(
        std::vector<std::unique_ptr<QuerySolution>> solutions,
        QueryPlanner::CostBasedRankerResult cbrResult) final {
        // TODO SERVER-92589: Support CBR with SBE plans
        for (auto&& solution : solutions) {
            solution->indexFilterApplied = this->_plannerParams->indexFiltersApplied;
        }

        if (solutions.size() > 1 ||
            // Search queries are not supported in classic multi-planner.
            (internalQueryPlannerUseMultiplannerForSingleSolutions &&
             !this->_cq->isSearchQuery())) {
            auto result = this->releaseResult();
            result->runtimePlanner = std::make_unique<crp_sbe::MultiPlanner>(
                this->makePlannerData(), std::move(solutions), true /*shouldWriteToPlanCache*/);
            return result;
        } else {
            return this->buildSingleSolutionPlan(std::move(solutions[0]), std::move(cbrResult));
        }
    }

    std::unique_ptr<WorkingSet> _ws;

    // When using the classic multi-planner for SBE, we need both classic and SBE yield policy to
    // support yielding during trial period in classic engine. The classic yield policy to stored in
    // 'PrepareExecutionHelper'.
    std::unique_ptr<PlanYieldPolicySBE> _sbeYieldPolicy;

    const bool _useSbePlanCache;

    // If there is a matching cache entry, this is the hash of that plan.
    boost::optional<size_t> _cachedPlanHash;
};

/**
 * Helper for SBE with classic runtime planning and SBE plan cache.
 */
class SbeWithClassicRuntimePlanningAndSbeCachePrepareExecutionHelper final
    : public SbeWithClassicRuntimePlanningPrepareExecutionHelperBase<
          sbe::PlanCacheKey,
          SbeWithClassicRuntimePlanningResult> {
public:
    SbeWithClassicRuntimePlanningAndSbeCachePrepareExecutionHelper(
        OperationContext* opCtx,
        const MultipleCollectionAccessor& collections,
        std::unique_ptr<WorkingSet> ws,
        CanonicalQuery* cq,
        PlanYieldPolicy::YieldPolicy policy,
        std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy,
        std::unique_ptr<QueryPlannerParams> plannerParams)
        : SbeWithClassicRuntimePlanningPrepareExecutionHelperBase{opCtx,
                                                                  collections,
                                                                  std::move(ws),
                                                                  cq,
                                                                  policy,
                                                                  std::move(sbeYieldPolicy),
                                                                  std::move(plannerParams),
                                                                  true /*useSbePlanCache*/} {}

private:
    sbe::PlanCacheKey buildPlanCacheKey() const override {
        return plan_cache_key_factory::make(*_cq, _collections);
    }

    std::unique_ptr<SbeWithClassicRuntimePlanningResult> tryToBuildCachedPlanFromSbeCache(
        const sbe::PlanCacheKey& sbeCacheKey) {
        auto&& planCache = sbe::getPlanCache(_opCtx);

        auto cacheEntry = planCache.getCacheEntryIfActive(sbeCacheKey);
        if (!cacheEntry) {
            planCacheCounters.incrementSbeMissesCounter();
            return nullptr;
        }
        planCacheCounters.incrementSbeHitsCounter();

        auto result = releaseResult();
        const auto cachedSolutionHash = cacheEntry->cachedPlan->solutionHash;
        result->runtimePlanner = crp_sbe::makePlannerForSbeCacheEntry(
            makePlannerData(), std::move(cacheEntry), cachedSolutionHash);
        return result;
    }

    /**
     * Helper for getting the plan hash from the SBE cache.
     */
    boost::optional<size_t> getPlanHashFromSbeCache(const sbe::PlanCacheKey& key) {
        auto&& planCache = sbe::getPlanCache(_opCtx);
        if (auto cacheEntry = planCache.getCacheEntryIfActive(key); cacheEntry) {
            return cacheEntry->cachedPlan->solutionHash;
        }
        return boost::none;
    }

    std::unique_ptr<SbeWithClassicRuntimePlanningResult> buildCachedPlan(
        const sbe::PlanCacheKey& key) final {
        if (shouldCacheQuery(*_cq)) {
            return tryToBuildCachedPlanFromSbeCache(key);
        }

        planCacheCounters.incrementSbeSkippedCounter();
        return nullptr;
    }

    boost::optional<size_t> getCachedPlanHash(const sbe::PlanCacheKey& key) final {
        if (_cachedPlanHash) {
            return _cachedPlanHash;
        }

        _cachedPlanHash = getPlanHashFromSbeCache(key);
        return _cachedPlanHash;
    }
};

/**
 * Helper for SBE with classic runtime planning and classic plan cache.
 */
class SbeWithClassicRuntimePlanningAndClassicCachePrepareExecutionHelper final
    : public SbeWithClassicRuntimePlanningPrepareExecutionHelperBase<
          PlanCacheKey,
          SbeWithClassicRuntimePlanningResult> {
public:
    SbeWithClassicRuntimePlanningAndClassicCachePrepareExecutionHelper(
        OperationContext* opCtx,
        const MultipleCollectionAccessor& collections,
        std::unique_ptr<WorkingSet> ws,
        CanonicalQuery* cq,
        PlanYieldPolicy::YieldPolicy policy,
        std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy,
        std::unique_ptr<QueryPlannerParams> plannerParams)
        : SbeWithClassicRuntimePlanningPrepareExecutionHelperBase{opCtx,
                                                                  collections,
                                                                  std::move(ws),
                                                                  cq,
                                                                  policy,
                                                                  std::move(sbeYieldPolicy),
                                                                  std::move(plannerParams),
                                                                  false /*useSbePlanCache*/} {}

private:
    PlanCacheKey buildPlanCacheKey() const override {
        return plan_cache_key_factory::make<PlanCacheKey>(*_cq, _collections.getMainCollection());
    }

    std::unique_ptr<SbeWithClassicRuntimePlanningResult> tryToBuildCachedPlanFromClassicCache(
        const PlanCacheKey& planCacheKey) {
        const auto& mainColl = _collections.getMainCollection();

        // Try to look up a cached solution for the query.
        if (auto cs = CollectionQueryInfo::get(mainColl).getPlanCache()->getCacheEntryIfActive(
                planCacheKey)) {
            // We have a CachedSolution.  Have the planner turn it into a QuerySolution.
            auto statusWithQs = QueryPlanner::planFromCache(*_cq, *_plannerParams, *cs->cachedPlan);

            if (statusWithQs.isOK()) {
                auto querySolution = std::move(statusWithQs.getValue());

                // This is a no-op when there is no pipeline to push down.
                querySolution = QueryPlanner::extendWithAggPipeline(
                    *_cq, std::move(querySolution), _plannerParams->secondaryCollectionsInfo);

                auto result = releaseResult();

                result->runtimePlanner =
                    crp_sbe::makePlannerForClassicCacheEntry(makePlannerData(),
                                                             std::move(querySolution),
                                                             cs->cachedPlan->solutionHash,
                                                             cs->decisionReads());

                planCacheCounters.incrementClassicHitsCounter();
                return result;
            }
        }

        planCacheCounters.incrementClassicMissesCounter();
        return nullptr;
    }

    std::unique_ptr<SbeWithClassicRuntimePlanningResult> buildCachedPlan(
        const PlanCacheKey& classicKey) final {
        if (shouldCacheQuery(*_cq)) {
            return tryToBuildCachedPlanFromClassicCache(classicKey);
        }

        planCacheCounters.incrementClassicSkippedCounter();
        return nullptr;
    }

    boost::optional<size_t> getCachedPlanHash(const PlanCacheKey& key) final {
        if (_cachedPlanHash) {
            return _cachedPlanHash;
        }

        _cachedPlanHash = getPlanHashFromClassicCache(key);
        return _cachedPlanHash;
    }
};

std::unique_ptr<PlannerInterface> getClassicPlanner(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    CanonicalQuery* canonicalQuery,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    std::unique_ptr<QueryPlannerParams> plannerParams) {
    ClassicPrepareExecutionHelper helper{
        opCtx,
        collections,
        std::make_unique<WorkingSet>(),
        canonicalQuery,
        yieldPolicy,
        std::move(plannerParams),
    };

    ScopedDebugInfo queryPlannerParams(
        "queryPlannerParams",
        diagnostic_printers::QueryPlannerParamsPrinter{helper.getPlannerParams()});
    auto planningResult = uassertStatusOK(helper.prepare());
    setOpDebugPlanCacheInfo(opCtx, planningResult->planCacheInfo());
    uassertStatusOK(planningResult->runtimePlanner->plan());
    return std::move(planningResult->runtimePlanner);
}

template <class PrepareExecutionHelperType>
std::unique_ptr<PlannerInterface> getClassicPlannerForSbe(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    CanonicalQuery* canonicalQuery,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy,
    std::unique_ptr<QueryPlannerParams> plannerParams) {
    PrepareExecutionHelperType helper{
        opCtx,
        collections,
        std::make_unique<WorkingSet>(),
        std::move(canonicalQuery),
        yieldPolicy,
        std::move(sbeYieldPolicy),
        std::move(plannerParams),
    };
    ScopedDebugInfo queryPlannerParams(
        "queryPlannerParams",
        diagnostic_printers::QueryPlannerParamsPrinter{helper.getPlannerParams()});
    auto planningResult = uassertStatusOK(helper.prepare());
    setOpDebugPlanCacheInfo(opCtx, planningResult->planCacheInfo());
    return std::move(planningResult->runtimePlanner);
}

/**
 * Function which returns true if 'cq' uses features that are currently supported in SBE without
 * 'featureFlagSbeFull' being set; false otherwise.
 */
bool shouldUseRegularSbe(OperationContext* opCtx,
                         const CanonicalQuery& cq,
                         const CollectionPtr& mainCollection,
                         const bool sbeFull) {
    // When featureFlagSbeFull is not enabled, we cannot use SBE unless 'trySbeEngine' is enabled or
    // if 'trySbeRestricted' is enabled, and we have eligible pushed down stages in the cq pipeline.
    auto& queryKnob = cq.getExpCtx()->getQueryKnobConfiguration();
    if (!queryKnob.canPushDownFullyCompatibleStages() && cq.cqPipeline().empty()) {
        return false;
    }

    if (mainCollection && mainCollection->isTimeseriesCollection() && cq.cqPipeline().empty()) {
        // TS queries only use SBE when there's a pipeline.
        return false;
    }

    // Return true if all the expressions in the CanonicalQuery's filter and projection are SBE
    // compatible.
    SbeCompatibility minRequiredCompatibility =
        getMinRequiredSbeCompatibility(queryKnob.getInternalQueryFrameworkControlForOp(), sbeFull);
    return cq.getExpCtx()->getSbeCompatibility() >= minRequiredCompatibility;
}

bool shouldUseSbePlanCache(const QueryPlannerParams& params) {
    // The logic in this funtion depends on the fact that we clear the SBE plan cache on index
    // creation.

    // SBE feature flag guards SBE plan cache use. Check this first to avoid doing potentially
    // expensive checks unnecessarily.
    if (!feature_flags::gFeatureFlagSbeFull.isEnabled()) {
        return false;
    }

    // SBE plan cache does not support partial indexes.
    // TODO SERVER-94392: Remove this restriction once they are supported.
    for (const auto& idx : params.mainCollectionInfo.indexes) {
        if (idx.filterExpr) {
            return false;
        }
    }
    return true;
}

boost::optional<ScopedCollectionFilter> getScopedCollectionFilter(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const QueryPlannerParams& plannerParams) {
    if (plannerParams.mainCollectionInfo.options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
        auto collFilter = collections.getMainCollectionPtrOrAcquisition().getShardingFilter(opCtx);
        invariant(collFilter,
                  "Attempting to use shard filter when there's no shard filter available for "
                  "the collection");
        return collFilter;
    }
    return boost::none;
}

void setCurOpQueryFramework(const PlanExecutor* executor) {
    auto opCtx = executor->getOpCtx();
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    CurOp::get(opCtx)->debug().queryFramework = executor->getQueryFramework();
}
}  // namespace

/**
 * Returns true iff 'descriptor' has fields A and B where all of the following hold
 *
 *   - A is a path prefix of B
 *   - A is a hashed field in the index
 *   - B is a non-hashed field in the index
 *
 * TODO SERVER-99889 this is a workaround for an SBE stage builder bug.
 */
bool indexHasHashedPathPrefixOfNonHashedPath(const IndexDescriptor* descriptor) {
    boost::optional<StringData> hashedPath;
    for (const auto& elt : descriptor->keyPattern()) {
        if (elt.valueStringDataSafe() == "hashed") {
            // Indexes may only contain one hashed field.
            hashedPath = elt.fieldNameStringData();
            break;
        }
    }
    if (hashedPath == boost::none) {
        // No hashed fields in the index.
        return false;
    }
    // Check if 'hashedPath' is a path prefix for any field in the index.
    for (const auto& elt : descriptor->keyPattern()) {
        if (expression::isPathPrefixOf(hashedPath.get(), elt.fieldNameStringData())) {
            return true;
        }
    }
    return false;
}

/**
 * Returns true if 'collection' has an index that contains two fields, one of which is a path prefix
 * of the other, where the prefix field is hashed. Indexes can only contain one hashed field.
 *
 * TODO SERVER-99889: At the time of writing, there is a bug in the SBE stage builders that
 * constructs ExpressionFieldPaths over hashed values. This leads to wrong query results.
 *
 * The bug arises for covered index scans where a path P is a non-hashed path in the index and a
 * strict prefix P' of P is a hashed path in the index.
 */
bool collectionHasIndexWithHashedPathPrefixOfNonHashedPath(const CollectionPtr& collection,
                                                           ExpressionContext* expCtx) {
    const IndexCatalog* indexCatalog = collection->getIndexCatalog();
    tassert(10230200, "'CollectionPtr' does not have an 'IndexCatalog'", indexCatalog);
    OperationContext* opCtx = expCtx->getOperationContext();
    tassert(10230201, "'ExpressionContext' does not have an 'OperationContext'", opCtx);
    std::unique_ptr<IndexCatalog::IndexIterator> indexIter =
        indexCatalog->getIndexIterator(IndexCatalog::InclusionPolicy::kReady);
    while (indexIter->more()) {
        const IndexCatalogEntry* entry = indexIter->next();
        if (indexHasHashedPathPrefixOfNonHashedPath(entry->descriptor())) {
            return true;
        }
    }
    return false;
}

/**
 * Checks if the given query can be executed with the SBE engine based on the canonical query.
 *
 * This method determines whether the query may be compatible with SBE based only on high-level
 * information from the canonical query, before query planning has taken place (such as ineligible
 * expressions or collections).
 *
 * If this method returns true, query planning should be done, followed by another layer of
 * validation to make sure the query plan can be executed with SBE. If it returns false, SBE query
 * planning can be short-circuited as it is already known that the query is ineligible for SBE.
 */

bool isQuerySbeCompatible(const CollectionPtr& collection, const CanonicalQuery& cq) {
    auto expCtx = cq.getExpCtxRaw();

    // If we don't support all expressions used or the query is eligible for IDHack, don't use SBE.
    if (!expCtx || expCtx->getSbeCompatibility() == SbeCompatibility::notCompatible ||
        expCtx->getSbePipelineCompatibility() == SbeCompatibility::notCompatible ||
        (collection && isIdHackEligibleQuery(collection, cq))) {
        return false;
    }

    const auto* proj = cq.getProj();
    if (proj && (proj->requiresMatchDetails() || proj->containsElemMatch())) {
        return false;
    }

    // Tailable and resumed scans are not supported either.
    if (expCtx->isTailable() || cq.getFindCommandRequest().getRequestResumeToken()) {
        return false;
    }

    const auto& nss = cq.nss();

    const auto isTimeseriesColl = collection && collection->isTimeseriesCollection();

    auto& queryKnob = cq.getExpCtx()->getQueryKnobConfiguration();
    if ((!feature_flags::gFeatureFlagTimeSeriesInSbe.isEnabled() ||
         queryKnob.getSbeDisableTimeSeriesForOp()) &&
        isTimeseriesColl) {
        return false;
    }

    // Queries against the oplog or a change collection are not supported. Also queries on the inner
    // side of a $lookup are not considered for SBE except search queries.
    if ((expCtx->getInLookup() && !cq.isSearchQuery()) || nss.isOplog() ||
        nss.isChangeCollection() || !cq.metadataDeps().none()) {
        return false;
    }


    // Queries against collections with a particular shape of compound hashed indexes are not
    // supported.
    if (collection && collectionHasIndexWithHashedPathPrefixOfNonHashedPath(collection, expCtx)) {
        return false;
    }

    // Find and aggregate queries with the $_startAt parameter are not supported in SBE.
    if (!cq.getFindCommandRequest().getStartAt().isEmpty()) {
        return false;
    }

    const auto& sortPattern = cq.getSortPattern();
    return !sortPattern || isSortSbeCompatible(*sortPattern);
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorFind(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    std::size_t plannerOptions,
    Pipeline* pipeline,
    bool needsMerge,
    boost::optional<TraversalPreference> traversalPreference,
    ExecShardFilterPolicy execShardFilterPolicy) {
    invariant(canonicalQuery);

    // Ensure that the shard filter option is set if this is a shard.
    if (OperationShardingState::isComingFromRouter(opCtx) &&
        std::holds_alternative<AutomaticShardFiltering>(execShardFilterPolicy)) {
        plannerOptions |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    }

    // Helper function for creating query planner parameters, with and without query settings. This
    // will be later used to ensure that queries can safely retry the planning process if the
    // application of the settings lead to a failure in generating the plan.
    auto makeQueryPlannerParams = [&](size_t options) -> std::unique_ptr<QueryPlannerParams> {
        return std::make_unique<QueryPlannerParams>(
            QueryPlannerParams::ArgsForSingleCollectionQuery{
                .opCtx = opCtx,
                .canonicalQuery = *canonicalQuery,
                .collections = collections,
                .plannerOptions = options,
                .traversalPreference = traversalPreference,
                .planRankerMode =
                    canonicalQuery->getExpCtx()->getQueryKnobConfiguration().getPlanRankerMode(),
            });
    };

    ON_BLOCK_EXIT([&] {
        // Stop the query planning timer once we have an execution plan.
        CurOp::get(opCtx)->stopQueryPlanningTimer();
    });

    // First try to use the express id point query fast path.
    const auto& mainColl = collections.getMainCollection();
    const auto expressEligibility = isExpressEligible(opCtx, mainColl, *canonicalQuery);
    if (expressEligibility == ExpressEligibility::IdPointQueryEligible) {
        planCacheCounters.incrementClassicSkippedCounter();
        auto plannerParams =
            std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForExpress{
                opCtx, *canonicalQuery, collections, plannerOptions});
        auto collectionFilter = getScopedCollectionFilter(opCtx, collections, *plannerParams);
        const bool isClusteredOnId = plannerParams->clusteredInfo
            ? clustered_util::isClusteredOnId(plannerParams->clusteredInfo)
            : false;

        auto expressExecutor = isClusteredOnId
            ? makeExpressExecutorForFindByClusteredId(
                  opCtx,
                  std::move(canonicalQuery),
                  collections.getMainCollectionPtrOrAcquisition(),
                  std::move(collectionFilter),
                  plannerOptions & QueryPlannerParams::RETURN_OWNED_DATA)
            : makeExpressExecutorForFindById(opCtx,
                                             std::move(canonicalQuery),
                                             collections.getMainCollectionPtrOrAcquisition(),
                                             std::move(collectionFilter),
                                             plannerOptions &
                                                 QueryPlannerParams::RETURN_OWNED_DATA);

        setCurOpQueryFramework(expressExecutor.get());
        return std::move(expressExecutor);
    }

    // The query might still be eligible for express execution via the index equality fast path.
    // However, that requires the full set of planner parameters for the main collection to be
    // available and creating those now allows them to be reused for subsequent strategies if
    // the express index equality one fails.
    auto paramsForSingleCollectionQuery = makeQueryPlannerParams(plannerOptions);
    if (expressEligibility == ExpressEligibility::IndexedEqualityEligible) {
        if (auto indexEntry =
                getIndexForExpressEquality(*canonicalQuery, *paramsForSingleCollectionQuery)) {
            auto expressExecutor = makeExpressExecutorForFindByUserIndex(
                opCtx,
                std::move(canonicalQuery),
                collections.getMainCollectionPtrOrAcquisition(),
                *indexEntry,
                getScopedCollectionFilter(opCtx, collections, *paramsForSingleCollectionQuery),
                plannerOptions & QueryPlannerParams::RETURN_OWNED_DATA);

            setCurOpQueryFramework(expressExecutor.get());
            return std::move(expressExecutor);
        }
    }

    const bool useSbeEngine = [&] {
        const bool forceClassic =
            canonicalQuery->getExpCtx()->getQueryKnobConfiguration().isForceClassicEngineEnabled();
        if (forceClassic || !isQuerySbeCompatible(mainColl, *canonicalQuery)) {
            return false;
        }

        // Add the stages that are candidates for SBE lowering from the 'pipeline' into the
        // 'canonicalQuery'. This must be done _before_ checking shouldUseRegularSbe() or
        // creating the planner.
        attachPipelineStages(collections, pipeline, needsMerge, canonicalQuery.get());

        const bool sbeFull = feature_flags::gFeatureFlagSbeFull.isEnabled();
        return sbeFull || shouldUseRegularSbe(opCtx, *canonicalQuery, mainColl, sbeFull);
    }();

    // If distinct multiplanning is enabled and we have a distinct property, we may not be able to
    // commit to SBE yet.
    auto canCommitToSbe = [&canonicalQuery]() {
        return !canonicalQuery->getExpCtx()->isFeatureFlagShardFilteringDistinctScanEnabled() ||
            !canonicalQuery->getDistinct();
    };

    canonicalQuery->setSbeCompatible(useSbeEngine);
    if (!useSbeEngine) {
        // There's a special case of the projection optimization being skipped when a query has
        // any user-defined "let" variable and the query may be run with SBE. Here we make sure
        // the projection is optimized for the classic engine.
        canonicalQuery->optimizeProjection();
    } else if (canCommitToSbe()) {
        // Commit to using SBE by removing the pushed-down aggregation stages from the original
        // pipeline and by mutating the canonical query with search specific metadata.
        finalizePipelineStages(pipeline, canonicalQuery.get());
    }


    auto makePlanner = [&](std::unique_ptr<QueryPlannerParams> plannerParams)
        -> std::unique_ptr<PlannerInterface> {
        // If we have a distinct, we might get a better plan using classic and DISTINCT_SCAN than
        // SBE without one.
        if (useSbeEngine && canCommitToSbe()) {
            auto sbeYieldPolicy =
                PlanYieldPolicySBE::make(opCtx, yieldPolicy, collections, canonicalQuery->nss());

            plannerParams->fillOutSecondaryCollectionsPlannerParams(
                opCtx, *canonicalQuery, collections);

            plannerParams->setTargetSbeStageBuilder(opCtx, *canonicalQuery, collections);

            if (shouldUseSbePlanCache(*plannerParams)) {
                canonicalQuery->setUsingSbePlanCache(true);
                return getClassicPlannerForSbe<
                    SbeWithClassicRuntimePlanningAndSbeCachePrepareExecutionHelper>(
                    opCtx,
                    collections,
                    canonicalQuery.get(),
                    yieldPolicy,
                    std::move(sbeYieldPolicy),
                    std::move(plannerParams));
            } else {
                canonicalQuery->setUsingSbePlanCache(false);
                return getClassicPlannerForSbe<
                    SbeWithClassicRuntimePlanningAndClassicCachePrepareExecutionHelper>(
                    opCtx,
                    collections,
                    canonicalQuery.get(),
                    yieldPolicy,
                    std::move(sbeYieldPolicy),
                    std::move(plannerParams));
            }
        }

        // This codepath will use the classic runtime planner with classic PlanStages, so will not
        // use the SBE plan cache.
        canonicalQuery->setUsingSbePlanCache(false);

        // Default to using the classic executor with the classic runtime planner.
        return getClassicPlanner(
            opCtx, collections, canonicalQuery.get(), yieldPolicy, std::move(plannerParams));
    };

    auto planner = [&] {
        static constexpr size_t kMaxIterations = 5;
        for (size_t iter = 0; iter < kMaxIterations; ++iter) {
            try {
                // First try the single collection query parameters, as these would have been
                // generated with query settings if present.
                return makePlanner(std::move(paramsForSingleCollectionQuery));
            } catch (const ExceptionFor<ErrorCodes::NoDistinctScansForDistinctEligibleQuery>&) {
                // The planner failed to generate a DISTINCT_SCAN for a distinct-like query. Remove
                // the distinct property and replan using SBE or subplanning as applicable.
                canonicalQuery->resetDistinct();
                if (canonicalQuery->isSbeCompatible()) {
                    // Stages still need to be finalized for SBE since classic was used previously.
                    finalizePipelineStages(pipeline, canonicalQuery.get());
                }
                return makePlanner(makeQueryPlannerParams(plannerOptions));
            } catch (const ExceptionFor<ErrorCodes::NoQueryExecutionPlans>& exception) {
                // The planner failed to generate a viable plan. Remove the query settings and
                // retry if any are present. Otherwise just propagate the exception.
                const auto& querySettings = canonicalQuery->getExpCtx()->getQuerySettings();
                const bool hasQuerySettings = querySettings.getIndexHints().has_value();
                // Planning has been tried without query settings and no execution plan was found.
                const bool ignoreQuerySettings =
                    plannerOptions & QueryPlannerParams::IGNORE_QUERY_SETTINGS;
                if (!hasQuerySettings || ignoreQuerySettings) {
                    throw;
                }
                LOGV2_DEBUG(
                    8524200,
                    2,
                    "Encountered planning error while running with query settings. Retrying "
                    "without query settings.",
                    "query"_attr = redact(canonicalQuery->toStringForErrorMsg()),
                    "querySettings"_attr = querySettings,
                    "reason"_attr = exception.reason(),
                    "code"_attr = exception.codeString());

                plannerOptions |= QueryPlannerParams::IGNORE_QUERY_SETTINGS;
                // Propagate the params to the next iteration.
                paramsForSingleCollectionQuery = makeQueryPlannerParams(plannerOptions);
            } catch (const ExceptionFor<ErrorCodes::RetryMultiPlanning>&) {
                // Propagate the params to the next iteration.
                paramsForSingleCollectionQuery = makeQueryPlannerParams(plannerOptions);
                canonicalQuery->getExpCtx()->setWasRateLimited(true);
            }
        }
        tasserted(8712800, "Exceeded retry iterations for making a planner");
    }();
    auto exec = planner->makeExecutor(std::move(canonicalQuery));
    setCurOpQueryFramework(exec.get());
    return std::move(exec);
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getSearchMetadataExecutorSBE(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ExpressionContext& expCtx,
    std::unique_ptr<executor::TaskExecutorCursor> metadataCursor) {
    // For metadata executor, we always have only one remote cursor, any id will work.
    const size_t metadataCursorId = 0;
    auto remoteCursors = std::make_unique<RemoteCursorMap>();
    remoteCursors->insert({metadataCursorId, std::move(metadataCursor)});

    MultipleCollectionAccessor emptyMca;
    auto sbeYieldPolicy =
        PlanYieldPolicySBE::make(opCtx, PlanYieldPolicy::YieldPolicy::YIELD_AUTO, emptyMca, nss);
    auto root = stage_builder::buildSearchMetadataExecutorSBE(
        opCtx, expCtx, metadataCursorId, remoteCursors.get(), sbeYieldPolicy.get());
    return plan_executor_factory::make(opCtx,
                                       nullptr /* cq */,
                                       nullptr /* solution */,
                                       std::move(root),
                                       emptyMca,
                                       {} /* plannerOptions */,
                                       nss,
                                       std::move(sbeYieldPolicy),
                                       false /* planIsFromCache */,
                                       boost::none /* cachedPlanHash */,
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
    OperationContext* opCtx = expCtx->getOperationContext();
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

    ON_BLOCK_EXIT([&] {
        // Stop the query planning timer once we have an execution plan.
        CurOp::get(opCtx)->stopQueryPlanningTimer();
    });

    if (!collectionPtr) {
        std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();

        // Treat collections that do not exist as empty collections. Return a PlanExecutor which
        // contains an EOF stage.
        LOGV2_DEBUG(20927,
                    2,
                    "Collection does not exist. Using EOF stage",
                    logAttrs(nss),
                    "query"_attr = redact(request->getQuery()));

        return plan_executor_factory::make(
            expCtx,
            std::move(ws),
            std::make_unique<EOFStage>(expCtx.get(), eof_node::EOFType::NonExistentNamespace),
            coll,
            parsedDelete->yieldPolicy(),
            false, /* whether we must return owned data */
            nss);
    }

    if (!parsedDelete->hasParsedQuery()) {

        // Only consider using the idhack if no hint was provided.
        if (request->getHint().isEmpty()) {
            // This is the idhack fast-path for getting a PlanExecutor without doing the work to
            // create a CanonicalQuery.
            const BSONObj& unparsedQuery = request->getQuery();

            bool hasIdIndex = collectionPtr->getIndexCatalog()->findIdIndex(opCtx) ||
                clustered_util::isClusteredOnId(collectionPtr->getClusteredInfo());

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

            if (hasIdIndex && isSimpleIdQuery(unparsedQuery) && request->getProj().isEmpty() &&
                hasCollectionDefaultCollation) {

                LOGV2_DEBUG(8376000, 2, "Using express", "query"_attr = redact(unparsedQuery));

                return makeExpressExecutorForDelete(opCtx, coll, parsedDelete);
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

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    const auto policy = parsedDelete->yieldPolicy();

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

    // Transfer the explain verbosity level into the expression context.
    cq->getExpCtx()->setExplain(verbosity);

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
    deleteStageParams->canonicalQuery = cq.get();

    MultipleCollectionAccessor collections{coll};
    auto plannerParams =
        std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForSingleCollectionQuery{
            .opCtx = opCtx,
            .canonicalQuery = *cq,
            .collections = collections,
            .planRankerMode = cq->getExpCtx()->getQueryKnobConfiguration().getPlanRankerMode(),
        });
    ClassicPrepareExecutionHelper helper{
        opCtx, collections, std::move(ws), cq.get(), policy, std::move(plannerParams)};

    ScopedDebugInfo queryPlannerParams(
        "queryPlannerParams",
        diagnostic_printers::QueryPlannerParamsPrinter{helper.getPlannerParams()});
    auto result = uassertStatusOK(helper.prepare());
    setOpDebugPlanCacheInfo(opCtx, result->planCacheInfo());
    result->runtimePlanner->addDeleteStage(
        parsedDelete, projection.get(), std::move(deleteStageParams));
    if (auto status = result->runtimePlanner->plan(); !status.isOK()) {
        return status;
    }
    return result->runtimePlanner->makeExecutor(std::move(cq));
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
    OperationContext* opCtx = expCtx->getOperationContext();

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

    ON_BLOCK_EXIT([&] {
        // Stop the query planning timer once we have an execution plan.
        CurOp::get(opCtx)->stopQueryPlanningTimer();
    });

    // If the collection doesn't exist, then return a PlanExecutor for a no-op EOF plan. We have
    // should have already enforced upstream that in this case either the upsert flag is false, or
    // we are an explain. If the collection doesn't exist, we're not an explain, and the upsert flag
    // is true, we expect the caller to have created the collection already.
    if (!coll.exists()) {
        std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
        LOGV2_DEBUG(20929,
                    2,
                    "Collection does not exist. Using EOF stage",
                    logAttrs(nss),
                    "query"_attr = redact(request->getQuery()));

        return plan_executor_factory::make(
            expCtx,
            std::move(ws),
            std::make_unique<EOFStage>(expCtx.get(), eof_node::EOFType::NonExistentNamespace),
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

            const bool hasCollectionDefaultCollation = CollatorInterface::collatorsMatch(
                expCtx->getCollator(), collectionPtr->getDefaultCollator());

            if (isSimpleIdQuery(unparsedQuery) && request->getProj().isEmpty() &&
                hasCollectionDefaultCollation) {

                const auto idIndexDesc = collectionPtr->getIndexCatalog()->findIdIndex(opCtx);
                if (!request->isUpsert() &&
                    (idIndexDesc ||
                     clustered_util::isClusteredOnId(collectionPtr->getClusteredInfo()))) {
                    // Upserts not supported in express for now.
                    LOGV2_DEBUG(83759, 2, "Using Express", "query"_attr = redact(unparsedQuery));

                    return makeExpressExecutorForUpdate(
                        opCtx, coll, parsedUpdate, false /* return owned BSON */);

                } else if (idIndexDesc) {
                    LOGV2_DEBUG(20930, 2, "Using idhack", "query"_attr = redact(unparsedQuery));
                    UpdateStageParams updateStageParams(
                        request, driver, opDebug, std::move(documentCounter));
                    BSONObj queryFilter = request->isUpsert()
                        ? getQueryFilterMaybeUnwrapEq(unparsedQuery)
                        : unparsedQuery;
                    fastPathQueryCounters.incrementIdHackQueryCounter();
                    return InternalPlanner::updateWithIdHack(
                        opCtx, coll, updateStageParams, idIndexDesc, queryFilter, policy);
                }
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
    UpdateStageParams updateStageParams(request, driver, opDebug, std::move(documentCounter));
    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    std::unique_ptr<CanonicalQuery> cq(parsedUpdate->releaseParsedQuery());

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
    updateStageParams.canonicalQuery = cq.get();

    MultipleCollectionAccessor collections{coll};
    ClassicPrepareExecutionHelper helper{
        opCtx,
        collections,
        std::move(ws),
        cq.get(),
        policy,
        std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForSingleCollectionQuery{
            .opCtx = opCtx,
            .canonicalQuery = *cq,
            .collections = collections,
            .planRankerMode = cq->getExpCtx()->getQueryKnobConfiguration().getPlanRankerMode(),
        })};

    ScopedDebugInfo queryPlannerParams(
        "queryPlannerParams",
        diagnostic_printers::QueryPlannerParamsPrinter{helper.getPlannerParams()});
    auto result = uassertStatusOK(helper.prepare());
    setOpDebugPlanCacheInfo(opCtx, result->planCacheInfo());
    result->runtimePlanner->addUpdateStage(
        parsedUpdate, projection.get(), std::move(updateStageParams));
    if (auto status = result->runtimePlanner->plan(); !status.isOK()) {
        return status;
    }
    return result->runtimePlanner->makeExecutor(std::move(cq));
}

//
// Count hack
//

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorCount(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const CollectionAcquisition& coll,
    std::unique_ptr<ParsedFindCommand> parsedFind,
    const CountCommandRequest& count) {
    OperationContext* opCtx = expCtx->getOperationContext();
    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();

    auto statusWithCQ = CanonicalQuery::make(
        {.expCtx = expCtx, .parsedFind = std::move(parsedFind), .isCountLike = true});
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }
    auto cq = std::move(statusWithCQ.getValue());

    const auto yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO;

    const auto skip = count.getSkip().value_or(0);
    const auto limit = count.getLimit().value_or(0);

    ON_BLOCK_EXIT([&] {
        // Stop the query planning timer once we have an execution plan.
        CurOp::get(opCtx)->stopQueryPlanningTimer();
    });

    if (!coll.exists()) {
        // Treat collections that do not exist as empty collections. Note that the explain reporting
        // machinery always assumes that the root stage for a count operation is a CountStage, so in
        // this case we put a CountStage on top of an EOFStage.
        std::unique_ptr<PlanStage> root = std::make_unique<CountStage>(
            expCtx.get(),
            limit,
            skip,
            ws.get(),
            new EOFStage(expCtx.get(), eof_node::EOFType::NonExistentNamespace));

        return plan_executor_factory::make(expCtx,
                                           std::move(ws),
                                           std::move(root),
                                           &CollectionPtr::null,
                                           yieldPolicy,
                                           false, /* whether we must return owned BSON */
                                           cq->getFindCommandRequest().getNamespaceOrUUID().nss());
    }

    // Can't encode plan cache key for non-existent collections. Add plan cache key information to
    // curOp here so both FastCountStage and multi-planner codepaths properly populate it.
    auto planCache = plan_cache_key_factory::make<PlanCacheKey>(*cq, coll.getCollectionPtr());
    setOpDebugPlanCacheInfo(opCtx,
                            PlanCacheInfo{.planCacheKey = planCache.planCacheKeyHash(),
                                          .planCacheShapeHash = planCache.planCacheShapeHash()});

    // If the query is empty, then we can determine the count by just asking the collection
    // for its number of records. This is implemented by the CountStage, and we don't need
    // to create a child for the count stage in this case.
    //
    // If there is a hint, then we can't use a trival count plan as described above.
    const bool isEmptyQueryPredicate =
        cq->getPrimaryMatchExpression()->matchType() == MatchExpression::AND &&
        cq->getPrimaryMatchExpression()->numChildren() == 0;
    const bool useRecordStoreCount =
        isEmptyQueryPredicate && cq->getFindCommandRequest().getHint().isEmpty();

    if (useRecordStoreCount) {
        std::unique_ptr<PlanStage> root =
            std::make_unique<RecordStoreFastCountStage>(expCtx.get(), coll, skip, limit);

        return plan_executor_factory::make(expCtx,
                                           std::move(ws),
                                           std::move(root),
                                           &CollectionPtr::null,
                                           yieldPolicy,
                                           false, /* whether we must returned owned BSON */
                                           cq->getFindCommandRequest().getNamespaceOrUUID().nss());
    }

    size_t plannerOptions = QueryPlannerParams::DEFAULT;
    if (OperationShardingState::isComingFromRouter(opCtx)) {
        plannerOptions |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    }

    MultipleCollectionAccessor collections{coll};
    auto plannerParams =
        std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForSingleCollectionQuery{
            .opCtx = opCtx,
            .canonicalQuery = *cq,
            .collections = collections,
            .plannerOptions = plannerOptions,
            .planRankerMode = cq->getExpCtx()->getQueryKnobConfiguration().getPlanRankerMode(),
        });
    ClassicPrepareExecutionHelper helper{
        opCtx, collections, std::move(ws), cq.get(), yieldPolicy, std::move(plannerParams)};

    ScopedDebugInfo queryPlannerParams(
        "queryPlannerParams",
        diagnostic_printers::QueryPlannerParamsPrinter{helper.getPlannerParams()});
    auto result = uassertStatusOK(helper.prepare());
    result->runtimePlanner->addCountStage(limit, skip);
    if (auto status = result->runtimePlanner->plan(); !status.isOK()) {
        return status;
    }
    return result->runtimePlanner->makeExecutor(std::move(cq));
}

StatusWith<std::unique_ptr<QuerySolution>> tryGetQuerySolutionForDistinct(
    const MultipleCollectionAccessor& collections,
    size_t plannerOptions,
    const CanonicalQuery& canonicalQuery,
    bool flipDistinctScanDirection) {
    tassert(9245500, "Expected distinct property on CanonicalQuery", canonicalQuery.getDistinct());

    const auto& collectionPtr = collections.getMainCollection();
    if (!collectionPtr) {
        // The caller should create EOF plan for the appropriate engine.
        return {ErrorCodes::NoQueryExecutionPlans, "No viable DISTINCT_SCAN plan"};
    }

    auto* opCtx = canonicalQuery.getExpCtx()->getOperationContext();

    auto getQuerySolution = [&](size_t options) -> std::unique_ptr<QuerySolution> {
        auto plannerParams =
            std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForDistinct{
                opCtx,
                canonicalQuery,
                collections,
                options,
                flipDistinctScanDirection,
            });

        // Can't create a DISTINCT_SCAN stage if no suitable indexes are present.
        if (plannerParams->mainCollectionInfo.indexes.empty()) {
            return nullptr;
        }
        return createDistinctScanSolution(
            canonicalQuery, *plannerParams, flipDistinctScanDirection);
    };
    auto soln = getQuerySolution(plannerOptions);
    if (!soln) {
        // Try again this time without query settings applied.
        soln = getQuerySolution(plannerOptions | QueryPlannerParams::IGNORE_QUERY_SETTINGS);
    }
    if (!soln) {
        return {ErrorCodes::NoQueryExecutionPlans, "No viable DISTINCT_SCAN plan"};
    }

    return {std::move(soln)};
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorDistinct(
    const MultipleCollectionAccessor& collections,
    size_t plannerOptions,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    std::unique_ptr<QuerySolution> soln) {
    tassert(9245501, "Expected distinct property on CanonicalQuery", canonicalQuery->getDistinct());

    const auto& collectionPtr = collections.getMainCollection();
    auto* opCtx = canonicalQuery->getExpCtx()->getOperationContext();
    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    auto collPtrOrAcq = collections.getMainCollectionPtrOrAcquisition();
    auto&& root = stage_builder::buildClassicExecutableTree(
        opCtx, collPtrOrAcq, *canonicalQuery, *soln, ws.get());

    LOGV2_DEBUG(
        20932, 2, "Using fast distinct", "query"_attr = redact(canonicalQuery->toStringShort()));

    // Stop the query planning timer once we have an execution plan.
    CurOp::get(opCtx)->stopQueryPlanningTimer();

    return plan_executor_factory::make(std::move(canonicalQuery),
                                       std::move(ws),
                                       std::move(root),
                                       collPtrOrAcq,
                                       PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                       plannerOptions,
                                       collectionPtr->ns(),
                                       std::move(soln));
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getCollectionScanExecutor(
    OperationContext* opCtx,
    const CollectionAcquisition& collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    CollectionScanDirection scanDirection,
    const boost::optional<RecordId>& resumeAfterRecordId) {
    auto isForward = scanDirection == CollectionScanDirection::kForward;
    auto direction = isForward ? InternalPlanner::FORWARD : InternalPlanner::BACKWARD;
    return InternalPlanner::collectionScan(
        opCtx, collection, yieldPolicy, direction, resumeAfterRecordId);
}

}  // namespace mongo
