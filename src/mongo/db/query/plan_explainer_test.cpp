// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_common.h"
#include "mongo/db/query/explain_diagnostic_printer.h"
#include "mongo/db/query/explain_policy.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_explainer_sbe.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_settings/query_knob_overrides.h"
#include "mongo/db/query/query_settings/query_settings_context_test_util.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/query/record_id_range.h"
#include "mongo/db/query/record_id_range_list.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <functional>

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

        const auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), nss, AcquisitionPrerequisites::OperationType::kWrite),
            MODE_IX);
        WriteUnitOfWork wuow{operationContext()};
        ASSERT_OK(Helpers::insert(operationContext(), coll.getCollectionPtr(), docs));
        wuow.commit();
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
            std::lock_guard<Client> clientLock(*operationContext()->getClient());
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
                std::lock_guard<Client> clientLock(*operationContext()->getClient());
                CurOp::get(operationContext())
                    ->setGenericOpRequestDetails(
                        clientLock, kNss, cmd, BSONObj(), NetworkOp::dbQuery);
            }

            auto transactionResourcesStasher =
                make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
            PipelineD::buildAndAttachInnerQueryExecutorAndBindCatalogInfoToPipeline(
                colls, kNss, &request, pipeline.get(), transactionResourcesStasher);

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

    void checkPipelinePlanExplainDiagnostics(PlanExecutor* exec, bool sbeEnabled) {
        auto explainDiagnostics = printExplainDiagnostics(exec);
        ASSERT_STRING_CONTAINS(explainDiagnostics, "IXSCAN");
        if (sbeEnabled) {
            ASSERT_STRING_CONTAINS(explainDiagnostics, "slotBasedPlan");
        } else {
            ASSERT_STRING_OMITS(explainDiagnostics, "slotBasedPlan");
        }
        ASSERT_STRING_CONTAINS(explainDiagnostics, "executionStats': {");
        ASSERT_STRING_CONTAINS(explainDiagnostics, "isEOF: 0");
        ASSERT_STRING_CONTAINS(explainDiagnostics, "saveState: ");
        ASSERT_STRING_CONTAINS(explainDiagnostics, "nReturned: ");
        ASSERT_STRING_OMITS(explainDiagnostics, "nReturned: 0");
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
};

TEST_F(PlanExplainerTest, ClassicSingleSolutionPlanExplain) {
    // A query against a non-indexed field will have a single solution plan that can be explained.
    // The explain output should indicate the plan that was chosen.
    auto exec = buildFindExecAndIter(fromjson("{c: {$eq: 1}}"));
    auto& explainer = exec->getPlanExplainer();

    ASSERT(!explainer.areThereRejectedPlansToExplain());
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "COLLSCAN");
    ASSERT(!winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, GetReportedVersionReflectsV3Verbosities) {
    auto exec = buildFindExecAndIter(fromjson("{c: {$eq: 1}}"));
    auto& explainer = exec->getPlanExplainer();

    // For legacy verbosities, the reported version matches the engine-determined version.
    const PlanExplainer::ExplainVersion engineVersion =
        explainer.getVersion(ExplainOptions::Verbosity::kQueryPlanner);
    for (auto verbosity : {ExplainOptions::Verbosity::kQueryPlanner,
                           ExplainOptions::Verbosity::kExecStats,
                           ExplainOptions::Verbosity::kExecAllPlans,
                           ExplainOptions::Verbosity::kInternal}) {
        ASSERT_EQ(explainer.getVersion(verbosity), engineVersion);
    }

    // For the V3 verbosity modes, the reported version is "3" regardless of the engine.
    for (auto verbosity : {ExplainOptions::Verbosity::kPlanSummary,
                           ExplainOptions::Verbosity::kPlannerChoice,
                           ExplainOptions::Verbosity::kPlannerStats,
                           ExplainOptions::Verbosity::kExecStatsV3}) {
        ASSERT_EQ(explainer.getVersion(verbosity), "3");
    }
}

TEST_F(PlanExplainerTest, SBESingleSolutionPlanExplain) {
    // Same as above, but with SBE enabled.
    unittest::ServerParameterGuard sbeFullController("featureFlagSbeFull", true);
    auto exec = buildFindExecAndIter(fromjson("{c: {$eq: 1}}"));
    auto& explainer = exec->getPlanExplainer();

    ASSERT(!explainer.areThereRejectedPlansToExplain());
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
    unittest::ServerParameterGuard sbeFullController("featureFlagSbeFull", true);
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

    ASSERT(explainer.areThereRejectedPlansToExplain());
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "IXSCAN");
    ASSERT(!winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, GetPlanEntriesSingleSolution) {
    // A single-solution (COLLSCAN) query yields exactly one plan entry, flagged as the winner.
    auto exec = buildFindExecAndIter(fromjson("{c: {$eq: 1}}"));
    auto& explainer = exec->getPlanExplainer();

    ASSERT(!explainer.areThereRejectedPlansToExplain());
    auto entries =
        explainer.getPlanEntries(explainPolicyFor(ExplainOptions::Verbosity::kQueryPlanner),
                                 PlanStatsFormat::kLegacy,
                                 PlanRankerMethod::kNone);
    ASSERT_EQ(entries.size(), 1u);
    ASSERT_STRING_CONTAINS(entries[0].planStatsTree.toString(), "COLLSCAN");
    // No execution stats are requested at queryPlanner verbosity.
    ASSERT_FALSE(entries[0].summary.has_value());
}

TEST_F(PlanExplainerTest, GetPlanEntriesMultiPlanner) {
    // A multi-planned query yields the winner first, then the remaining candidates.
    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    auto& explainer = exec->getPlanExplainer();

    ASSERT(explainer.areThereRejectedPlansToExplain());
    auto entries =
        explainer.getPlanEntries(explainPolicyFor(ExplainOptions::Verbosity::kQueryPlanner),
                                 PlanStatsFormat::kLegacy,
                                 PlanRankerMethod::kNone);
    ASSERT_GTE(entries.size(), 2u);

    // The winner is the first entry (its position is the contract) and is byte-identical to the
    // dedicated winning-plan accessor, proving both read the same per-plan formatting core.
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_BSONOBJ_EQ(entries[0].planStatsTree, winningPlan);
}

