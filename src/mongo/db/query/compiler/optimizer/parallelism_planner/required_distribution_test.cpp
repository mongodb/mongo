// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/parallelism_planner/required_distribution.h"

#include "mongo/db/query/compiler/optimizer/parallelism_planner/parallel_planner_test_utils.h"
#include "mongo/db/query/compiler/optimizer/parallelism_planner/parallelism_planner_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo::parallelism_planner {
namespace {

const ParallelismPlannerContext kParallelCtx{kMaxDoPForTests};

TEST(DistributionDescTest, BroadcastProducesReplicatedDistribution) {
    auto out = toOutputDistribution(ExchangeRouting::broadcast());
    ASSERT_EQ(out.type, DistributionType::Replicated);
    ASSERT_TRUE(out.keys.empty());
}

TEST(DistributionDescTest, GatherProducesUnpartitioned) {
    ASSERT_EQ(toOutputDistribution(ExchangeRouting::gather(true /*sortPreserving*/)).type,
              DistributionType::Unpartitioned);
    ASSERT_EQ(toOutputDistribution(ExchangeRouting::gather(false /*sortPreserving*/)).type,
              DistributionType::Unpartitioned);
}

TEST(DistributionDescTest, HashProducesHashPartitionedOnSameKeys) {
    auto out = toOutputDistribution(ExchangeRouting::hashOn({FieldPath("a"), FieldPath("b")}));
    ASSERT_EQ(out.type, DistributionType::HashPartitioned);
    ASSERT_EQ(out.keys.size(), 2u);
    ASSERT_EQ(out.keys[0].fullPath(), "a");
    ASSERT_EQ(out.keys[1].fullPath(), "b");
}

DEATH_TEST(DistributionDescDeathTest,
           HashOnRejectsEmptyKeys,
           "hash partitioning requires at least one key") {
    DistributionDesc::hashOn({});
}

DEATH_TEST(DistributionDescDeathTest,
           RangeOnRejectsEmptyKeys,
           "range partitioning requires at least one key") {
    DistributionDesc::rangeOn({});
}

DEATH_TEST(ExchangeNodeDeathTest,
           HashRoutingRejectsEmptyKeys,
           "hash partitioning requires at least one key") {
    // An exchange builds its output distribution from its routing, so it is rejected there too.
    ExchangeNode exchange(scan(), ExchangeRouting::hashOn({}));
}

TEST(DistributionDescTest, RoundRobinProducesRoundRobinWithNoKeys) {
    auto out = toOutputDistribution(ExchangeRouting::roundRobin());
    ASSERT_EQ(out.type, DistributionType::RoundRobin);
    ASSERT_TRUE(out.keys.empty());
}

TEST(DistributionDescTest, ToStringNamesTypeAndKeys) {
    ASSERT_EQ(toString(DistributionDesc::unpartitioned()), "unpartitioned");
    ASSERT_EQ(toString(DistributionDesc::replicated()), "replicated");
    ASSERT_EQ(toString(DistributionDesc::roundRobin()), "roundRobin");
    ASSERT_EQ(toString(DistributionDesc::hashOn({FieldPath("a"), FieldPath("b.c")})),
              "hashPartitioned[a, b.c]");
}

TEST(SatisfiesTest, AnyAcceptsEverything) {
    ASSERT_TRUE(satisfies(DistributionDesc::unpartitioned(), RequiredDistribution::any()));
    ASSERT_TRUE(satisfies(DistributionDesc::replicated(), RequiredDistribution::any()));
    ASSERT_TRUE(satisfies(DistributionDesc::roundRobin(), RequiredDistribution::any()));
}

TEST(SatisfiesTest, UnpartitionedAndReplicatedAreStrictBothWays) {
    ASSERT_FALSE(satisfies(DistributionDesc::unpartitioned(), RequiredDistribution::replicated()));
    ASSERT_FALSE(satisfies(DistributionDesc::replicated(), RequiredDistribution::unpartitioned()));
}

TEST(SatisfiesTest, HashPartitionedRequiresExactKeyMatch) {
    auto onA = DistributionDesc::hashOn({FieldPath("a")});
    ASSERT_TRUE(satisfies(onA, RequiredDistribution::hashOn({FieldPath("a")})));
    ASSERT_FALSE(satisfies(onA, RequiredDistribution::hashOn({FieldPath("b")})));
    ASSERT_FALSE(satisfies(onA, RequiredDistribution::hashOn({FieldPath("a"), FieldPath("b")})));
    ASSERT_FALSE(satisfies(DistributionDesc::hashOn({FieldPath("a"), FieldPath("b")}),
                           RequiredDistribution::hashOn({FieldPath("a")})));
    // RoundRobin carries no key guarantee, so it never satisfies a hash requirement.
    ASSERT_FALSE(
        satisfies(DistributionDesc::roundRobin(), RequiredDistribution::hashOn({FieldPath("a")})));
}

TEST(SatisfiesTest, EveryExchangeOutputSatisfiesTheRequirementItWasBuiltFor) {
    const auto keys = std::vector<FieldPath>{FieldPath("a")};
    ASSERT_TRUE(satisfies(toOutputDistribution(ExchangeRouting::gather(false)),
                          RequiredDistribution::unpartitioned()));
    ASSERT_TRUE(satisfies(toOutputDistribution(ExchangeRouting::broadcast()),
                          RequiredDistribution::replicated()));
    ASSERT_TRUE(satisfies(toOutputDistribution(ExchangeRouting::hashOn(keys)),
                          RequiredDistribution::hashOn(keys)));
}

TEST(RequiredDistributionTest, ToString) {
    ASSERT_EQ(toString(RequiredDistribution::any()), "any()");
    ASSERT_EQ(toString(RequiredDistribution::unpartitioned()), "unpartitioned()");
    ASSERT_EQ(toString(RequiredDistribution::replicated()), "replicated()");
    ASSERT_EQ(toString(RequiredDistribution::hashOn({FieldPath("a"), FieldPath("b")})),
              "hashOn[a, b]");
}

//
// ExchangeNode
//

TEST(ExchangeNodeTest, StageTypeIsExchange) {
    ExchangeNode exchange(scan(), ExchangeRouting::gather(false));
    ASSERT_EQ(exchange.getType(), STAGE_EXCHANGE);
}

TEST(ExchangeNodeTest, OutputDistributionFollowsRoutingFromConstruction) {
    ASSERT_EQ(ExchangeNode(scan(), ExchangeRouting::gather(false)).distribution(),
              DistributionDesc::unpartitioned());
    ASSERT_EQ(ExchangeNode(scan(), ExchangeRouting::broadcast()).distribution(),
              DistributionDesc::replicated());
    ASSERT_EQ(ExchangeNode(scan(), ExchangeRouting::roundRobin()).distribution(),
              DistributionDesc::roundRobin());
    ASSERT_EQ(ExchangeNode(scan(), ExchangeRouting::hashOn({FieldPath("a")})).distribution(),
              DistributionDesc::hashOn({FieldPath("a")}));
}

TEST(ExchangeNodeTest, CloneRoundTrips) {
    auto exchange =
        std::make_unique<ExchangeNode>(scan(), ExchangeRouting::hashOn({FieldPath("a")}));

    auto copy = exchange->clone();
    const auto* asExch = static_cast<const ExchangeNode*>(copy.get());
    ASSERT_EQ(asExch->getType(), STAGE_EXCHANGE);
    const auto& partition = std::get<ExchangeRouting::Partition>(asExch->routing.mode);
    ASSERT_EQ(partition.type, ExchangePartitionType::Hash);
    ASSERT_EQ(partition.keys[0].fullPath(), "a");
    ASSERT_EQ(asExch->distribution(), exchange->distribution());
    ASSERT_EQ(asExch->children.size(), 1u);
}

TEST(ExchangeNodeTest, AppendToStringDistinguishesTheThreeRoutingKinds) {
    auto gather = std::make_unique<ExchangeNode>(scan(), ExchangeRouting::gather(false));
    ASSERT_STRING_CONTAINS(gather->toString(), "gather");

    auto broadcast = std::make_unique<ExchangeNode>(scan(), ExchangeRouting::broadcast());
    ASSERT_STRING_CONTAINS(broadcast->toString(), "broadcast");

    auto repartition =
        std::make_unique<ExchangeNode>(scan(), ExchangeRouting::hashOn({FieldPath("a")}));
    ASSERT_STRING_CONTAINS(repartition->toString(), "hash");
    ASSERT_STRING_CONTAINS(repartition->toString(), "a");

    // The common properties are indented like any other node's.
    ASSERT_STRING_CONTAINS(gather->toString(), "\n---nodeId = ");
}

TEST(ExchangeNodeTest, OnlyASortPreservingGatherKeepsTheChildsSortOrder) {
    // An index scan on {a: 1}, which provides that sort order. Each exchange takes ownership of its
    // child, so every use gets a fresh one.
    auto sortedScan = [] {
        auto node = std::make_unique<IndexScanNode>(
            NamespaceString::createNamespaceString_forTest("test.dummy"),
            buildSimpleIndexEntry(BSON("a" << 1)));
        node->computeProperties();
        return node;
    };
    ASSERT_BSONOBJ_EQ(sortedScan()->providedSorts().getBaseSortPattern(), BSON("a" << 1));

    ExchangeNode preserving(sortedScan(), ExchangeRouting::gather(true));
    ASSERT_BSONOBJ_EQ(preserving.providedSorts().getBaseSortPattern(), BSON("a" << 1));

    // Other routing types discard the provided sort.
    for (auto routing : {ExchangeRouting::gather(false),
                         ExchangeRouting::broadcast(),
                         ExchangeRouting::hashOn({FieldPath("a")}),
                         ExchangeRouting::roundRobin()}) {
        ExchangeNode exchange(sortedScan(), routing);
        ASSERT_TRUE(exchange.providedSorts().getBaseSortPattern().isEmpty()) << toString(routing);
    }
}

TEST(ExchangeNodeTest, ReportsWhetherItIsAGather) {
    ExchangeNode repartition(scan(), ExchangeRouting::hashOn({FieldPath("a")}));
    ASSERT_TRUE(repartition.providedSorts().getBaseSortPattern().isEmpty());
    ASSERT_FALSE(repartition.isGather());

    ExchangeNode gather(scan(), ExchangeRouting::gather(true));
    ASSERT_TRUE(gather.isGather());
    ASSERT_FALSE(gather.sortedByDiskLoc());
}

TEST(ExchangeNodeTest, ForwardsFetchedAndFieldAvailabilityFromChild) {
    ExchangeNode exchange(scan(), ExchangeRouting::gather(false));
    ASSERT_TRUE(exchange.fetched());
    ASSERT_TRUE(exchange.getFieldAvailability("a") == FieldAvailability::kFullyProvided);
}

//
// Tests for planDistribution() and insertExchangeFor().
//

DEATH_TEST_REGEX(PlanDistributionDeathTest, IneligibleNodeIsRejected, "13623108") {
    IneligibleNode node(scan());
    planDistribution(node, kParallelCtx);
}

TEST(PlanDistributionTest, ScanIsRoundRobin) {
    auto root = scan();
    planDistribution(*root, kParallelCtx);
    ASSERT_EQ(root->distribution().type, DistributionType::RoundRobin);
}

TEST(PlanDistributionTest, NoExchangeInsertedWhenRequirementAlreadySatisfied) {
    auto root = matchOver(scan());
    planDistribution(*root, kParallelCtx);
    ASSERT_EQ(root->distribution().type, DistributionType::RoundRobin);
    ASSERT_EQ(root->children[0]->getType(), STAGE_COLLSCAN);
}

TEST(PlanDistributionTest, ExchangeSetsItsOwnDistributionToMatchItsRouting) {
    auto exchange = insertExchangeFor(scan(), RequiredDistribution::hashOn({FieldPath("a")}));
    ASSERT_EQ(exchange->distribution(), DistributionDesc::hashOn({FieldPath("a")}));
    exchange = insertExchangeFor(scan(), RequiredDistribution::replicated());
    ASSERT_EQ(exchange->distribution(), DistributionDesc::replicated());
    exchange = insertExchangeFor(scan(), RequiredDistribution::unpartitioned());
    ASSERT_EQ(exchange->distribution(), DistributionDesc::unpartitioned());
}

TEST(PlanDistributionTest, GatherIsNeverSortPreserving) {
    auto gather = insertExchangeFor(scan(), RequiredDistribution::unpartitioned());
    ASSERT_FALSE(std::get<ExchangeRouting::Gather>(
                     static_cast<const ExchangeNode*>(gather.get())->routing.mode)
                     .sortPreserving);
}

DEATH_TEST_REGEX(PlanDistributionDeathTest, GatherOverOrderedInputIsRejected, "13623107") {
    insertExchangeFor(std::make_unique<MockSortedNode>(scan(), BSON("a" << 1)),
                      RequiredDistribution::unpartitioned());
}

TEST(PlanDistributionTest, RepartitionIsNeverSortPreserving) {
    auto sorted = insertExchangeFor(std::make_unique<MockSortedNode>(scan(), BSON("a" << 1)),
                                    RequiredDistribution::hashOn({FieldPath("a")}));
    ASSERT_TRUE(sorted->providedSorts().getBaseSortPattern().isEmpty());
}

TEST(ChildRequirementsTest, PassThroughNodesImposeNothingOnTheirInput) {
    auto root = matchOver(scan());
    auto reqs = childRequirements(*root);
    ASSERT_EQ(reqs.size(), 1u);
    ASSERT_EQ(reqs[0].type, RequiredDistribution::Type::Any);
    ASSERT_TRUE(childRequirements(*scan()).empty());
}

// MockSortedNode reports STAGE_SORT_DEFAULT, which has no requirement defined yet.
DEATH_TEST_REGEX(ChildRequirementsDeathTest, UnsupportedNodeTypeIsRejected, "13623106") {
    MockSortedNode node(scan(), BSON("a" << 1));
    childRequirements(node);
}

}  // namespace
}  // namespace mongo::parallelism_planner
