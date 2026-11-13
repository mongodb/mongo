// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/parallelism_planner/parallelism_planner.h"

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/optimizer/parallelism_planner/parallel_planner_test_utils.h"
#include "mongo/db/query/compiler/optimizer/parallelism_planner/required_distribution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/query_knobs/query_knob_configuration_test_util.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <memory>
#include <vector>

namespace mongo::parallelism_planner {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.parallel");
const NamespaceString kMissingNss = NamespaceString::createNamespaceString_forTest("test.missing");

class ParallelPlannerTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions()));
    }

    struct DefaultPQConfig {
        explicit DefaultPQConfig(OperationContext* opCtx)
            : enable(opCtx, "internalQueryEnableParallelQuery", true),
              maxDoP(opCtx, "internalQueryMaxDegreeOfParallelism", kMaxDoPForTests),
              minSize(opCtx, "internalQueryParallelQueryMinCollectionSizeBytes", 0) {}

        QueryKnobGuardForTest enable;
        QueryKnobGuardForTest maxDoP;
        QueryKnobGuardForTest minSize;
    };

    DefaultPQConfig defaultPQConfig() {
        return DefaultPQConfig(operationContext());
    }

    static std::unique_ptr<QuerySolution> collScanSolution(
        std::function<void(CollectionScanNode&)> mutate = nullptr) {
        auto node = std::make_unique<CollectionScanNode>();
        if (mutate) {
            mutate(*node);
        }
        auto solution = std::make_unique<QuerySolution>();
        solution->setRoot(std::move(node));
        return solution;
    }


    static void assertEveryEdgeSatisfiesItsRequirement(const QuerySolutionNode& node) {
        const auto reqs = childRequirements(node);
        ASSERT_EQ(reqs.size(), node.children.size());
        for (size_t i = 0; i < reqs.size(); ++i) {
            ASSERT_TRUE(satisfies(node.children[i]->physicalProps.distribution, reqs[i]))
                << nodeStageTypeToString(&node) << " child " << i;
            assertEveryEdgeSatisfiesItsRequirement(*node.children[i]);
        }
    }

    // Whether query settings index hints were applied. Tests set this before planning.
    bool querySettingsApplied = false;

    std::unique_ptr<QuerySolution> addParallelPlan(
        std::unique_ptr<QuerySolution> solution,
        std::function<void(FindCommandRequest&)> mutateFind = nullptr,
        const NamespaceString& nss = kNss) {
        auto expCtx = make_intrusive<ExpressionContextForTest>(operationContext(), nss);
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        if (mutateFind) {
            mutateFind(*findCommand);
        }
        CanonicalQuery cq(CanonicalQueryParams{
            .expCtx = expCtx,
            .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});

        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), nss, AcquisitionPrerequisites::OperationType::kRead),
            MODE_IS);
        MultipleCollectionAccessor collections(
            std::move(coll), {}, /*isAnySecondaryNamespaceAViewOrNotFullyLocal=*/false);
        return parallelism_planner::addParallelPlan(
            cq, collections, querySettingsApplied, std::move(solution));
    }

    bool isEligible(std::unique_ptr<QuerySolution> solution,
                    std::function<void(FindCommandRequest&)> mutateFind = nullptr,
                    const NamespaceString& nss = kNss) {
        solution = addParallelPlan(std::move(solution), std::move(mutateFind), nss);
        return solution->hasParallelRoot();
    }
};

TEST_F(ParallelPlannerTest, CollectionScanIsGatheredAtTheRoot) {
    auto enabled = defaultPQConfig();
    const auto solution = addParallelPlan(collScanSolution());

    ASSERT_TRUE(solution->hasParallelRoot());
    const auto* root = solution->parallelRoot();
    ASSERT_EQ(root->getType(), STAGE_EXCHANGE);
    const auto& gather = static_cast<const ExchangeNode&>(*root);
    ASSERT_TRUE(gather.isGather());
    ASSERT_FALSE(std::get<ExchangeRouting::Gather>(gather.routing.mode).sortPreserving);
    ASSERT_EQ(gather.physicalProps.distribution.type, DistributionDesc::Type::Unpartitioned);

    const auto* collScan = gather.children[0].get();
    ASSERT_EQ(collScan->getType(), STAGE_COLLSCAN);
    ASSERT_EQ(collScan->physicalProps.distribution.type, DistributionDesc::Type::RoundRobin);
}