TEST_F(PlanExplainerTest, GetPlanEntriesMultiPlannerExecStats) {
    // At allPlansExecution verbosity every entry carries a summary, and the rejected entries carry
    // a trial-period score.
    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    auto& explainer = exec->getPlanExplainer();

    auto entries =
        explainer.getPlanEntries(explainPolicyFor(ExplainOptions::Verbosity::kExecAllPlans),
                                 PlanStatsFormat::kLegacy,
                                 PlanRankerMethod::kNone);
    ASSERT_GTE(entries.size(), 2u);
    for (const auto& entry : entries) {
        ASSERT(entry.summary.has_value());
    }
    // A rejected entry carries its trial-period score.
    ASSERT(entries[1].summary->score.has_value());
}

// Walks a V3-format plan stats tree, invoking 'callback' on every node (root to leaves). The V3
// node shape always nests children as the 'inputStages' array.
void forEachV3Node(const BSONObj& node, const std::function<void(const BSONObj&)>& callback) {
    callback(node);
    if (auto inputStages = node["inputStages"]; !inputStages.eoo()) {
        for (auto&& child : inputStages.Array()) {
            forEachV3Node(child.Obj(), callback);
        }
    }
}

TEST_F(PlanExplainerTest, GetPlanEntriesV3MultiPlannerNodeGrouping) {
    // The V3 node shape for a multi-planned query (default knobs; the trial produces results, so
    // the multi-planner decides): structural fields stay flat on the node, the trial counters are
    // regrouped under statistics.multiPlan.
    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    auto& explainer = exec->getPlanExplainer();

    const auto policy = explainPolicyFor(ExplainOptions::Verbosity::kPlannerStats);
    auto entries =
        explainer.getPlanEntries(policy, PlanStatsFormat::kV3, PlanRankerMethod::kMultiPlanner);
    ASSERT_GTE(entries.size(), 2u);

    for (const auto& entry : entries) {
        // Every candidate ran a multi-planning trial, and the V3 plannerStats policy requests
        // per-candidate statistics, so the plan-level summary exists despite hasExecStats() being
        // false.
        ASSERT(entry.hasTrialStats) << entry.planStatsTree;
        ASSERT(entry.summary.has_value()) << entry.planStatsTree;

        forEachV3Node(entry.planStatsTree, [&](const BSONObj& node) {
            // Counters moved into statistics.multiPlan; never flat on the node.
            ASSERT_FALSE(node.hasField("works")) << node;
            ASSERT_FALSE(node.hasField("nReturned")) << node;
            auto multiPlan = node["statistics"]["multiPlan"];
            ASSERT(multiPlan.isABSONObj()) << node;
            ASSERT(multiPlan.Obj().hasField("works")) << node;
            ASSERT(multiPlan.Obj().hasField("nReturned")) << node;
            ASSERT(multiPlan.Obj().hasField("isEOF")) << node;
            // Structural fields stay flat on the node.
            if (node["stage"].String() == "IXSCAN") {
                ASSERT(node.hasField("keyPattern")) << node;
                ASSERT(node.hasField("indexBounds")) << node;
                ASSERT(multiPlan.Obj().hasField("keysExamined")) << node;
                ASSERT_FALSE(node.hasField("keysExamined")) << node;
            }
            // planNodeId appears only on nodes with a known QSN mapping; the pure-multiplanning
            // decision path does not populate the mapping, so it is legitimately absent here.
        });
    }

    // The plans after the winner are ordered by trial score, descending (the multi-planner
    // decided).
    for (size_t i = 2; i < entries.size(); ++i) {
        if (entries[i - 1].summary->score && entries[i].summary->score) {
            ASSERT_GTE(*entries[i - 1].summary->score, *entries[i].summary->score);
        }
    }
}

TEST_F(PlanExplainerTest, GetPlanEntriesV3StopConditionFullBatch) {
    // A multi-planned query whose candidates each return far more than the trial's result target
    // and never reach EOF within it: every plan that ran a trial reports a stop condition, and the
    // plans that ended the trial did so by filling a batch.
    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    auto& explainer = exec->getPlanExplainer();

    auto entries =
        explainer.getPlanEntries(explainPolicyFor(ExplainOptions::Verbosity::kPlannerStats),
                                 PlanStatsFormat::kV3,
                                 PlanRankerMethod::kMultiPlanner);
    ASSERT_GTE(entries.size(), 2u);

    bool sawFullBatch = false;
    for (const auto& entry : entries) {
        // The stop condition is reported for exactly the plans that ran a trial.
        ASSERT_EQ(entry.hasTrialStats, entry.stopCondition.has_value()) << entry.planStatsTree;
        // Each candidate either filled the batch itself or was stopped when a sibling did. No
        // candidate can reach EOF here (each index scan covers all 200 documents, more than the
        // trial's result target), and none can run out of budget, since filling a batch ends the
        // trial long before the budget is spent.
        ASSERT(entry.stopCondition == MultiPlannerStopCondition::kFullBatch ||
               entry.stopCondition == MultiPlannerStopCondition::kTrialEndedEarly)
            << entry.planStatsTree;
        sawFullBatch |= entry.stopCondition == MultiPlannerStopCondition::kFullBatch;
    }
    ASSERT(sawFullBatch) << "expected a plan to have filled the trial's result batch";
}

