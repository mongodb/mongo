// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/classic_runtime_planner_for_sbe_test_util.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/planner_interface.h"
#include "mongo/db/exec/runtime_planners/exec_deferred_engine_choice_runtime_planner/planner_interface.h"
#include "mongo/db/exec/runtime_planners/planner_types.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_projection.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/get_executor_deferred_engine_choice_lowering.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_ranking/plan_selection_strategy.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/replanning_required_info.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/unittest/server_parameter_guard.h"

#include <variant>

#include <boost/optional/optional.hpp>

namespace mongo::classic_runtime_planner_for_sbe {
namespace {

namespace deferred_exec = exec_deferred_engine_choice;

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.collection");

const BSONObj kFindFilter = fromjson("{a: {$gte: 0}, b: {$gte: 0}}");
const BSONObj kAddFieldsSpec = fromjson(R"({sum: {$add: ["$a", "$b"]}})");
const BSONObj kGroupSpec = fromjson(R"({$group: {_id: "$b", total: {$sum: "$a"}}})");

/**
 * Fixture for the classic runtime planner for SBE. As a test query, it uses an aggregation
 * pipeline [{$match: <match statement>}, {$addFields: <addFields statement>]
 * where $match is pushed down to the find query and $addFields is left to be a single-stage agg
 * pipeline.
 */
class DeferredEngineChoiceRuntimePlannerTest : public CatalogTestFixture {
protected:
    void setUp() final {
        CatalogTestFixture::setUp();
        OperationContext* opCtx = operationContext();
        expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);

        ASSERT_OK(storageInterface()->createCollection(opCtx, kNss, CollectionOptions{}));
    }

    void tearDown() final {
        _collections.reset();
        expCtx.reset();
        CatalogTestFixture::tearDown();
    }

    const MultipleCollectionAccessor& collections() const {
        return *_collections;
    }