TEST_F(ParallelPlannerTest, PassThroughNodesStayInsideTheParallelSection) {
    auto enabled = defaultPQConfig();
    const auto solution = addParallelPlan(solutionWithRoot(matchOver(scan())));

    // GATHER -> MATCH -> COLLSCAN: the filter runs below the gather.
    const auto* root = solution->parallelRoot();
    ASSERT_EQ(root->getType(), STAGE_EXCHANGE);
    const auto* filter = root->children[0].get();
    ASSERT_EQ(filter->getType(), STAGE_MATCH);
    ASSERT_EQ(filter->physicalProps.distribution.type, DistributionDesc::Type::RoundRobin);
    ASSERT_EQ(filter->children[0]->getType(), STAGE_COLLSCAN);
}

TEST_F(ParallelPlannerTest, EveryEdgeSatisfiesItsRequirementAfterPlanning) {
    auto enabled = defaultPQConfig();
    std::vector<std::unique_ptr<QuerySolution>> solutions;
    solutions.push_back(collScanSolution());
    solutions.push_back(solutionWithRoot(matchOver(scan())));
    solutions.push_back(solutionWithRoot(matchOver(matchOver(scan()))));
    for (auto& serial : solutions) {
        const auto solution = addParallelPlan(std::move(serial));
        ASSERT_TRUE(solution->hasParallelRoot());
        assertEveryEdgeSatisfiesItsRequirement(*solution->parallelRoot());
    }
}

TEST_F(ParallelPlannerTest, PlainCollectionScanPassesEveryGate) {
    auto enabled = defaultPQConfig();
    ASSERT_TRUE(isEligible(collScanSolution()));
}

TEST_F(ParallelPlannerTest, DisabledByDefault) {
    ASSERT_FALSE(isEligible(collScanSolution()));
}

TEST_F(ParallelPlannerTest, RequiresEnableParallelQuery) {
    auto enabled = defaultPQConfig();
    QueryKnobGuardForTest enableOff{operationContext(), "internalQueryEnableParallelQuery", false};
    ASSERT_FALSE(isEligible(collScanSolution()));
}

TEST_F(ParallelPlannerTest, RequiresMaxDegreeOfParallelismAboveOne) {
    auto enabled = defaultPQConfig();
    QueryKnobGuardForTest serial{operationContext(), "internalQueryMaxDegreeOfParallelism", 1};
    ASSERT_FALSE(isEligible(collScanSolution()));
}

TEST_F(ParallelPlannerTest, AllowsPassThroughNodesAboveTheScan) {
    auto enabled = defaultPQConfig();
    ASSERT_TRUE(isEligible(solutionWithRoot(matchOver(scan()))));
    ASSERT_TRUE(isEligible(solutionWithRoot(matchOver(matchOver(scan())))));
}

TEST_F(ParallelPlannerTest, AnyIneligibleNodeMakesTheWholeTreeIneligible) {
    auto enabled = defaultPQConfig();
    ASSERT_FALSE(isEligible(solutionWithRoot(ineligibleOver(scan()))));
    // Below an eligible node, and not just at the root.
    ASSERT_FALSE(isEligible(solutionWithRoot(matchOver(ineligibleOver(scan())))));
}

TEST_F(ParallelPlannerTest, DeclinesSolutionsWithoutARoot) {
    auto enabled = defaultPQConfig();
    ASSERT_FALSE(isEligible(std::make_unique<QuerySolution>()));
}

TEST_F(ParallelPlannerTest, DeclinesQueriesWithALimit) {
    auto enabled = defaultPQConfig();
    // Planning represents a limit as a LimitNode, which is not parallel eligible.
    ASSERT_FALSE(isEligible(solutionWithRoot(
        std::make_unique<LimitNode>(scan(), 10, LimitSkipParameterization::Disabled))));
}

TEST_F(ParallelPlannerTest, DeclinesNaturalHint) {
    auto enabled = defaultPQConfig();
    ASSERT_FALSE(
        isEligible(collScanSolution(), [](auto& find) { find.setHint(BSON("$natural" << 1)); }));
    ASSERT_FALSE(
        isEligible(collScanSolution(), [](auto& find) { find.setHint(BSON("$natural" << -1)); }));
}

TEST_F(ParallelPlannerTest, DeclinesNaturalSort) {
    auto enabled = defaultPQConfig();
    ASSERT_FALSE(
        isEligible(collScanSolution(), [](auto& find) { find.setSort(BSON("$natural" << 1)); }));
}

