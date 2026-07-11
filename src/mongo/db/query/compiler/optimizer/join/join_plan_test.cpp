// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/join_plan.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_test_utils.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo::join_ordering {
namespace {

using namespace cost_based_ranker;

JoinCostEstimate zeroJoinCost() {
    return JoinCostEstimate{zeroCE, zeroCE, zeroCE, zeroCE};
}

JoinGraph makeJoinGraph(std::initializer_list<NamespaceString> nsses) {
    MutableJoinGraph mgraph;
    for (const auto& nss : nsses) {
        ASSERT(mgraph.addNode(nss, nullptr, boost::none));
    }
    return JoinGraph(std::move(mgraph));
}

std::string redactedNamespaceString(const NamespaceString& nss) {
    return std::string(redactTenant(nss));
}

std::vector<std::string> getSubsetCollectionNames(const BSONObj& planBson) {
    std::vector<std::string> names;
    for (auto&& elem : planBson.getField("subsetCollectionNames").Array()) {
        names.push_back(elem.String());
    }
    return names;
}

class JoinPlanNodeRegistryTest : public unittest::Test {
protected:
    JoinPlanNodeId registerBaseNode(NodeId nodeId, const NamespaceString& nss) {
        _solutions.push_back(makeCollScanPlan({}));
        return _registry.registerBaseNode(nodeId, _solutions.back().get(), nss, zeroJoinCost());
    }

    BSONObj getPlanNodeBSON(JoinPlanNodeId nodeId, const JoinGraph& graph) {
        return _registry.joinPlanNodeToBSON(nodeId, graph, graph.numNodes());
    }

    JoinPlanNodeRegistry _registry;
    std::vector<std::unique_ptr<QuerySolution>> _solutions;
};

TEST_F(JoinPlanNodeRegistryTest, JoinPlanNodeToBSONIncludesSubsetCollectionNamesForBaseNode) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test", "foo");
    const auto nodeId = registerBaseNode(0, nss);
    const JoinGraph graph = makeJoinGraph({nss});

    const auto planBson = getPlanNodeBSON(nodeId, graph);

    ASSERT_EQ(std::vector<std::string>{redactedNamespaceString(nss)},
              getSubsetCollectionNames(planBson));
}

TEST_F(JoinPlanNodeRegistryTest, JoinPlanNodeToBSONIncludesSubsetCollectionNamesForJoinNode) {
    const auto fooNss = NamespaceString::createNamespaceString_forTest("test", "foo");
    const auto barNss = NamespaceString::createNamespaceString_forTest("test", "bar");
    const auto leftId = registerBaseNode(0, fooNss);
    const auto rightId = registerBaseNode(1, barNss);
    const JoinGraph graph = makeJoinGraph({fooNss, barNss});

    JoinSubset subset(makeNodeSet(0, 1));
    const auto joinId =
        _registry.registerJoinNode(subset, JoinMethod::HJ, leftId, rightId, zeroJoinCost());

    const auto planBson = getPlanNodeBSON(joinId, graph);

    ASSERT_EQ(std::vector<std::string>(
                  {redactedNamespaceString(fooNss), redactedNamespaceString(barNss)}),
              getSubsetCollectionNames(planBson));
}

TEST_F(JoinPlanNodeRegistryTest, JoinPlanNodeToBSONOrdersSubsetCollectionNamesByNodeId) {
    const auto fooNss = NamespaceString::createNamespaceString_forTest("test", "foo");
    const auto unusedNss = NamespaceString::createNamespaceString_forTest("test", "unused");
    const auto bazNss = NamespaceString::createNamespaceString_forTest("test", "baz");

    // NodeId 0 is on the right side of the join tree; nodeId 2 is on the left.
    const auto fooId = registerBaseNode(0, fooNss);
    const auto bazId = registerBaseNode(2, bazNss);
    const JoinGraph graph = makeJoinGraph({fooNss, unusedNss, bazNss});

    JoinSubset subset(makeNodeSet(0, 2));
    const auto joinId =
        _registry.registerJoinNode(subset, JoinMethod::HJ, bazId, fooId, zeroJoinCost());

    const auto planBson = getPlanNodeBSON(joinId, graph);

    ASSERT_EQ("101", planBson.getStringField("subset"));
    ASSERT_EQ(std::vector<std::string>(
                  {redactedNamespaceString(fooNss), redactedNamespaceString(bazNss)}),
              getSubsetCollectionNames(planBson));
}

TEST_F(JoinPlanNodeRegistryTest, JoinPlanNodeToBSONIncludesSubsetCollectionNamesForNestedJoin) {
    const auto fooNss = NamespaceString::createNamespaceString_forTest("test", "foo");
    const auto barNss = NamespaceString::createNamespaceString_forTest("test", "bar");
    const auto bazNss = NamespaceString::createNamespaceString_forTest("test", "baz");
    const JoinGraph graph = makeJoinGraph({fooNss, barNss, bazNss});

    const auto fooId = registerBaseNode(0, fooNss);
    const auto barId = registerBaseNode(1, barNss);
    const auto bazId = registerBaseNode(2, bazNss);

    JoinSubset fooBarSubset(makeNodeSet(0, 1));
    const auto fooBarJoinId =
        _registry.registerJoinNode(fooBarSubset, JoinMethod::HJ, fooId, barId, zeroJoinCost());

    JoinSubset allSubset(makeNodeSet(0, 1, 2));
    const auto rootJoinId =
        _registry.registerJoinNode(allSubset, JoinMethod::HJ, fooBarJoinId, bazId, zeroJoinCost());

    const auto planBson = getPlanNodeBSON(rootJoinId, graph);

    ASSERT_EQ(std::vector<std::string>({redactedNamespaceString(fooNss),
                                        redactedNamespaceString(barNss),
                                        redactedNamespaceString(bazNss)}),
              getSubsetCollectionNames(planBson));
}

TEST_F(JoinPlanNodeRegistryTest, JoinPlanNodeToBSONIncludesSubsetCollectionNamesForINLJRHSNode) {
    const auto fooNss = NamespaceString::createNamespaceString_forTest("test", "foo");
    const auto barNss = NamespaceString::createNamespaceString_forTest("test", "bar");
    const auto leftId = registerBaseNode(0, fooNss);
    const JoinGraph graph = makeJoinGraph({fooNss, barNss});

    const auto indexes = makeIndexCatalogEntries({BSON("a" << 1)});
    const auto inlRhsId = _registry.registerINLJRHSNode(1, indexes.front(), barNss);

    JoinSubset subset(makeNodeSet(0, 1));
    const auto joinId =
        _registry.registerJoinNode(subset, JoinMethod::INLJ, leftId, inlRhsId, zeroJoinCost());

    const auto planBson = getPlanNodeBSON(joinId, graph);

    ASSERT_EQ(std::vector<std::string>(
                  {redactedNamespaceString(fooNss), redactedNamespaceString(barNss)}),
              getSubsetCollectionNames(planBson));

    const auto inlRhsBson = getPlanNodeBSON(inlRhsId, graph);
    ASSERT_EQ(std::vector<std::string>{redactedNamespaceString(barNss)},
              getSubsetCollectionNames(inlRhsBson));
}

}  // namespace
}  // namespace mongo::join_ordering