    /**
     * Creates a pair of CanonicalQuery & PlannerData for the pipeline
     * [{$match: 'findFilter'}, ...'pipeline'], where 'pipeline' is pushed down to SBE.
     */
    auto createPlannerDataForPipeline(std::vector<boost::intrusive_ptr<DocumentSource>> pipeline,
                                      BSONObj findFilter = kFindFilter) {
        auto findCommand = std::make_unique<FindCommandRequest>(kNss);
        findCommand->setFilter(findFilter);

        auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = expCtx,
            .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)},
            .pipeline = std::move(pipeline)});
        cq->setSbeCompatible(true);

        auto params = std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForTest{});
        PlannerData plannerData{operationContext(),
                                cq.get(),
                                std::make_unique<WorkingSet>(),
                                *_collections,
                                std::move(params),
                                PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                /* cachedPlanHash */ boost::none};
        return std::make_pair(std::move(cq), std::move(plannerData));
    }

    /**
     * Creates a pair of CanonicalQuery & PlannerData for the following pipeline: [
     *   {$match: 'findFilter'}},
     *   {$addFields: 'addFieldsSpec'}}
     * ]
     * Defaults to kFindFilter and kAddFieldsSpec for 'findFilter' and 'addFieldsSpec' respectively
     * if arguments are not supplied. If addFieldsSpec is empty, $addFields stage won't be added.
     */
    auto createPlannerData(BSONObj findFilter = kFindFilter,
                           BSONObj addFieldsSpec = kAddFieldsSpec) {
        std::vector<boost::intrusive_ptr<DocumentSource>> pipeline;
        if (!addFieldsSpec.isEmpty()) {
            pipeline.emplace_back(make_intrusive<DocumentSourceInternalProjection>(
                expCtx, kAddFieldsSpec, InternalProjectionPolicyEnum::kAddFields));
        }
        return createPlannerDataForPipeline(std::move(pipeline), findFilter);
    }

    /**
     * Returns the 'expectedDocCount' documents that 'exec' produces before EOF. They are read with
     * getNextDocument() if 'readDocuments' is set, and with getNext() otherwise.
     */
    std::vector<BSONObj> getResultDocumentsAndAssertExecState(size_t expectedDocCount,
                                                              PlanExecutor* exec,
                                                              bool readDocuments = false) {
        std::vector<BSONObj> result;
        result.reserve(expectedDocCount);
        for (size_t i = 0; i < expectedDocCount; ++i) {
            BSONObj out;
            if (readDocuments) {
                Document doc;
                ASSERT_EQ(PlanExecutor::ExecState::ADVANCED, exec->getNextDocument(doc));
                out = doc.toBson();
            } else {
                ASSERT_EQ(PlanExecutor::ExecState::ADVANCED, exec->getNext(&out, nullptr));
            }
            result.push_back(out.getOwned());
        }
        Document doc;
        ASSERT_EQ(PlanExecutor::ExecState::IS_EOF, exec->getNextDocument(doc));
        return result;
    }

    /**
     * For a given PlanExecutor, asserts the following:
     * 1. PlanExecutor returns EOF after exactly expectedSums.size() documents.
     * 2. Each returned document have a field "sum" with an integer value, equal to corresponding
     *    element in expectedSums.
     */
    void assertPlanExecutorReturnsCorrectSums(std::vector<int> expectedSums,
                                              PlanExecutor* exec,
                                              bool readDocuments = false) {
        size_t docCount = expectedSums.size();
        auto result = getResultDocumentsAndAssertExecState(docCount, exec, readDocuments);
        for (size_t i = 0; i < docCount; ++i) {
            ASSERT_EQ(expectedSums[i], result[i].getField("sum").Int());
        }
    }

    void acquireCollectionForRead() {
        auto opCtx = operationContext();
        _collections.emplace(acquireCollectionMaybeLockFree(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kRead)));
    }

    std::pair<std::vector<std::unique_ptr<QuerySolution>>, std::vector<int>>
    createVirtualScanQuerySolutionsForDefaultFilter(int resultDocCount, const CanonicalQuery* cq) {
        std::vector<std::unique_ptr<QuerySolution>> solutions;
        std::vector<int> expectedSums;
        expectedSums.reserve(resultDocCount);

        auto addVirtualScanSolution = [&](std::vector<BSONArray> docs) {
            auto virtScan = std::make_unique<VirtualScanNode>(
                std::move(docs), VirtualScanNode::ScanType::kCollScan, /*hasRecordId=*/false);
            virtScan->filter = cq->getPrimaryMatchExpression()->clone();
            auto solution = std::make_unique<QuerySolution>();
            solution->cacheData = std::make_unique<SolutionCacheData>();
            solution->cacheData->virtualScanData = std::make_unique<VirtualScanCacheData>(
                virtScan->docs, /*hasRecordId=*/false, /*indexKeyPattern=*/BSONObj());
            solution->setRoot(std::move(virtScan));
            solution->cacheData->solnType = SolutionCacheData::VIRTSCAN_SOLN;
            solutions.push_back(std::move(solution));
        };

        {  // Generate a plan with every second document matching the filter. We
           // expect the multi-planner to choose this plan.
            std::vector<BSONArray> docs;
            docs.reserve(2 * resultDocCount);
            for (int i = 1; i <= resultDocCount; ++i) {
                docs.push_back(BSON_ARRAY(BSON("a" << i << "b" << 1)));
                docs.push_back(BSON_ARRAY(BSON("a" << -i << "b" << 1)));
                expectedSums.push_back(i + 1);
            }
            addVirtualScanSolution(std::move(docs));
        }
        {  // Generate a plan with every third document matching the filter.
            std::vector<BSONArray> docs;
            docs.reserve(3 * resultDocCount);
            for (int i = 1; i <= resultDocCount; ++i) {
                docs.push_back(BSON_ARRAY(BSON("a" << i << "b" << 2)));
                docs.push_back(BSON_ARRAY(BSON("a" << -i << "b" << 2)));
                docs.push_back(BSON_ARRAY(BSON("a" << -i << "b" << 2)));
            }
            addVirtualScanSolution(std::move(docs));
        }
        return {std::move(solutions), std::move(expectedSums)};
    }

    /**
     * Returns a PlannerGenerator for a classic cache entry, using the given canonical query and
     * planner data. The PlannerGenerator can then be used to make a runtime planner. The caller
     * may also swap the SBE plan created by the PlannerGenerator, in order to simulate a plan that
     * has other behavior.
     *
     * The 'numReads' argument is used to override the 'numReads' value stored in the cache entry,
     * as this will always be 0 when a VirtualScan plan is used.
     *
     * 'hasDocumentSourceSuffix' tells the planner whether DocumentSources follow the SBE plan.
     */
    std::pair<PlanCacheKey, std::unique_ptr<PlannerGeneratorFromClassicCacheEntry>>
    makePlannerGeneratorFromClassicCache(const CanonicalQuery* cq,
                                         PlannerData plannerData,
                                         NumReads numReads,
                                         bool hasDocumentSourceSuffix = false) {
        auto planCacheKey = plan_cache_key_factory::make<PlanCacheKey>(
            *cq, plannerData.collections.getMainCollectionAcquisition());

        auto cacheEntry = getClassicPlanCache().getCacheEntryIfActive(planCacheKey);
        ASSERT_TRUE(static_cast<bool>(cacheEntry));

        invariant(cacheEntry->cachedPlan->virtualScanData);
        auto statusWithQs =
            QueryPlanner::planFromCache(*cq, *plannerData.plannerParams, *cacheEntry->cachedPlan);
        ASSERT(statusWithQs.isOK());
        auto querySolution = std::move(statusWithQs.getValue());

        querySolution = QueryPlanner::extendWithAggPipeline(
            *plannerData.cq,
            std::move(querySolution),
            plannerData.plannerParams->secondaryCollectionsInfo);

        auto plannerParams = plannerData.plannerParams;
        PlannerDataForSBE plannerDataForSbe{std::move(plannerData),
                                            *_collections,
                                            std::move(plannerParams),
                                            makeYieldPolicy(),
                                            hasDocumentSourceSuffix};
        plannerDataForSbe.cachedPlanSolutionHash = cacheEntry->cachedPlan->solutionHash;

        return {planCacheKey,
                std::make_unique<PlannerGeneratorFromClassicCacheEntry>(
                    std::move(plannerDataForSbe), std::move(querySolution), numReads.value)};
    }

    /**
     * Runs the trial of the plan cached for 'cq' and returns the executor that resumes the plan.
     * Asserts that the trial stashed 'expectedStashSize' results (at least one if boost::none), as
     * Documents if 'hasDocumentSourceSuffix' and as BSON otherwise.
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutorFromCachedPlan(
        std::unique_ptr<CanonicalQuery> cq,
        PlannerData plannerData,
        bool hasDocumentSourceSuffix,
        boost::optional<size_t> expectedStashSize = boost::none) {
        auto [classicCacheKey, plannerGenerator] = makePlannerGeneratorFromClassicCache(
            cq.get(), std::move(plannerData), NumReads{10000}, hasDocumentSourceSuffix);
        auto rankingResult = plannerGenerator->makePlanner()->extractPlanRankingResult();

        ASSERT(rankingResult.execState);
        const auto* execState = rankingResult.execState->peekExecState<SbeExecState>();
        ASSERT(execState);
        const auto& stash = execState->sbeCandidate.results;
        if (expectedStashSize) {
            ASSERT_EQ(stash.size(), *expectedStashSize);
        } else {
            ASSERT_FALSE(stash.empty());
        }
        for (const auto& [result, recordId] : stash) {
            ASSERT_EQ(std::holds_alternative<Document>(result), hasDocumentSourceSuffix);
        }

        // Lowering expects the stages pushed down to the CanonicalQuery to come from a Pipeline.
        auto pipeline = Pipeline::create(
            DocumentSourceContainer(cq->cqPipeline().begin(), cq->cqPipeline().end()), expCtx);
        return deferred_exec::lowerPlanRankingResult(std::move(cq),
                                                     std::move(rankingResult),
                                                     operationContext(),
                                                     collections(),
                                                     PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                     pipeline.get());
    }


    /**
     * Inserts 200 documents with {a: i, b: 1} which is the same as the outputs from the
     * VirtualScanNode from createVirtualScanQuerySolutionsForDefaultFilter(). Also responsible for
     * acquiring the test collection.
     */
    void setUpCachedPlannerTest() {
        auto opCtx = operationContext();
        std::vector<InsertStatement> docs;
        for (int i = 1; i <= 200; ++i) {
            docs.emplace_back(InsertStatement(BSON("_id" << OID::gen() << "a" << i << "b" << 1)));
        }

        ASSERT_OK(storageInterface()->insertDocuments(opCtx, kNss, docs));

        acquireCollectionForRead();
    }

    PlanCache& getClassicPlanCache() {
        auto&& collQueryInfo = CollectionQueryInfo::get(_collections->getMainCollection());
        auto cache = collQueryInfo.getPlanCache();
        ASSERT(cache);
        return *cache;
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;

private:
    std::unique_ptr<PlanYieldPolicySBE> makeYieldPolicy() {
        return PlanYieldPolicySBE::make(
            operationContext(),
            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
            operationContext()->getServiceContext()->getFastClockSource(),
            0,
            Milliseconds::zero());
    }

    boost::optional<MultipleCollectionAccessor> _collections;
};