TEST_F(PlanExplainerTest, GetPlanEntriesV3StopConditionTrialEndedEarly) {
    // A candidate that met no early-exit condition of its own reports why it stopped anyway, and
    // that reason distinguishes the two ways it can happen. Here the winner reaches EOF, which ends
    // the trial period for everyone: the remaining candidates were cut short with budget to spare
    // ('kTrialEndedEarly'), which is a different fact from having spent the budget
    // ('kExhaustedBudget', asserted below) even though neither exited early. Their trial counters
    // are tiny - a handful of works out of thousands - which is exactly why the two must not be
    // conflated.
    auto exec = buildFindExecAndIter(fromjson("{a: {$eq: 5}, b: {$eq: 5}}"));
    auto& explainer = exec->getPlanExplainer();

    auto entries =
        explainer.getPlanEntries(explainPolicyFor(ExplainOptions::Verbosity::kPlannerStats),
                                 PlanStatsFormat::kV3,
                                 PlanRankerMethod::kMultiPlanner);
    ASSERT_GTE(entries.size(), 2u);
    ASSERT_EQ(entries[0].stopCondition, MultiPlannerStopCondition::kEof)
        << entries[0].planStatsTree;
    for (size_t i = 1; i < entries.size(); ++i) {
        ASSERT_EQ(entries[i].stopCondition, MultiPlannerStopCondition::kTrialEndedEarly)
            << entries[i].planStatsTree;
        // The budget is nowhere near spent: the trial stopped as soon as the winner hit EOF, after
        // a few works out of a per-plan budget of at least 'internalQueryPlanEvaluationWorks'
        // (10000 by default).
        ASSERT_LT(entries[i].summary->totalKeysExamined, 1000u) << entries[i].planStatsTree;
    }
}

TEST_F(PlanExplainerTest, GetPlanEntriesV3StopConditionEof) {
    // The winning plan of a multi-planned query whose candidates exhaust their results well before
    // a batch is filled.
    auto exec = buildFindExecAndIter(fromjson("{a: {$eq: 5}, b: {$eq: 5}}"));
    auto& explainer = exec->getPlanExplainer();

    auto entries =
        explainer.getPlanEntries(explainPolicyFor(ExplainOptions::Verbosity::kPlannerStats),
                                 PlanStatsFormat::kV3,
                                 PlanRankerMethod::kMultiPlanner);
    ASSERT_GTE(entries.size(), 2u);
    ASSERT_EQ(entries[0].stopCondition, MultiPlannerStopCondition::kEof)
        << entries[0].planStatsTree;
}

TEST_F(PlanExplainerTest, GetPlanEntriesV3StopConditionExhaustedBudget) {
    // With the trial's work budget squeezed to a single work per plan, no candidate can meet an
    // early-exit condition, so every candidate ran out of budget.
    unittest::ServerParameterGuard worksGuard("internalQueryPlanEvaluationWorks", 1);
    unittest::ServerParameterGuard collFractionGuard("internalQueryPlanEvaluationCollFraction",
                                                     0.0);
    unittest::ServerParameterGuard totalCollFractionGuard(
        "internalQueryPlanTotalEvaluationCollFraction", 0.0);

    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    auto& explainer = exec->getPlanExplainer();

    auto entries =
        explainer.getPlanEntries(explainPolicyFor(ExplainOptions::Verbosity::kPlannerStats),
                                 PlanStatsFormat::kV3,
                                 PlanRankerMethod::kMultiPlanner);
    ASSERT_GTE(entries.size(), 2u);
    for (const auto& entry : entries) {
        ASSERT_EQ(entry.stopCondition, MultiPlannerStopCondition::kExhaustedBudget)
            << entry.planStatsTree;
    }
}

TEST_F(PlanExplainerTest, GetPlanEntriesV3SingleSolutionSparseStatistics) {
    // Sparseness: a single-solution plan never ran a trial and was never costed, so no node has a
    // "statistics" subobject at all (absent, not empty), and there are no plan-level trial stats.
    auto exec = buildFindExecAndIter(fromjson("{c: {$eq: 1}}"));
    auto& explainer = exec->getPlanExplainer();

    auto entries =
        explainer.getPlanEntries(explainPolicyFor(ExplainOptions::Verbosity::kPlannerStats),
                                 PlanStatsFormat::kV3,
                                 PlanRankerMethod::kNone);
    ASSERT_EQ(entries.size(), 1u);
    ASSERT_FALSE(entries[0].hasTrialStats);

    forEachV3Node(entries[0].planStatsTree, [&](const BSONObj& node) {
        ASSERT_FALSE(node.hasField("statistics")) << node;
        ASSERT(node.hasField("stage")) << node;
    });
}

TEST_F(PlanExplainerTest, GetPlanEntriesV3WinnerUsesTrialSnapshot) {
    // The winner's V3 tree must show trial statistics, never final-execution statistics: executing
    // the query further must not change the winner's entry. Explain queries get the winner's trial
    // snapshot (exported by the ranking strategies, or captured at explainer construction).
    expCtx->setExplain(ExplainOptions::Verbosity::kPlannerStats);
    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    auto& explainer = exec->getPlanExplainer();

    const auto policy = explainPolicyFor(ExplainOptions::Verbosity::kPlannerStats);
    auto before =
        explainer.getPlanEntries(policy, PlanStatsFormat::kV3, PlanRankerMethod::kMultiPlanner);

    // Drain the executor: the live root's counters accumulate real-execution work.
    while (exec->getNext(nullptr, nullptr) != PlanExecutor::IS_EOF) {
    }

    auto after =
        explainer.getPlanEntries(policy, PlanStatsFormat::kV3, PlanRankerMethod::kMultiPlanner);
    ASSERT_EQ(before.size(), after.size());
    ASSERT_BSONOBJ_EQ(before[0].planStatsTree, after[0].planStatsTree);
}

TEST_F(PlanExplainerTest, GetPlanEntriesV3WinnerUsesTrialSnapshotPureMultiPlanning) {
    // Same guarantee on the pure-multiplanning path (MultiPlanStage still in the execution tree):
    // the explainer's constructor snapshots the trial statistics before the explained query can
    // execute, isolating the winner's trial tree from execution.
    unittest::ServerParameterGuard cbrController("featureFlagCostBasedRanker", false);
    expCtx->setExplain(ExplainOptions::Verbosity::kPlannerStats);
    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    auto& explainer = exec->getPlanExplainer();

    const auto policy = explainPolicyFor(ExplainOptions::Verbosity::kPlannerStats);
    auto before =
        explainer.getPlanEntries(policy, PlanStatsFormat::kV3, PlanRankerMethod::kMultiPlanner);

    while (exec->getNext(nullptr, nullptr) != PlanExecutor::IS_EOF) {
    }

    auto after =
        explainer.getPlanEntries(policy, PlanStatsFormat::kV3, PlanRankerMethod::kMultiPlanner);
    ASSERT_EQ(before.size(), after.size());
    for (size_t i = 0; i < before.size(); ++i) {
        ASSERT_BSONOBJ_EQ(before[i].planStatsTree, after[i].planStatsTree);
    }
}

