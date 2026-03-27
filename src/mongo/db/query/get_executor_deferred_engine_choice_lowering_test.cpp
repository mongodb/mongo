/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/get_executor_deferred_engine_choice_lowering.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/classic/idhack.h"
#include "mongo/db/exec/runtime_planners/exec_deferred_engine_choice_runtime_planner/planner_interface.h"
#include "mongo/db/exec/runtime_planners/planner_types.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/plan_executor_sbe.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"

namespace mongo::exec_deferred_engine_choice {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.collection");
const BSONObj kFindFilter = fromjson("{a: {$gte: 0}, b: {$gte: 0}}");
const BSONObj kIdHackFilter = fromjson("{_id: 1}");
const BSONObj kGroupStage = fromjson(R"({$group: {_id: null, foo: {$count: {}}}})");

std::unique_ptr<QuerySolution> makeVirtualScan(std::vector<BSONArray> docs) {
    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::make_unique<VirtualScanNode>(
        std::move(docs), VirtualScanNode::ScanType::kCollScan, false /*hasRecordId*/));
    return solution;
}

std::vector<std::unique_ptr<QuerySolution>> makeEmptyVirtualScan() {
    std::vector<std::unique_ptr<QuerySolution>> solutions;
    solutions.push_back(makeVirtualScan({}));
    return solutions;
}

/**
 * For `featureFlagGetExecutorDeferredEngineChoice` on, tests the behavior of the lowering
 * machinery.
 */
class DeferredEngineChoiceLoweringTest : public CatalogTestFixture {
protected:
    void setUp() final {
        CatalogTestFixture::setUp();
        OperationContext* opCtx = operationContext();
        _expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);
        _expCtx->setMongoProcessInterface(std::make_shared<StandaloneProcessInterface>(nullptr));

        ASSERT_OK(storageInterface()->createCollection(opCtx, kNss, CollectionOptions{}));
        acquireCollectionForRead();
    }

    void tearDown() final {
        _collections.reset();
        _expCtx.reset();
        CatalogTestFixture::tearDown();
    }

    boost::intrusive_ptr<ExpressionContext> expCtx() {
        return _expCtx;
    }

    MultipleCollectionAccessor& collections() {
        return *_collections;
    }

    void acquireCollectionForRead() {
        auto opCtx = operationContext();
        _collections.emplace(acquireCollectionMaybeLockFree(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kRead)));
    }

    std::unique_ptr<Pipeline> makeSbeEligiblePipeline() {
        return pipeline_factory::makePipeline(
            {kGroupStage}, expCtx(), {.attachCursorSource = false});
    }

    // Creates a CanonicalQuery and PlannerData for the test.
    auto createPlannerData(boost::optional<BSONObj> findFilter = boost::none) {
        auto findCommand = std::make_unique<FindCommandRequest>(kNss);
        if (findFilter) {
            findCommand->setFilter(*findFilter);
        }
        auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = expCtx(),
            .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});
        cq->setSbeCompatible(true);

        PlannerData plannerData{
            operationContext(),
            cq.get(),
            std::make_unique<WorkingSet>(),
            *_collections,
            std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForTest{}),
            PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
            boost::none /* cachedPlanHash */};
        return std::make_pair(std::move(cq), std::move(plannerData));
    }

    // Return two virtual scans with the given filter. Both return N=`resultDocCount` documents.
    std::vector<std::unique_ptr<QuerySolution>> createVirtualScanQuerySolutionsForDefaultFilter(
        int resultDocCount, const MatchExpression* filter) {
        std::vector<std::unique_ptr<QuerySolution>> solutions;
        auto addVirtualScanSolution = [&](std::vector<BSONArray> docs) {
            auto virtScan = makeVirtualScan(std::move(docs));
            virtScan->root()->filter = filter->clone();
            solutions.push_back(std::move(virtScan));
        };

        {  // Generate a plan with every second document matching the filter. We
           // expect the multi-planner to choose this plan.
            std::vector<BSONArray> docs;
            for (int i = 1; i <= resultDocCount; ++i) {
                docs.push_back(BSON_ARRAY(BSON("a" << i << "b" << 1)));
                docs.push_back(BSON_ARRAY(BSON("a" << -i << "b" << 1)));
            }
            addVirtualScanSolution(std::move(docs));
        }
        {  // Generate a plan with every third document matching the filter.
            std::vector<BSONArray> docs;
            for (int i = 1; i <= resultDocCount; ++i) {
                docs.push_back(BSON_ARRAY(BSON("a" << i << "b" << 2)));
                docs.push_back(BSON_ARRAY(BSON("a" << -i << "b" << 2)));
                docs.push_back(BSON_ARRAY(BSON("a" << -i << "b" << 2)));
            }
            addVirtualScanSolution(std::move(docs));
        }
        return solutions;
    }