TEST_F(DeferredEngineChoiceRuntimePlannerTest, MultiPlannerPicksMoreEfficientPlan) {
    acquireCollectionForRead();

    // Ensures that cache entries are available immediately.
    bool previousQueryKnobValue = internalQueryCacheDisableInactiveEntries.swap(true);
    ON_BLOCK_EXIT([&] { internalQueryCacheDisableInactiveEntries.store(previousQueryKnobValue); });

    auto [cq, plannerData] = createPlannerData();
    auto [solutions, expectedSums] =
        createVirtualScanQuerySolutionsForDefaultFilter(/*resultDocCount=*/200, plannerData.cq);
    {  // The multi-planner writes the winning plan to the classic cache.
        deferred_exec::MultiPlanner planner{std::move(plannerData),
                                            std::move(solutions),
                                            /*addingCBRChosenPlanToPlanCache=*/false,
                                            /*maybeExplainData=*/boost::none,
                                            PlanSelectionStrategy::kMultiPlanner};
    }

    {  // Run CachedPlanner to execute the cached plan.
        auto [cq, plannerData] = createPlannerData();
        auto [classicCacheKey, plannerGenerator] =
            makePlannerGeneratorFromClassicCache(cq.get(), std::move(plannerData), NumReads{10000});
        auto rankingResult = plannerGenerator->makePlanner()->extractPlanRankingResult();

        // Lowering expects the stages pushed down to the CanonicalQuery to come from a Pipeline.
        auto pipeline = Pipeline::create(
            DocumentSourceContainer(cq->cqPipeline().begin(), cq->cqPipeline().end()), expCtx);
        auto cachedExec =
            deferred_exec::lowerPlanRankingResult(std::move(cq),
                                                  std::move(rankingResult),
                                                  operationContext(),
                                                  collections(),
                                                  PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                  pipeline.get());
        assertPlanExecutorReturnsCorrectSums(std::move(expectedSums), cachedExec.get());
    }
}