TEST_F(PlanExplainerTest, GetPlanEntriesV3CostBasedRankerOrdering) {
    // When the cost-based ranker decided, plans are grouped under statistics.costBased and the
    // plans after the winner are ordered by root cost estimate, ascending.
    unittest::ServerParameterGuard planRankerController("internalQueryPlanRanker", "costBased");
    unittest::ServerParameterGuard samplingController("internalQueryCBRCEMode", "samplingCE");
    expCtx->setExplain(ExplainOptions::Verbosity::kPlannerStats);

    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    auto& explainer = exec->getPlanExplainer();

    auto entries =
        explainer.getPlanEntries(explainPolicyFor(ExplainOptions::Verbosity::kPlannerStats),
                                 PlanStatsFormat::kV3,
                                 PlanRankerMethod::kCostBasedRanker);
    ASSERT_GTE(entries.size(), 2u);

    boost::optional<double> previousCost;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        auto costBased = entry.planStatsTree["statistics"]["costBased"];
        ASSERT(costBased.isABSONObj()) << entry.planStatsTree;
        ASSERT(costBased.Obj().hasField("costEstimate")) << entry.planStatsTree;
        ASSERT(costBased.Obj().hasField("cardinalityEstimate")) << entry.planStatsTree;

        if (i != 0) {
            // CBR-rejected plans never ran a trial: no multiPlan group, no plan-level trial stats.
            ASSERT_FALSE(entry.hasTrialStats) << entry.planStatsTree;
            ASSERT_FALSE(entry.planStatsTree["statistics"].Obj().hasField("multiPlan"))
                << entry.planStatsTree;

            const double cost = costBased.Obj()["costEstimate"].numberDouble();
            if (previousCost) {
                ASSERT_GTE(cost, *previousCost) << entry.planStatsTree;
            }
            previousCost = cost;
        }
    }
}

TEST_F(PlanExplainerTest, LegacyAccessorsMatchPlanEntriesAcrossVerbosities) {
    // The legacy winning/rejected accessors and the kLegacy
    // per-plan enumerator produce BSON-identical output for every legacy verbosity across
    // single-plan, multi-planned, and CBR-rejected scenarios. First verified against the
    // pre-consolidation accessor implementations, it now pins the consolidation (the accessors
    // are thin wrappers over getPlanEntries) to byte-identity.
    auto assertAccessorsMatchEntries = [&](PlanExecutor* exec) {
        auto& explainer = exec->getPlanExplainer();
        for (auto verbosity : {ExplainOptions::Verbosity::kQueryPlanner,
                               ExplainOptions::Verbosity::kExecStats,
                               ExplainOptions::Verbosity::kExecAllPlans,
                               ExplainOptions::Verbosity::kInternal}) {
            auto entries = explainer.getPlanEntries(
                explainPolicyFor(verbosity), PlanStatsFormat::kLegacy, PlanRankerMethod::kNone);
            ASSERT_GTE(entries.size(), 1u);

            auto&& [winningPlan, winningSummary] = explainer.getWinningPlanStats(verbosity);
            ASSERT_BSONOBJ_EQ(entries[0].planStatsTree, winningPlan);
            ASSERT_EQ(entries[0].summary.has_value(), winningSummary.has_value());

            auto rejected = explainer.getRejectedPlansStats(verbosity);
            ASSERT_EQ(rejected.size(), entries.size() - 1);
            for (size_t i = 0; i < rejected.size(); ++i) {
                ASSERT_BSONOBJ_EQ(entries[i + 1].planStatsTree, rejected[i].first);
                ASSERT_EQ(entries[i + 1].summary.has_value(), rejected[i].second.has_value());
                if (rejected[i].second) {
                    ASSERT_EQ(entries[i + 1].summary->score.has_value(),
                              rejected[i].second->score.has_value());
                }
            }
        }
    };

    {
        // Single-plan scenario.
        auto exec = buildFindExecAndIter(fromjson("{c: {$eq: 1}}"));
        assertAccessorsMatchEntries(exec.get());
    }
    {
        // Multi-planned scenario.
        auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
        assertAccessorsMatchEntries(exec.get());
    }
    {
        // CBR scenario with rejected plans that never ran a trial.
        unittest::ServerParameterGuard planRankerController("internalQueryPlanRanker", "costBased");
        unittest::ServerParameterGuard samplingController("internalQueryCBRCEMode", "samplingCE");
        expCtx->setExplain(ExplainOptions::Verbosity::kQueryPlanner);
        auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
        assertAccessorsMatchEntries(exec.get());
    }
}

TEST_F(PlanExplainerTest, V3ExecStatsSectionMatchesLegacyExecStatsSection) {
    // Serializing the same executor state at the V3 execStats and the legacy executionStats
    // verbosities must produce identical executionStats sections - V3's retained section is
    // generated by the same code path at the kExecStats policy by design, so a future fork of that
    // path (e.g. a "V3-ification" of executionStages) fails here rather than shipping silently.
    // Only the wall-clock totals, which generateExecutionInfo() reads from the operation timer at
    // serialization time, are excluded from the comparison. The jstest explain_exec_stats_parity.js
    // carries the system-level form of the guarantee.
    //
    // Plan under explain, as the explain command does before serializing at any V3 verbosity: the
    // ranking strategies record rankerChoice.reason only for explain-planned queries, and V3
    // emission tasserts (13237700) if a strategy decided the winner but no reason was carried.
    expCtx->setExplain(ExplainOptions::Verbosity::kExecStatsV3);
    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    while (exec->getNext(nullptr, nullptr) != PlanExecutor::IS_EOF) {
    }

    auto coll = acquireCollection(operationContext(),
                                  CollectionAcquisitionRequest::fromOpCtx(
                                      operationContext(), kNss, AcquisitionPrerequisites::kRead),
                                  MODE_IS);
    MultipleCollectionAccessor colls{coll};

    auto explainExecutionStatsAt = [&](ExplainOptions::Verbosity verbosity) {
        BSONObjBuilder bob;
        Explain::explainStages(exec.get(),
                               colls,
                               verbosity,
                               Status::OK(),
                               exec->getPlanExplainer().getWinningPlanTrialStats(),
                               BSONObj(),
                               SerializationContext::stateCommandReply(),
                               BSONObj(),
                               &bob);
        const BSONObj explained = bob.obj();
        ASSERT(explained["executionStats"].isABSONObj()) << explained;
        // Strip the wall-clock totals (see the comment above); everything else must be equal.
        return explained["executionStats"].Obj().removeFields(
            StringDataSet{"executionTimeMillis", "executionTimeMicros"});
    };

    const BSONObj legacySection = explainExecutionStatsAt(ExplainOptions::Verbosity::kExecStats);
    const BSONObj v3Section = explainExecutionStatsAt(ExplainOptions::Verbosity::kExecStatsV3);
    ASSERT_BSONOBJ_EQ(legacySection, v3Section);
}

