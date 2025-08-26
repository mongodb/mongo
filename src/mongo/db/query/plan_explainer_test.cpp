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
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/commands.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/explain_diagnostic_printer.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/idl/server_parameter_test_controller.h"

namespace mongo {
namespace {

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

        const auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), nss, AcquisitionPrerequisites::OperationType::kWrite),
            MODE_IX);
        {
            WriteUnitOfWork wuow{operationContext()};
            ASSERT_OK(collection_internal::insertDocuments(operationContext(),
                                                           coll.getCollectionPtr(),
                                                           inserts.begin(),
                                                           inserts.end(),
                                                           nullptr /* opDebug */));
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

        auto indexSpec =
            BSON("v" << IndexConfig::kLatestIndexVersion << "key" << index << "name" << indexName);
        ASSERT_OK(
            indexCatalog
                ->createIndexOnEmptyCollection(opCtx, coll.getWritableCollection(opCtx), indexSpec)
                .getStatus());
        wunit.commit();
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> buildFindExecAndIter(
        BSONObj filter, bool limitOne = false) {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), kNss, AcquisitionPrerequisites::OperationType::kRead),
            MODE_IS);
        auto colls = MultipleCollectionAccessor(
            std::move(coll), {}, false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */);

        auto findCommand = std::make_unique<FindCommandRequest>(kNss);
        findCommand->setFilter(filter);
        if (limitOne) {
            findCommand->setLimit(1);
        }
        auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = expCtx,
            .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});

        // The explain printer checks the CurOp's Command to see if it is allowed to print it.
        Command* cmd = CommandHelpers::findCommand(operationContext(), "find");
        {
            stdx::lock_guard<Client> clientLock(*operationContext()->getClient());
            CurOp::get(operationContext())
                ->setGenericOpRequestDetails(clientLock, kNss, cmd, BSONObj(), NetworkOp::dbQuery);
        }

        auto exec = getExecutorFind(
            operationContext(), colls, std::move(cq), PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);
        ASSERT(exec.isOK());

        exec.getValue()->getNext(nullptr, nullptr);
        return std::move(exec.getValue());
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> buildAggExecAndIter(
        std::vector<BSONObj> stages) {
        DocumentSourceContainer sources;
        for (const auto& s : stages) {
            sources.push_back(DocumentSource::parse(expCtx, s).front());
        }
        auto pipeline = Pipeline::create(sources, expCtx);

        {
            auto coll = acquireCollection(
                operationContext(),
                CollectionAcquisitionRequest::fromOpCtx(
                    operationContext(), kNss, AcquisitionPrerequisites::OperationType::kRead),
                MODE_IS);
            auto colls = MultipleCollectionAccessor(
                std::move(coll), {}, false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */);

            // The explain printer checks the CurOp's Command to see if it is allowed to print it.
            auto request = AggregateCommandRequest(kNss, stages);
            Command* cmd = CommandHelpers::findCommand(operationContext(), "aggregate");
            {
                stdx::lock_guard<Client> clientLock(*operationContext()->getClient());
                CurOp::get(operationContext())
                    ->setGenericOpRequestDetails(
                        clientLock, kNss, cmd, BSONObj(), NetworkOp::dbQuery);
            }

            auto transactionResourcesStasher =
                make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
            auto catalogResourceHandle =
                make_intrusive<DSCursorCatalogResourceHandle>(transactionResourcesStasher);
            PipelineD::buildAndAttachInnerQueryExecutorToPipeline(
                colls, kNss, &request, pipeline.get(), catalogResourceHandle);

            // Stash the ShardRole resources.
            stashTransactionResourcesFromOperationContext(operationContext(),
                                                          transactionResourcesStasher.get());
        }

        auto exec = plan_executor_factory::make(expCtx, std::move(pipeline));
        exec->getNext(nullptr, nullptr);
        return exec;
    }

    std::string printExplainDiagnostics(PlanExecutor* exec) {
        diagnostic_printers::ExplainDiagnosticPrinter printer{exec};
        return fmt::format("{}", printer);
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
};