TEST_F(DeferredEngineChoiceRuntimePlannerTest,
       ClassicCachedPlannerReplansOnFailureMemoryLimitExceeded) {
    // Ensures that cache entries are available immediately.
    bool previousQueryKnobValue = internalQueryCacheDisableInactiveEntries.swap(true);
    ON_BLOCK_EXIT([&] { internalQueryCacheDisableInactiveEntries.store(previousQueryKnobValue); });

    setUpCachedPlannerTest();

    auto [cqForCacheWrite, plannerDataForCacheWrite] = createPlannerData();
    auto [solutions, expectedSums] = createVirtualScanQuerySolutionsForDefaultFilter(
        /*resultDocCount=*/200, plannerDataForCacheWrite.cq);
    {  // The multi-planner writes the winning plan to the classic cache.
        deferred_exec::MultiPlanner planner{std::move(plannerDataForCacheWrite),
                                            std::move(solutions),
                                            /*addingCBRChosenPlanToPlanCache=*/false,
                                            /*maybeExplainData=*/boost::none,
                                            PlanSelectionStrategy::kMultiPlanner};
    }

    {
        auto [cq, plannerData] = createPlannerData();
        auto [classicCacheKey, plannerGenerator] =
            makePlannerGeneratorFromClassicCache(cq.get(), std::move(plannerData), NumReads{10000});

        {
            // Swap in a mock SBE plan.
            auto planStageData = stage_builder::PlanStageData(
                stage_builder::Environment(std::make_unique<sbe::RuntimeEnvironment>()),
                std::make_shared<stage_builder::PlanStageStaticData>());
            plannerGenerator->setSbePlan_forTest(
                std::make_unique<sbe::MockExceededMemoryLimitStage>(0), std::move(planStageData));
        }

        // With the deferred executor, the cached planner signals replanning by throwing
        // ReplanningRequired rather than replanning inline.
        ASSERT_THROWS_WITH_CHECK(
            plannerGenerator->makePlanner(),
            ExceptionFor<ErrorCodes::ReplanningRequired>,
            ([](const ExceptionFor<ErrorCodes::ReplanningRequired>& ex) {
                ASSERT_STRING_SEARCH_REGEX(ex.reason(), "cached plan returned: ");
                ASSERT_EQ(ex.extraInfo<ReplanningRequiredInfo>()->getCacheMode(),
                          plan_cache_util::CacheMode::NeverCache);
            }));
        ASSERT(getClassicPlanCache().getCacheEntryIfActive(classicCacheKey));
    }
}