TEST_F(ParallelPlannerTest, DeclinesWhenQuerySettingsAreApplied) {
    auto enabled = defaultPQConfig();
    querySettingsApplied = true;
    ASSERT_FALSE(isEligible(collScanSolution()));

    querySettingsApplied = false;
    ASSERT_TRUE(isEligible(collScanSolution()));
}

TEST_F(ParallelPlannerTest, DeclinesSortNodes) {
    auto enabled = defaultPQConfig();
    // Neither sort node is parallel eligible yet, so a sorted query stays serial.
    ASSERT_FALSE(isEligible(
        solutionWithRoot(std::make_unique<SortNodeDefault>(scan(),
                                                           BSON("a" << 1),
                                                           /*limit=*/0,
                                                           LimitSkipParameterization::Disabled,
                                                           /*maxMemoryUsageBytes=*/1024 * 1024))));
    ASSERT_FALSE(isEligible(
        solutionWithRoot(std::make_unique<SortNodeSimple>(scan(),
                                                          BSON("a" << 1),
                                                          /*limit=*/0,
                                                          LimitSkipParameterization::Disabled,
                                                          /*maxMemoryUsageBytes=*/1024 * 1024))));
}

TEST_F(ParallelPlannerTest, DeclinesPlansThatAreNotPlainCollectionScans) {
    auto enabled = defaultPQConfig();
    ASSERT_FALSE(isEligible(
        collScanSolution([](auto& scan) { scan.clusteredIndex = ClusteredIndexSpec{}; })));
    ASSERT_FALSE(isEligible(collScanSolution([](auto& scan) { scan.direction = -1; })));
    ASSERT_FALSE(isEligible(collScanSolution([](auto& scan) { scan.tailable = true; })));
}

TEST_F(ParallelPlannerTest, DeclinesCollectionsBelowTheMinimumSize) {
    auto enabled = defaultPQConfig();
    QueryKnobGuardForTest minSize{
        operationContext(), "internalQueryParallelQueryMinCollectionSizeBytes", 1};
    ASSERT_FALSE(isEligible(collScanSolution()));
}

TEST_F(ParallelPlannerTest, AcceptsCollectionsAtOrAboveTheMinimumSize) {
    auto enabled = defaultPQConfig();
    ASSERT_OK(storageInterface()->insertDocument(operationContext(),
                                                 kNss,
                                                 {BSON("_id" << 1 << "x" << 1)},
                                                 repl::OpTime::kUninitializedTerm));
    QueryKnobGuardForTest minSize{
        operationContext(), "internalQueryParallelQueryMinCollectionSizeBytes", 1};
    ASSERT_TRUE(isEligible(collScanSolution()));
}

TEST_F(ParallelPlannerTest, DeclinesMissingCollections) {
    auto enabled = defaultPQConfig();
    ASSERT_FALSE(isEligible(collScanSolution(), nullptr, kMissingNss));
}

TEST_F(ParallelPlannerTest, AttachesAParallelPlanAndKeepsTheSerialRoot) {
    auto enabled = defaultPQConfig();
    auto serial = collScanSolution();
    const auto serialRoot = serial->root();
    const auto serialHash = serial->hash();

    auto solution = addParallelPlan(std::move(serial));

    ASSERT_TRUE(solution->hasParallelRoot());
    ASSERT_EQ(solution->parallelRoot()->getType(), STAGE_EXCHANGE);
    ASSERT_EQ(solution->parallelRoot()->children[0]->getType(), STAGE_COLLSCAN);
    ASSERT_EQ(solution->root(), serialRoot);
    ASSERT_EQ(solution->root()->getType(), STAGE_COLLSCAN);
    ASSERT_EQ(solution->hash(), serialHash);
}

TEST_F(ParallelPlannerTest, DeclinedQueriesComeBackUnchanged) {
    auto serial = collScanSolution();
    const auto serialRoot = serial->root();

    auto solution = addParallelPlan(std::move(serial));

    ASSERT_FALSE(solution->hasParallelRoot());
    ASSERT_EQ(solution->root(), serialRoot);
}

TEST_F(ParallelPlannerTest, DeclinesSolutionsExtendedWithAPipeline) {
    auto enabled = defaultPQConfig();
    // A non-leading $match is pushed down as an extension and is a parallel eligible node.
    auto solution = collScanSolution();
    solution->extendWith(std::make_unique<MatchNode>(
        std::make_unique<SentinelNode>(), std::make_unique<AlwaysTrueMatchExpression>()));
    ASSERT_TRUE(solution->hasExtension());
    ASSERT_FALSE(isEligible(std::move(solution)));
}

}  // namespace
}  // namespace mongo::parallelism_planner