TEST_F(PlanExplainerTest, ClassicSingleSolutionPlanExplain) {
    // A query against a non-indexed field will have a single solution plan that can be explained.
    // The explain output should indicate the plan that was chosen.
    auto exec = buildFindExecAndIter(fromjson("{c: {$eq: 1}}"));
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
    auto exec = buildFindExecAndIter(fromjson("{c: {$eq: 1}}"));
    auto& explainer = exec->getPlanExplainer();

    ASSERT(!explainer.isMultiPlan());
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "COLLSCAN");
    ASSERT(winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, ClassicSingleSolutionPlanExplainDiagnostics) {
    // The ExplainDiagnosticPrinter includes the query planner and execution stats info for the
    // query above.
    auto exec = buildFindExecAndIter(fromjson("{c: {$eq: 1}}"));

    auto explainDiagnostics = printExplainDiagnostics(exec.get());
    ASSERT_STRING_CONTAINS(explainDiagnostics, "COLLSCAN");
    ASSERT_STRING_OMITS(explainDiagnostics, "slotBasedPlan");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "executionStats': {");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "isEOF: 1");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "saveState: ");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "nReturned: 0");
}

TEST_F(PlanExplainerTest, SBESingleSolutionPlanExplainDiagnostics) {
    // Same as above, but with SBE enabled.
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", true);
    auto exec = buildFindExecAndIter(fromjson("{c: {$eq: 1}}"));

    auto explainDiagnostics = printExplainDiagnostics(exec.get());
    ASSERT_STRING_CONTAINS(explainDiagnostics, "COLLSCAN");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "slotBasedPlan");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "executionStats': {");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "isEOF: 1");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "saveState: ");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "nReturned: 0");
}

TEST_F(PlanExplainerTest, ClassicMultiPlannerExplain) {
    // A query including sargable predicates on different fields will consider multiple plans during
    // planning. Its executor can be explained, and the explain output should indicate the plan that
    // was chosen.
    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
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
    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    auto& explainer = exec->getPlanExplainer();

    ASSERT(explainer.isMultiPlan());
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "IXSCAN");
    ASSERT(winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, ClassicMultiPlannerExplainDiagnostics) {
    // The ExplainDiagnosticPrinter includes the query planner and execution stats info for the
    // query above.
    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));

    auto explainDiagnostics = printExplainDiagnostics(exec.get());
    ASSERT_STRING_CONTAINS(explainDiagnostics, "IXSCAN");
    ASSERT_STRING_OMITS(explainDiagnostics, "slotBasedPlan");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "executionStats': {");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "isEOF: 0");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "saveState: ");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "nReturned: 1");
}

TEST_F(PlanExplainerTest, SBEMultiPlannerExplainDiagnostics) {
    // Same as above, but with SBE enabled.
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", true);
    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));

    auto explainDiagnostics = printExplainDiagnostics(exec.get());
    ASSERT_STRING_CONTAINS(explainDiagnostics, "IXSCAN");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "slotBasedPlan");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "executionStats': {");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "isEOF: 0");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "saveState: ");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "nReturned: 1");
}

TEST_F(PlanExplainerTest, ExpressPlanIdHackExplain) {
    // An express-eligible query will have a single solution plan that can be explained.
    // The explain output should indicate the plan that was chosen.
    auto exec = buildFindExecAndIter(fromjson("{_id: 1}"));
    auto& explainer = exec->getPlanExplainer();

    ASSERT(!explainer.isMultiPlan());
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "EXPRESS_IXSCAN");
    ASSERT(!winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, ExpressPlanSingleFieldEqExplain) {
    // Same as above, but testing non-_id express plan.
    auto exec = buildFindExecAndIter(fromjson("{a: 1}"), true /* limitOne */);
    auto& explainer = exec->getPlanExplainer();

    ASSERT(!explainer.isMultiPlan());
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "EXPRESS_IXSCAN");
    ASSERT(!winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, ExpressPlanIdHackExplainDiagnostics) {
    // The ExplainDiagnosticPrinter includes the query planner and execution stats info for the _id
    // query above.
    auto exec = buildFindExecAndIter(fromjson("{_id: 1}"));

    auto explainDiagnostics = printExplainDiagnostics(exec.get());
    ASSERT_STRING_CONTAINS(explainDiagnostics, "EXPRESS_IXSCAN");
    ASSERT_STRING_OMITS(explainDiagnostics, "slotBasedPlan");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "executionStats': {");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "nReturned: 1");
}