TEST_F(DeferredEngineChoiceRuntimePlannerTest, ClassicCachedPlannerReplansOnHittingMaxNumReads) {
    // Ensures that cache entries are available immediately.
    bool previousQueryKnobValue = internalQueryCacheDisableInactiveEntries.swap(true);
    ON_BLOCK_EXIT([&] { internalQueryCacheDisableInactiveEntries.store(previousQueryKnobValue); });

    setUpCachedPlannerTest();

    auto [cq, plannerData] = createPlannerData();
    auto [solutions, expectedSums] =
        createVirtualScanQuerySolutionsForDefaultFilter(/*resultDocCount=*/200, plannerData.cq);
    {  // The multi-planner writes the winning plan to the classic cache.
        deferred_exec::MultiPlanner planner{std::move(plannerData),
                                            std::move(solutions),
                                            /*addingCBRChosenPlanToPlanCache=*/false,
                                            /*maybeExplainData=*/boost::none,
                                            PlanSelectionStrategy::kMultiPlanner};
    }

    {  // Run CachedPlanner to execute the cached plan.
        auto [cq, plannerData] = createPlannerData();
        auto [classicCacheKey, plannerGenerator] =
            makePlannerGeneratorFromClassicCache(cq.get(),
                                                 std::move(plannerData),
                                                 // We give a threshold of 1 read, which will
                                                 // always be exceeded by the mock stage.
                                                 NumReads{1});

        {
            // Swap in a mock SBE plan that always exceeds the number of reads.
            auto staticData = std::make_shared<stage_builder::PlanStageStaticData>();
            staticData->resultSlot = sbe::value::SlotId{0};
            auto planStageData = stage_builder::PlanStageData(
                stage_builder::Environment(std::make_unique<sbe::RuntimeEnvironment>()),
                staticData);
            plannerGenerator->setSbePlan_forTest(
                std::make_unique<sbe::MockExceededMaxReadsStage>(0), std::move(planStageData));
        }

        // With the deferred executor the cached planner deactivates the cache entry and then
        // signals replanning via ReplanningRequired (re-caching happens at the retry level).
        ASSERT_THROWS_WITH_CHECK(
            plannerGenerator->makePlanner(),
            ExceptionFor<ErrorCodes::ReplanningRequired>,
            ([](const ExceptionFor<ErrorCodes::ReplanningRequired>& ex) {
                ASSERT_STRING_SEARCH_REGEX(ex.reason(),
                                           "cached plan was less efficient than expected");
                ASSERT_EQ(ex.extraInfo<ReplanningRequiredInfo>()->getCacheMode(),
                          plan_cache_util::CacheMode::AlwaysCache);
            }));
        ASSERT_TRUE(getClassicPlanCache().getCacheEntryIfActive(classicCacheKey));
    }
}