TEST_F(PlanExplainerTest, SBEMultiPlannerExplain) {
    // Same as above, but with SBE enabled.
    unittest::ServerParameterGuard sbeFullController("featureFlagSbeFull", true);
    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    auto& explainer = exec->getPlanExplainer();

    ASSERT(explainer.areThereRejectedPlansToExplain());
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
    unittest::ServerParameterGuard sbeFullController("featureFlagSbeFull", true);
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

    ASSERT(!explainer.areThereRejectedPlansToExplain());
    auto&& [winningPlan, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "EXPRESS_IXSCAN");
    ASSERT(!winningPlan.hasField("slotBasedPlan"));
}

TEST_F(PlanExplainerTest, ExpressPlanSingleFieldEqExplain) {
    // Same as above, but testing non-_id express plan.
    auto exec = buildFindExecAndIter(fromjson("{a: 1}"), true /* limitOne */);
    auto& explainer = exec->getPlanExplainer();

    ASSERT(!explainer.areThereRejectedPlansToExplain());
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

TEST_F(PlanExplainerTest, ExpressPlanExecStatsIncludeExecutionTime) {
    // When explain is set on the ExpressionContext, the express executor should collect per-stage
    // timing and the explain output at kExecStats verbosity should include the
    // 'executionTimeMillisEstimate' field.
    expCtx->setExplain(ExplainOptions::Verbosity::kExecStats);
    auto exec = buildFindExecAndIter(fromjson("{_id: 1}"));
    auto& explainer = exec->getPlanExplainer();

    auto&& [winningPlan, summary] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "EXPRESS_IXSCAN");
    ASSERT(winningPlan.hasField("executionTimeMillisEstimate"))
        << "stage output missing executionTimeMillisEstimate: " << winningPlan.toString();
    ASSERT_EQ(summary->executionTime.precision, QueryExecTimerPrecision::kMillis);
}

TEST_F(PlanExplainerTest, ExpressPlanExecStatsIncludeNanoExecutionTime) {
    // With the nanosecond-precision knob enabled, the express stage output should expose
    // 'executionTimeMicros' and 'executionTimeNanos' alongside the millisecond estimate.
    unittest::ServerParameterGuard nanosController("internalMeasureQueryExecutionTimeInNanoseconds",
                                                   true);
    expCtx->setExplain(ExplainOptions::Verbosity::kExecStats);
    auto exec = buildFindExecAndIter(fromjson("{_id: 1}"));
    auto& explainer = exec->getPlanExplainer();

    auto&& [winningPlan, summary] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
    ASSERT_STRING_CONTAINS(winningPlan.toString(), "EXPRESS_IXSCAN");
    ASSERT(winningPlan.hasField("executionTimeMillisEstimate")) << winningPlan.toString();
    ASSERT(winningPlan.hasField("executionTimeMicros")) << winningPlan.toString();
    ASSERT(winningPlan.hasField("executionTimeNanos")) << winningPlan.toString();
    ASSERT_EQ(summary->executionTime.precision, QueryExecTimerPrecision::kNanos);
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
    unittest::ServerParameterGuard sbeFullController("featureFlagSbeFull", true);
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
    checkPipelinePlanExplainDiagnostics(exec.get(), false);
}

TEST_F(PlanExplainerTest, SBEPipelinePlanExplainDiagnostics) {
    // Same as above, with SBE enabled.
    unittest::ServerParameterGuard sbeFullController("featureFlagSbeFull", true);
    auto stages =
        std::vector{fromjson("{$match: {a: {$gte: 0}, b: {$gte: 0}}}"),
                    fromjson("{$redact: {$cond: {if: '$a', then: '$$PRUNE', else: '$$DESCEND'}}}")};
    auto exec = buildAggExecAndIter(stages);
    checkPipelinePlanExplainDiagnostics(exec.get(), true);
}

// Helper function to make a collection scan node.
auto makeCollScanNode(const std::string& collName) {
    auto node = std::make_unique<CollectionScanNode>();
    node->nss = NamespaceString::createNamespaceString_forTest(collName);
    return node;
}

namespace {
RecordIdRange makeIntRange(int min, bool minInclusive, int max, bool maxInclusive) {
    RecordIdRange r;
    r.maybeNarrowMin(RecordIdBound(RecordId(min)), minInclusive);
    r.maybeNarrowMax(RecordIdBound(RecordId(max)), maxInclusive);
    return r;
}

BSONObj callStatsToBSON(const QuerySolutionNode* node) {
    BSONObjBuilder bob;
    statsToBSON(node, &bob, &bob);
    return bob.obj();
}
}  // namespace