private:
    RAIIServerParameterControllerForTest sbeFullController{
        "featureFlagGetExecutorDeferredEngineChoice", true};

    boost::intrusive_ptr<ExpressionContext> _expCtx;
    boost::optional<MultipleCollectionAccessor> _collections;
};


TEST_F(DeferredEngineChoiceLoweringTest, IdhackUsesClassic) {
    auto [cq, plannerData] = createPlannerData(kIdHackFilter);

    // Create the IdHack plan stage.
    auto idIndex = collections()
                       .getMainCollectionPtrOrAcquisition()
                       .getCollectionPtr()
                       ->getIndexCatalog()
                       ->findIdIndex(operationContext());
    auto idHackStage =
        std::make_unique<IDHackStage>(expCtx().get(),
                                      cq.get(),
                                      plannerData.workingSet.get(),
                                      collections().getMainCollectionPtrOrAcquisition(),
                                      idIndex);

    // Mark that IdHack was used, and provide the existing exec state.
    PlanRankingResult rankingResult{
        .usedIdhack = true,
        .execState = SavedExecState{ClassicExecState{
            .workingSet = std::move(plannerData.workingSet), .root = std::move(idHackStage)}},
        .plannerParams = std::move(plannerData.plannerParams)};
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec =
        lowerPlanRankingResult(std::move(cq),
                               std::move(rankingResult),
                               operationContext(),
                               collections(),
                               PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                               nullptr /*pipeline*/);
    ASSERT(dynamic_cast<PlanExecutorImpl*>(exec.get()));
}

// A basic scan with no pipeline or filter should use classic.
TEST_F(DeferredEngineChoiceLoweringTest, BasicScanUsesClassic) {
    auto [cq, plannerData] = createPlannerData();
    PlanRankingResult rankingResult{.solutions = makeEmptyVirtualScan(),
                                    .plannerParams = std::move(plannerData.plannerParams)};
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec =
        lowerPlanRankingResult(std::move(cq),
                               std::move(rankingResult),
                               operationContext(),
                               collections(),
                               PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                               nullptr /*pipeline*/);
    ASSERT(dynamic_cast<PlanExecutorImpl*>(exec.get()));
}

// A query with an SBE-eligible pipeline should use SBE.
TEST_F(DeferredEngineChoiceLoweringTest, GroupQueryUsesSbe) {
    auto [cq, plannerData] = createPlannerData();
    PlanRankingResult rankingResult{.solutions = makeEmptyVirtualScan(),
                                    .plannerParams = std::move(plannerData.plannerParams)};
    auto pipeline = makeSbeEligiblePipeline();
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec =
        lowerPlanRankingResult(std::move(cq),
                               std::move(rankingResult),
                               operationContext(),
                               collections(),
                               PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                               pipeline.get());
    ASSERT(dynamic_cast<PlanExecutorSBE*>(exec.get()));
}

// After multiplanning is complete, if a plan has reached EOF and requires no more work,
// we should return a classic plan stage even if engine selection would choose SBE. This
// is because the query was already fully answered during multiplanning, and lowering to
// SBE and restarting the query would be unnecessary.
TEST_F(DeferredEngineChoiceLoweringTest, MultiplanningUsesEof) {
    if (kDebugBuild) {
        // EOF optimization is not used in debug builds.
        return;
    }

    auto testNumDocs = [&](size_t numDocs) {
        // Depending on the number of documents, a plan may reach EOF.
        bool shouldUseSbe = numDocs > 100;

        auto [cq, plannerData] = createPlannerData(kFindFilter);
        auto solutions = createVirtualScanQuerySolutionsForDefaultFilter(
            numDocs /*resultDocCount*/, cq->getPrimaryMatchExpression());
        exec_deferred_engine_choice::MultiPlanner planner{std::move(plannerData),
                                                          std::move(solutions)};
        // Include the SBE-eligible pipeline, so that if multiplanning hits EOF we see classic used,
        // and if EOF is not reached we see SBE used.
        auto pipeline = makeSbeEligiblePipeline();
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec =
            lowerPlanRankingResult(std::move(cq),
                                   planner.extractPlanRankingResult(),
                                   operationContext(),
                                   collections(),
                                   PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                   pipeline.get());

        if (shouldUseSbe) {
            ASSERT(dynamic_cast<PlanExecutorSBE*>(exec.get()));
        } else {
            ASSERT(dynamic_cast<PlanExecutorImpl*>(exec.get()));
        }
    };

    testNumDocs(1);
    testNumDocs(50);
    testNumDocs(150);
    testNumDocs(200);
}
}  // namespace
}  // namespace mongo::exec_deferred_engine_choice