// The trial of a cached plan stashes its results as Documents when a pipeline follows the SBE plan,
// and as BSON otherwise. The executor returns the stashed results through both getNext() and
// getNextDocument(), whichever format they were stashed in.
TEST_F(DeferredEngineChoiceRuntimePlannerTest, CachedPlannerStashesTrialResultsInReadFormat) {
    acquireCollectionForRead();

    // Ensures that cache entries are available immediately.
    bool previousQueryKnobValue = internalQueryCacheDisableInactiveEntries.swap(true);
    ON_BLOCK_EXIT([&] { internalQueryCacheDisableInactiveEntries.store(previousQueryKnobValue); });

    auto [cq, plannerData] = createPlannerData();
    auto [solutions, expectedSums] =
        createVirtualScanQuerySolutionsForDefaultFilter(/*resultDocCount=*/200, plannerData.cq);
    {  // The multi-planner writes the winning plan to the classic cache.
        deferred_exec::MultiPlanner planner{std::move(plannerData),
                                            std::move(solutions),
                                            /*addingCBRChosenPlanToPlanCache=*/false,
                                            /*maybeExplainData=*/boost::none,
                                            PlanSelectionStrategy::kMultiPlanner};
    }

    for (bool hasDocumentSourceSuffix : {false, true}) {
        for (bool readDocuments : {false, true}) {
            auto [cq, plannerData] = createPlannerData();
            auto exec = makeExecutorFromCachedPlan(
                std::move(cq), std::move(plannerData), hasDocumentSourceSuffix);
            assertPlanExecutorReturnsCorrectSums(expectedSums, exec.get(), readDocuments);
        }
    }
}

// Same as above, but for a $group plan whose results the executor builds from the plan's slots
// instead of the plan materializing them.
TEST_F(DeferredEngineChoiceRuntimePlannerTest, CachedPlannerStashesGroupTrialResultsInReadFormat) {
    unittest::ServerParameterGuard efficientBoundaryMaterialization{
        "featureFlagSbeEfficientBoundaryMaterialization", true};
    acquireCollectionForRead();

    // Ensures that cache entries are available immediately.
    bool previousQueryKnobValue = internalQueryCacheDisableInactiveEntries.swap(true);
    ON_BLOCK_EXIT([&] { internalQueryCacheDisableInactiveEntries.store(previousQueryKnobValue); });

    auto createGroupPlannerData = [&] {
        return createPlannerDataForPipeline(
            {DocumentSourceGroup::createFromBson(kGroupSpec.firstElement(), expCtx)});
    };

    auto [cq, plannerData] = createGroupPlannerData();
    auto [solutions, expectedSums] =
        createVirtualScanQuerySolutionsForDefaultFilter(/*resultDocCount=*/200, plannerData.cq);
    {  // The multi-planner writes the winning plan to the classic cache.
        deferred_exec::MultiPlanner planner{std::move(plannerData),
                                            std::move(solutions),
                                            /*addingCBRChosenPlanToPlanCache=*/false,
                                            /*maybeExplainData=*/boost::none,
                                            PlanSelectionStrategy::kMultiPlanner};
    }

    // The winning plan returns {a: i, b: 1} for i in [1, 200], which is a single group.
    const auto expectedGroup = BSON("_id" << 1 << "total" << 20100);
    for (bool hasDocumentSourceSuffix : {false, true}) {
        for (bool readDocuments : {false, true}) {
            auto [cq, plannerData] = createGroupPlannerData();
            // The $group returns its first result once the trial has read all of its input.
            auto exec = makeExecutorFromCachedPlan(std::move(cq),
                                                   std::move(plannerData),
                                                   hasDocumentSourceSuffix,
                                                   /*expectedStashSize=*/1);
            auto results = getResultDocumentsAndAssertExecState(
                /*expectedDocCount=*/1, exec.get(), readDocuments);
            ASSERT_BSONOBJ_EQ(results[0], expectedGroup);
        }
    }
}

}  // namespace
}  // namespace mongo::classic_runtime_planner_for_sbe