TEST_F(PlanExplainerTest, HashJoinEmbeddingTest) {
    auto outerScanNode = makeCollScanNode("testdb.explain");
    auto innerScanNode = makeCollScanNode("testdb.foreign_explain");
    auto hjNode = std::make_unique<HashJoinEmbeddingNode>(
        std::move(outerScanNode),
        std::move(innerScanNode),
        std::vector<QSNJoinPredicate>{QSNJoinPredicate{.op = QSNJoinPredicate::ComparisonOp::Eq,
                                                       .leftField = FieldPath("a"),
                                                       .rightField = FieldPath("b")}},
        FieldPath("xyz") /* leftFieldEmbedding */,
        boost::none /* rightFieldEmbedding */);

    BSONObjBuilder outerBob;
    BSONObjBuilder innerBob(outerBob.subobjStart("inner"));
    innerBob.done();
    statsToBSON(hjNode.get(), &innerBob, &outerBob);

    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "inner": {},
            "stage": "HASH_JOIN_EMBEDDING",
            "planNodeId": 0,
            "leftEmbeddingField": "xyz",
            "rightEmbeddingField": "none",
            "joinPredicates": [
                "a = b"
            ],
            "inputStages": [
                {
                    "stage": "COLLSCAN",
                    "planNodeId": 0,
                    "nss": "testdb.explain",
                    "direction": "forward"
                },
                {
                    "stage": "COLLSCAN",
                    "planNodeId": 0,
                    "nss": "testdb.foreign_explain",
                    "direction": "forward"
                }
            ]
        })",
        outerBob.obj());
}

TEST_F(PlanExplainerTest, NLJEmbeddingTest) {
    auto outerScanNode = makeCollScanNode("testdb.explain");
    auto innerScanNode = makeCollScanNode("testdb.foreign_explain");
    auto nljNode = std::make_unique<NestedLoopJoinEmbeddingNode>(
        std::move(outerScanNode),
        std::move(innerScanNode),
        std::vector<QSNJoinPredicate>{QSNJoinPredicate{.op = QSNJoinPredicate::ComparisonOp::Eq,
                                                       .leftField = FieldPath("x"),
                                                       .rightField = FieldPath("y")}},
        boost::none /* leftFieldEmbedding */,
        FieldPath("xyz" /* rightFieldEmbedding */));

    BSONObjBuilder outerBob;
    BSONObjBuilder innerBob(outerBob.subobjStart("inner"));
    innerBob.done();
    statsToBSON(nljNode.get(), &innerBob, &outerBob);

    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "inner": {},
            "stage": "NESTED_LOOP_JOIN_EMBEDDING",
            "planNodeId": 0,
            "leftEmbeddingField": "none",
            "rightEmbeddingField": "xyz",
            "joinPredicates": [
                "x = y"
            ],
            "inputStages": [
                {
                    "stage": "COLLSCAN",
                    "planNodeId": 0,
                    "nss": "testdb.explain",
                    "direction": "forward"
                },
                {
                    "stage": "COLLSCAN",
                    "planNodeId": 0,
                    "nss": "testdb.foreign_explain",
                    "direction": "forward"
                }
            ]
        })",
        outerBob.obj());
}

TEST_F(PlanExplainerTest, INLJEmbeddingTest) {
    auto outerScanNode = makeCollScanNode("testdb.explain");
    auto innerScanNode = makeCollScanNode("testdb.foreign_explain");
    auto inljNode = std::make_unique<IndexedNestedLoopJoinEmbeddingNode>(
        std::move(outerScanNode),
        std::move(innerScanNode),
        std::vector<QSNJoinPredicate>{QSNJoinPredicate{.op = QSNJoinPredicate::ComparisonOp::Eq,
                                                       .leftField = FieldPath("x"),
                                                       .rightField = FieldPath("y")}},
        boost::none /* leftFieldEmbedding */,
        FieldPath("xyz" /* rightFieldEmbedding */));

    BSONObjBuilder outerBob;
    BSONObjBuilder innerBob(outerBob.subobjStart("inner"));
    innerBob.done();
    statsToBSON(inljNode.get(), &innerBob, &outerBob);

    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "inner": {},
            "stage": "INDEXED_NESTED_LOOP_JOIN_EMBEDDING",
            "planNodeId": 0,
            "leftEmbeddingField": "none",
            "rightEmbeddingField": "xyz",
            "joinPredicates": [
                "x = y"
            ],
            "inputStages": [
                {
                    "stage": "COLLSCAN",
                    "planNodeId": 0,
                    "nss": "testdb.explain",
                    "direction": "forward"
                },
                {
                    "stage": "COLLSCAN",
                    "planNodeId": 0,
                    "nss": "testdb.foreign_explain",
                    "direction": "forward"
                }
            ]
        })",
        outerBob.obj());
}

TEST_F(PlanExplainerTest, PlanExplainerDataMergeEmpty) {
    PlanExplainerData data1;
    data1.rejectedPlansWithStages.push_back({nullptr, nullptr});

    PlanExplainerData data2;
    data2.planStageQsnMap.emplace(nullptr, nullptr);

    data1 << std::move(data2);

    ASSERT_EQ(data1.rejectedPlansWithStages.size(), 1);
    ASSERT_EQ(data1.planStageQsnMap.size(), 1);
}

TEST_F(PlanExplainerTest, OptionalPlanExplainerDataMerge) {
    boost::optional<PlanExplainerData> data1;
    boost::optional<PlanExplainerData> data2;

    // Both empty - stays empty
    data1 << std::move(data2);
    ASSERT_FALSE(data1);

    // LHS empty, RHS has value - merged has value
    data2.emplace();
    data2->rejectedPlansWithStages.push_back({nullptr, nullptr});
    data1 << std::move(data2);
    ASSERT_TRUE(data1);
    ASSERT_EQ(data1->rejectedPlansWithStages.size(), 1);

    // Both have values - merges content
    boost::optional<PlanExplainerData> data3;
    data3.emplace();
    data3->planStageQsnMap.emplace(nullptr, nullptr);

    data1 << std::move(data3);
    ASSERT_EQ(data1->rejectedPlansWithStages.size(), 1);
    ASSERT_EQ(data1->planStageQsnMap.size(), 1);
}