TEST_F(PlanExplainerTest, ExpressPlanSingleFieldEqExplainDiagnostics) {
    // The ExplainDiagnosticPrinter includes the query planner and execution stats info for the
    // non-_id query above.
    auto exec = buildFindExecAndIter(fromjson("{a: 1}"), true /* limitOne */);

    auto explainDiagnostics = printExplainDiagnostics(exec.get());
    ASSERT_STRING_CONTAINS(explainDiagnostics, "EXPRESS_IXSCAN");
    ASSERT_STRING_OMITS(explainDiagnostics, "slotBasedPlan");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "executionStats': {");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "nReturned: 1");
}

TEST_F(PlanExplainerTest, ClassicPipelinePlanExplain) {
    // A pipeline query including sargable predicates on different fields will consider multiple
    // plans during planning. Its executor can be explained, and the explain output should indicate
    // the plan that was chosen. Note that we intentionally build a pipeline that cannot be
    // completely pushed into the find layer here.
    auto stages =
        std::vector{fromjson("{$match: {a: {$gte: 0}, b: {$gte: 0}}}"),
                    fromjson("{$redact: {$cond: {if: '$a', then: '$$PRUNE', else: '$$DESCEND'}}}")};
    auto exec = buildAggExecAndIter(stages);
    auto& explainer = exec->getPlanExplainer();

    // TODO SERVER-49808: add assertion on the pipeline part of the explain output.
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "IXSCAN");
    ASSERT(!winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, SBEPipelinePlanExplain) {
    // Same as above, with SBE enabled.
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", true);
    auto stages =
        std::vector{fromjson("{$match: {a: {$gte: 0}, b: {$gte: 0}}}"),
                    fromjson("{$redact: {$cond: {if: '$a', then: '$$PRUNE', else: '$$DESCEND'}}}")};
    auto exec = buildAggExecAndIter(stages);
    auto& explainer = exec->getPlanExplainer();

    // TODO SERVER-49808: add assertion on the pipeline part of the explain output.
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "IXSCAN");
    ASSERT(winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, ClassicPipelinePlanExplainDiagnostics) {
    // The ExplainDiagnosticPrinter includes the query planner and execution stats info for the
    // query above.
    auto stages =
        std::vector{fromjson("{$match: {a: {$gte: 0}, b: {$gte: 0}}}"),
                    fromjson("{$redact: {$cond: {if: '$a', then: '$$PRUNE', else: '$$DESCEND'}}}")};
    auto exec = buildAggExecAndIter(stages);

    auto explainDiagnostics = printExplainDiagnostics(exec.get());
    ASSERT_STRING_CONTAINS(explainDiagnostics, "IXSCAN");
    ASSERT_STRING_OMITS(explainDiagnostics, "slotBasedPlan");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "executionStats': {");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "isEOF: 0");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "saveState: ");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "nReturned: 1");
}

TEST_F(PlanExplainerTest, SBEPipelinePlanExplainDiagnostics) {
    // Same as above, with SBE enabled.
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", true);
    auto stages =
        std::vector{fromjson("{$match: {a: {$gte: 0}, b: {$gte: 0}}}"),
                    fromjson("{$redact: {$cond: {if: '$a', then: '$$PRUNE', else: '$$DESCEND'}}}")};
    auto exec = buildAggExecAndIter(stages);

    auto explainDiagnostics = printExplainDiagnostics(exec.get());
    ASSERT_STRING_CONTAINS(explainDiagnostics, "IXSCAN");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "slotBasedPlan");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "executionStats': {");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "isEOF: 0");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "saveState: ");
    ASSERT_STRING_CONTAINS(explainDiagnostics, "numTested: 1");
}
}  // namespace
}  // namespace mongo
