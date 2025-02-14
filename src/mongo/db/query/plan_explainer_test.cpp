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

#include "mongo/bson/json.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/commands.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/idl/server_parameter_test_util.h"

namespace mongo {
namespace {

using namespace fmt::literals;

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.collection");

// This fixture sets up a collection on namespace kNss with indexes on field "a" and "b" and 200
// sample documents.
class PlanExplainerTest : public CatalogTestFixture {
protected:
    void setUp() final {
        CatalogTestFixture::setUp();
        OperationContext* opCtx = operationContext();
        expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions()));
        createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
        createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
        insertDocuments(kNss, 200);
    }

    void tearDown() final {
        expCtx.reset();
        CatalogTestFixture::tearDown();
    }

    void insertDocuments(const NamespaceString& nss, int count) {
        std::vector<BSONObj> docs;
        for (int i = 0; i < count; i++) {
            BSONObj obj = BSON("_id" << i << "a" << i % 100 << "b" << i % 10);
            docs.push_back(obj);
        }
        std::vector<InsertStatement> inserts{docs.begin(), docs.end()};

        AutoGetCollection agc(operationContext(), nss, LockMode::MODE_IX);
        {
            WriteUnitOfWork wuow{operationContext()};
            ASSERT_OK(collection_internal::insertDocuments(
                operationContext(), *agc, inserts.begin(), inserts.end(), nullptr /* opDebug */));
            wuow.commit();
        }
    }

    void createIndexOnEmptyCollection(OperationContext* opCtx,
                                      BSONObj index,
                                      std::string indexName) {
        auto acquisition = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kWrite),
            MODE_X);
        CollectionWriter coll(opCtx, &acquisition);

        WriteUnitOfWork wunit(opCtx);
        auto indexCatalog = coll.getWritableCollection(opCtx)->getIndexCatalog();
        ASSERT(indexCatalog);

        auto indexSpec = BSON("v" << IndexDescriptor::kLatestIndexVersion << "key" << index
                                  << "name" << indexName);
        ASSERT_OK(
            indexCatalog
                ->createIndexOnEmptyCollection(opCtx, coll.getWritableCollection(opCtx), indexSpec)
                .getStatus());
        wunit.commit();
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> buildFindExecutorForFilter(
        BSONObj filter, bool limitOne = false) {
        AutoGetCollection collPtr(operationContext(), kNss, LockMode::MODE_IX);
        auto colls =
            MultipleCollectionAccessor(operationContext(),
                                       &collPtr.getCollection(),
                                       kNss,
                                       false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */,
                                       {});

        auto findCommand = std::make_unique<FindCommandRequest>(kNss);
        findCommand->setFilter(filter);
        if (limitOne) {
            findCommand->setLimit(1);
        }
        auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = expCtx,
            .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});
        auto exec = getExecutorFind(
            operationContext(), colls, std::move(cq), PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);
        ASSERT(exec.isOK());
        return std::move(exec.getValue());
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> buildAggExecutor(
        std::vector<BSONObj> stages) {
        AutoGetCollection collPtr(operationContext(), kNss, LockMode::MODE_IX);
        auto colls =
            MultipleCollectionAccessor(operationContext(),
                                       &collPtr.getCollection(),
                                       kNss,
                                       false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */,
                                       {});

        auto pipeline = Pipeline::parse(stages, expCtx);
        auto request = AggregateCommandRequest(kNss, stages);
        PipelineD::buildAndAttachInnerQueryExecutorToPipeline(
            colls, kNss, &request, pipeline.get());
        return plan_executor_factory::make(expCtx, std::move(pipeline));
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
};

TEST_F(PlanExplainerTest, ClassicSingleSolutionPlanExplain) {
    // A query against a non-indexed field will have a single solution plan that can be explained.
    // The explain output should indicate the plan that was chosen.
    auto exec = buildFindExecutorForFilter(fromjson("{c: {$eq: 1}}"));
    auto& explainer = exec->getPlanExplainer();

    ASSERT(!explainer.isMultiPlan());
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "COLLSCAN");
    ASSERT(!winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, SBESingleSolutionPlanExplain) {
    // Same as above, but with SBE enabled.
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", true);
    auto exec = buildFindExecutorForFilter(fromjson("{c: {$eq: 1}}"));
    auto& explainer = exec->getPlanExplainer();

    ASSERT(!explainer.isMultiPlan());
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "COLLSCAN");
    ASSERT(winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, ClassicMultiPlannerExplain) {
    // A query including sargable predicates on different fields will consider multiple plans during
    // planning. Its executor can be explained, and the explain output should indicate the plan that
    // was chosen.
    auto exec = buildFindExecutorForFilter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    auto& explainer = exec->getPlanExplainer();

    ASSERT(explainer.isMultiPlan());
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "IXSCAN");
    ASSERT(!winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, SBEMultiPlannerExplain) {
    // Same as above, but with SBE enabled.
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", true);
    auto exec = buildFindExecutorForFilter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    auto& explainer = exec->getPlanExplainer();

    ASSERT(explainer.isMultiPlan());
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "IXSCAN");
    ASSERT(winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, ExpressPlanIdHackExplain) {
    // An express-eligible query will have a single solution plan that can be explained.
    // The explain output should indicate the plan that was chosen.
    auto exec = buildFindExecutorForFilter(fromjson("{_id: 1}"));
    auto& explainer = exec->getPlanExplainer();

    ASSERT(!explainer.isMultiPlan());
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "EXPRESS_IXSCAN");
    ASSERT(!winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, ExpressPlanSingleFieldEqExplain) {
    // Same as above, but testing non-_id express plan.
    auto exec = buildFindExecutorForFilter(fromjson("{a: 1}"), true /* limitOne */);
    auto& explainer = exec->getPlanExplainer();

    ASSERT(!explainer.isMultiPlan());
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "EXPRESS_IXSCAN");
    ASSERT(!winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, ClassicPipelinePlanExplain) {
    // A pipeline query including sargable predicates on different fields will consider multiple
    // plans during planning. Its executor can be explained, and the explain output should indicate
    // the plan that was chosen. Note that we intentionally build a pipeline that cannot be
    // completely pushed into the find layer here.
    auto stages = std::vector{
        fromjson("{$match: {a: {$gte: 0}, b: {$gte: 0}}}"),
        fromjson("{$setWindowFields: {sortBy: {_id: 1}, output: {rank: {$rank: {}}}}}")};
    auto exec = buildAggExecutor(stages);
    auto& explainer = exec->getPlanExplainer();

    // TODO SERVER-49808: add assertion on explain output for a pipeline executor.
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(winningPlan.toString(), "{}");
}

TEST_F(PlanExplainerTest, SBEPipelinePlanExplain) {
    // Same as above, with SBE enabled.
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", true);
    auto stages = std::vector{
        fromjson("{$match: {a: {$gte: 0}, b: {$gte: 0}}}"),
        fromjson("{$setWindowFields: {sortBy: {_id: 1}, output: {rank: {$rank: {}}}}}")};
    auto exec = buildAggExecutor(stages);
    auto& explainer = exec->getPlanExplainer();

    // TODO SERVER-49808: add assertion on explain output for a pipeline executor.
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(winningPlan.toString(), "{}");
}
}  // namespace
}  // namespace mongo