TEST_F(PlanExplainerTest, PlanExplainerDataMergeFull) {
    PlanExplainerData data1;
    auto qsn1 = std::make_unique<QuerySolution>();
    data1.rejectedPlansWithStages.push_back({std::move(qsn1), nullptr});
    // Use distinct pointer values to avoid key collision
    data1.planStageQsnMap.emplace(reinterpret_cast<const PlanStage*>(0x1), nullptr);
    data1.estimates.emplace(reinterpret_cast<const QuerySolutionNode*>(0x1),
                            std::make_unique<cost_based_ranker::QSNEstimate>());

    PlanExplainerData data2;
    auto qsn2 = std::make_unique<QuerySolution>();
    data2.rejectedPlansWithStages.push_back({std::move(qsn2), nullptr});
    data2.planStageQsnMap.emplace(reinterpret_cast<const PlanStage*>(0x2), nullptr);
    data2.estimates.emplace(reinterpret_cast<const QuerySolutionNode*>(0x2),
                            std::make_unique<cost_based_ranker::QSNEstimate>());

    data1 << std::move(data2);

    ASSERT_EQ(data1.rejectedPlansWithStages.size(), 2);
    ASSERT(data1.rejectedPlansWithStages[0].solution != nullptr);
    ASSERT(data1.rejectedPlansWithStages[1].solution != nullptr);
    ASSERT_EQ(data1.planStageQsnMap.size(), 2);
    ASSERT_EQ(data1.estimates.size(), 2);
}

TEST_F(PlanExplainerTest, CBRSamplingMetadataSerializedInExplain) {
    // Verify that when CBR uses sampling CE, the 'ceSamplingMetadata' section appears in the
    // queryPlanner explain output and contains the expected fields for each collection.
    unittest::ServerParameterGuard planRankerController("internalQueryPlanRanker", "costBased");
    unittest::ServerParameterGuard samplingController("internalQueryCBRCEMode", "samplingCE");

    const auto verbosity = ExplainOptions::Verbosity::kQueryPlanner;
    expCtx->setExplain(verbosity);

    auto coll = acquireCollection(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(
            operationContext(), kNss, AcquisitionPrerequisites::OperationType::kRead),
        MODE_IS);
    MultipleCollectionAccessor colls{coll};

    auto findCommand = std::make_unique<FindCommandRequest>(kNss);
    findCommand->setFilter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));
    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = expCtx,
        .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});

    Command* cmd = CommandHelpers::findCommand(operationContext(), "find");
    {
        std::lock_guard<Client> clientLock(*operationContext()->getClient());
        CurOp::get(operationContext())
            ->setGenericOpRequestDetails(clientLock, kNss, cmd, BSONObj(), NetworkOp::dbQuery);
    }

    auto swExec = getExecutorFind(
        operationContext(), colls, std::move(cq), PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);
    ASSERT_OK(swExec);

    BSONObjBuilder bob;
    Explain::explainStages(swExec.getValue().get(),
                           colls,
                           verbosity,
                           Status::OK(),
                           boost::none,
                           BSONObj(),
                           SerializationContext::stateCommandReply(),
                           BSONObj(),
                           &bob);
    const BSONObj explained = bob.obj();

    auto queryPlanner = explained["queryPlanner"];
    ASSERT(queryPlanner.isABSONObj()) << "Missing queryPlanner in: " << explained;

    auto ceSamplingMeta = queryPlanner["ceSamplingMetadata"];
    ASSERT(ceSamplingMeta.isABSONObj())
        << "Missing ceSamplingMetadata in queryPlanner: " << queryPlanner;

    // Exactly one namespace entry expected.
    ASSERT_EQ(ceSamplingMeta.Obj().nFields(), 1) << ceSamplingMeta;
    const BSONElement nsElem = ceSamplingMeta.Obj().firstElement();
    ASSERT_EQ(nsElem.type(), BSONType::object);
    const BSONObj nsMeta = nsElem.Obj();

    ASSERT_EQ(nsMeta["sampleSource"].String(), "onTheFly");
    ASSERT(nsMeta.hasField("sampleTechnique")) << nsMeta;
    ASSERT(nsMeta.hasField("sampleDocCount")) << nsMeta;
    ASSERT(nsMeta.hasField("sampleRequestedDocCount")) << nsMeta;
    ASSERT(nsMeta.hasField("sampleMemorySizeBytes")) << nsMeta;
    ASSERT(!nsMeta.hasField("sampleNumPages")) << nsMeta;
}

TEST_F(PlanExplainerTest, CBRSamplingMetadataReportsPagesForPersistedSample) {
    // Verifies that the page count shows up in 'ceSamplingMetadata' when read back
    // from persisted samples collection.
    // TODO SERVER-112627: Remove once featureFlagPersistentStats is enabled by default.
    unittest::ServerParameterGuard persistentStatsFlag("featureFlagPersistentStats", true);
    unittest::ServerParameterGuard planRankerController("internalQueryPlanRanker", "costBased");
    unittest::ServerParameterGuard samplingController("internalQueryCBRCEMode", "samplingCE");
    // Requested sample size is part of a persisted sample's key, so pin it to avoid full coll scan.
    constexpr int kPersistedSampleSize = 10;
    unittest::ServerParameterGuard sampleSizeOverride("internalSamplingSizeOverride",
                                                      kPersistedSampleSize);

    const UUID collUuid = [&] {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), kNss, AcquisitionPrerequisites::OperationType::kRead),
            MODE_IS);
        return coll.getCollectionPtr()->uuid();
    }();

    std::vector<BSONObj> sample;
    for (int i = 0; i < kPersistedSampleSize; ++i) {
        sample.push_back(BSON("_id" << i << "a" << i << "b" << i));
    }

    const std::vector<BSONObj> pages =
        ce::makePersistentSamplePageDocs(collUuid,
                                         ce::SamplingTechniqueEnum::kRandom,
                                         kPersistedSampleSize,
                                         boost::none /* numChunks */,
                                         sample,
                                         Date_t::now());
    ASSERT_EQ(pages.size(), 1u);

    ce::createCollAndInsertDocuments(
        operationContext(),
        NamespaceString::createNamespaceString_forTest(kNss.dbName(), ce::kSamplesCollectionName),
        pages,
        /*clustered=*/true);

    const auto verbosity = ExplainOptions::Verbosity::kQueryPlanner;
    expCtx->setExplain(verbosity);
    auto exec = buildFindExecAndIter(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));

    auto coll = acquireCollection(operationContext(),
                                  CollectionAcquisitionRequest::fromOpCtx(
                                      operationContext(), kNss, AcquisitionPrerequisites::kRead),
                                  MODE_IS);
    MultipleCollectionAccessor colls{coll};

    BSONObjBuilder bob;
    Explain::explainStages(exec.get(),
                           colls,
                           verbosity,
                           Status::OK(),
                           boost::none,
                           BSONObj(),
                           SerializationContext::stateCommandReply(),
                           BSONObj(),
                           &bob);
    const BSONObj explained = bob.obj();

    auto queryPlanner = explained["queryPlanner"];
    ASSERT(queryPlanner.isABSONObj()) << "Missing queryPlanner in: " << explained;
    auto ceSamplingMeta = queryPlanner["ceSamplingMetadata"];
    ASSERT(ceSamplingMeta.isABSONObj())
        << "Missing ceSamplingMetadata in queryPlanner: " << queryPlanner;
    ASSERT_EQ(ceSamplingMeta.Obj().nFields(), 1) << ceSamplingMeta;
    const BSONObj nsMeta = ceSamplingMeta.Obj().firstElement().Obj();

    // Reading the persisted sample makes the page count available.
    ASSERT_EQ(nsMeta["sampleSource"].String(), "persisted") << nsMeta;
    ASSERT_EQ(nsMeta["sampleNumPages"].numberLong(), 1) << nsMeta;
}

TEST_F(PlanExplainerTest, GenerateQueryKnobsEmitsNothingWhenFeatureFlagOff) {
    unittest::ServerParameterGuard flagGuard("featureFlagPqsQueryKnobs", false);

    BSONObjBuilder bob;
    explain_common::generateQueryKnobs(expCtx, &bob);

    ASSERT_FALSE(bob.asTempObj().hasField("queryKnobs"));
}

TEST_F(PlanExplainerTest, GenerateQueryKnobsEmitsQuerySettingsKnobsWhenFeatureFlagOn) {
    auto* opCtx = operationContext();
    unittest::ServerParameterGuard flagGuard("featureFlagPqsQueryKnobs", true);
    query_settings::QuerySettingsGuardForTest settingsGuard(
        opCtx, fromjson(R"({queryKnobs: {samplingMarginOfError: 2.5}})"));

    auto testExpCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);

    BSONObjBuilder bob;
    explain_common::generateQueryKnobs(testExpCtx, &bob);
    auto result = bob.obj();

    ASSERT_TRUE(result.hasField("queryKnobs"));
    auto knobEntry = result["queryKnobs"]["samplingMarginOfError"].Obj();
    ASSERT_EQ(knobEntry["source"].String(), "querySettings");
    ASSERT_EQ(knobEntry["value"].Double(), 2.5);
}

TEST_F(PlanExplainerTest, GenerateQueryKnobsOmitsKnobsWhenOutputNearlyFull) {
    auto* opCtx = operationContext();
    unittest::ServerParameterGuard flagGuard("featureFlagPqsQueryKnobs", true);
    query_settings::QuerySettingsGuardForTest settingsGuard(
        opCtx, fromjson(R"({queryKnobs: {samplingMarginOfError: 2.5, cbrCEMode: "samplingCE"}})"));
    auto testExpCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);

    int knobsSize = 0;
    {
        BSONObjBuilder probe;
        explain_common::generateQueryKnobs(testExpCtx, &probe);
        knobsSize = probe.obj()["queryKnobs"].Obj().objsize();
    }

    // Mirror of the file-internal 'append_if_room::OutputObjectMaxSize' in explain_common.cpp.
    const int kOutputObjectMaxSize = BSONObjMaxUserSize - 10 * 1024;

    // Almost fill 'out' so the knobs blob no longer fits but the builder stays below the threshold,
    // leaving room for the truncation warning. 'kFillerOverhead' is what append() adds beyond the
    // string payload: element type, "filler\0", the string length prefix, and the NUL terminator.
    BSONObjBuilder out;
    constexpr int kFillerOverhead = 13;
    out.append("filler",
               std::string(kOutputObjectMaxSize - knobsSize - out.len() - kFillerOverhead, 'x'));

    explain_common::generateQueryKnobs(testExpCtx, &out);

    auto result = out.obj();
    ASSERT_FALSE(result.hasField("queryKnobs"));
    ASSERT_TRUE(result.hasField("warning"));
}

// A CollectionScanNode with two ranges must emit recordIdRanges with both entries.
TEST_F(PlanExplainerTest, CollScanMultiRangeIncludesRecordIdRanges) {
    auto csn = makeCollScanNode("testdb.col");
    csn->rangeList = RecordIdRangeList::makeUnion(
        {makeIntRange(1, true, 5, true), makeIntRange(10, false, 20, true)});

    auto result = callStatsToBSON(csn.get());
    ASSERT(result.hasField("recordIdRanges")) << result;
    ASSERT_BSONOBJ_EQ_AUTO(
        R"([{"min":1, "minInclusive": true, "max":5, "maxInclusive": true}, {"min":10, "minInclusive": false, "max": 20, "maxInclusive": true}])",
        BSONArray(result["recordIdRanges"].Obj()))
        << result;
    // Outer bounds are also emitted for backward compatibility.
    ASSERT_EQ(result["minRecord"].Long(), 1) << result;
    ASSERT_EQ(result["maxRecord"].Long(), 20) << result;
}

// An empty rangeList (∅, no ranges) also emits recordIdRanges (as an empty array).
// In this case, both minRecord and maxRecord are set to null.
TEST_F(PlanExplainerTest, CollScanEmptyRangeListIncludesRecordIdRanges) {
    auto csn = makeCollScanNode("testdb.col");
    csn->rangeList = RecordIdRangeList::makeUnion({});  // explicit empty list

    auto result = callStatsToBSON(csn.get());
    ASSERT(result.hasField("recordIdRanges")) << result;
    ASSERT_EQ(result["recordIdRanges"].Array().size(), 0u) << result;
    ASSERT(result["minRecord"].isNull()) << result;
    ASSERT(result["maxRecord"].isNull()) << result;
}

}  // namespace
}  // namespace mongo
