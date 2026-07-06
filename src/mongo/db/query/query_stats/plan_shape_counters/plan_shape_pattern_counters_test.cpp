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

#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution_test_util.h"
#include "mongo/db/query/query_stats/plan_shape_counters/plan_shape_counters.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <bitset>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

namespace mongo::plan_shape_counters {
namespace {
using namespace std::literals::string_view_literals;

using NodeWrapper =
    std::function<std::unique_ptr<QuerySolutionNode>(std::unique_ptr<QuerySolutionNode>)>;

// A solution paired with the single shape counter it should set, or boost::none if it should match
// no shape.
struct ShapeTestCase {
    std::unique_ptr<QuerySolution> solution;
    boost::optional<PlanShapeCounter> expected;
};

std::string_view counterToStr(boost::optional<PlanShapeCounter> counter) {
    return counter ? toStringData(*counter) : "none";
}

size_t countNodes(const QuerySolutionNode& node) {
    size_t count = 1;
    for (auto&& child : node.children) {
        count += countNodes(*child);
    }
    return count;
}

std::unique_ptr<QuerySolutionNode> insertNodeAtDepthImpl(std::unique_ptr<QuerySolutionNode> node,
                                                         size_t& counter,
                                                         size_t target,
                                                         const NodeWrapper& wrap) {
    if (counter++ == target) {
        return wrap(std::move(node));
    }
    for (auto& child : node->children) {
        child = insertNodeAtDepthImpl(std::move(child), counter, target, wrap);
    }
    return node;
}

// Returns 'root' with the 'target'-th node (in preorder) wrapped by 'wrap', inserting a new
// node at the target depth.
std::unique_ptr<QuerySolutionNode> insertNodeAtDepth(std::unique_ptr<QuerySolutionNode> root,
                                                     size_t target,
                                                     const NodeWrapper& wrap) {
    size_t counter = 0;
    return insertNodeAtDepthImpl(std::move(root), counter, target, wrap);
}

class PlanShapeCountersTest : public unittest::Test {
public:
    PlanShapeCountersTest()
        : _nss(NamespaceString::createNamespaceString_forTest("test.coll")),
          _expCtx(make_intrusive<ExpressionContextForTest>()) {}

    // Asserts that 'solution' is identified as exactly the 'expected' shape, or as no shape when
    // 'expected' is boost::none. 'context' identifies the failing combination in the failure
    // message.
    void assertShape(const QuerySolution& solution,
                     boost::optional<PlanShapeCounter> expected,
                     const std::string& context) {
        const auto actual = analyzePlanShapeForCounters(solution).pattern;
        const auto name = [](const boost::optional<PlanShapeCounter>& shape) {
            return shape ? toStringData(*shape) : "no shape"sv;
        };
        ASSERT(actual == expected) << "expected " << name(expected) << " but got " << name(actual)
                                   << " in " << context << "with qsn " << solution.toString();
    }

    std::unique_ptr<QuerySolution> makePlan(std::unique_ptr<QuerySolutionNode> root) {
        auto solution = std::make_unique<QuerySolution>();
        solution->setRoot(std::move(root));
        return solution;
    }

    // Builds a solution rooted at 'findRoot' and extends it with 'extensionRoot' (which must
    // contain a SentinelNode at the attachment point), mirroring how the planner layers pushed-down
    // aggregation stages on top of the find layer.
    std::unique_ptr<QuerySolution> makeExtendedPlan(
        std::unique_ptr<QuerySolutionNode> findRoot,
        std::unique_ptr<QuerySolutionNode> extensionRoot) {
        auto solution = makePlan(std::move(findRoot));
        solution->extendWith(std::move(extensionRoot));
        return solution;
    }

    std::unique_ptr<MatchExpression> trivialFilter() {
        return std::make_unique<AndMatchExpression>();
    }

    std::unique_ptr<CollectionScanNode> makeCollScan() {
        return std::make_unique<CollectionScanNode>(_nss);
    }

    std::unique_ptr<IndexScanNode> makeIxScan(BSONObj keyPattern = BSON("a" << 1)) {
        return std::make_unique<IndexScanNode>(_nss, buildSimpleIndexEntry(keyPattern));
    }

    std::unique_ptr<FetchNode> makeFetch(std::unique_ptr<QuerySolutionNode> child) {
        return std::make_unique<FetchNode>(std::move(child), _nss);
    }

    projection_ast::Projection parseProjection() {
        return projection_ast::parseAndAnalyze(
            _expCtx, BSON("a" << 1), ProjectionPolicies::findProjectionPolicies());
    }

    std::unique_ptr<ProjectionNodeDefault> makeProjection(
        std::unique_ptr<QuerySolutionNode> child) {
        return std::make_unique<ProjectionNodeDefault>(
            std::move(child), nullptr, parseProjection());
    }

    std::unique_ptr<SortNodeDefault> makeSort(std::unique_ptr<QuerySolutionNode> child,
                                              size_t limit = 0) {
        return std::make_unique<SortNodeDefault>(
            std::move(child), BSON("a" << 1), limit, LimitSkipParameterization::Disabled);
    }

    std::unique_ptr<LimitNode> makeLimit(std::unique_ptr<QuerySolutionNode> child,
                                         long long limit = 5) {
        return std::make_unique<LimitNode>(
            std::move(child), limit, LimitSkipParameterization::Disabled);
    }

    std::unique_ptr<SkipNode> makeSkip(std::unique_ptr<QuerySolutionNode> child,
                                       long long skip = 5) {
        return std::make_unique<SkipNode>(
            std::move(child), skip, LimitSkipParameterization::Disabled);
    }

    std::unique_ptr<ShardingFilterNode> makeShardingFilter(
        std::unique_ptr<QuerySolutionNode> child) {
        auto node = std::make_unique<ShardingFilterNode>();
        node->children.push_back(std::move(child));
        return node;
    }

    std::unique_ptr<SortKeyGeneratorNode> makeSortKeyGenerator(
        std::unique_ptr<QuerySolutionNode> child) {
        auto node = std::make_unique<SortKeyGeneratorNode>();
        node->children.push_back(std::move(child));
        return node;
    }

    std::unique_ptr<ReturnKeyNode> makeReturnKey(std::unique_ptr<QuerySolutionNode> child) {
        return std::make_unique<ReturnKeyNode>(std::move(child), std::vector<FieldPath>{});
    }

    std::unique_ptr<OrNode> makeOr(std::unique_ptr<QuerySolutionNode> left,
                                   std::unique_ptr<QuerySolutionNode> right) {
        auto orNode = std::make_unique<OrNode>();
        orNode->children.push_back(std::move(left));
        orNode->children.push_back(std::move(right));
        return orNode;
    }

    std::unique_ptr<MergeSortNode> makeMergeSort(std::unique_ptr<QuerySolutionNode> left,
                                                 std::unique_ptr<QuerySolutionNode> right) {
        auto mergeSort = std::make_unique<MergeSortNode>();
        mergeSort->children.push_back(std::move(left));
        mergeSort->children.push_back(std::move(right));
        return mergeSort;
    }

    // Composite shared by several shapes.
    std::unique_ptr<QuerySolutionNode> makeFetchOr() {
        return makeOr(makeFetch(makeIxScan()), makeFetch(makeIxScan(BSON("b" << 1))));
    }

    std::unique_ptr<QuerySolutionNode> makeOrFetch() {
        return makeFetch(makeOr(makeIxScan(), makeIxScan(BSON("b" << 1))));
    }

    std::unique_ptr<QuerySolutionNode> makeCoveredOr() {
        return makeOr(makeIxScan(), makeIxScan(BSON("b" << 1)));
    }

    std::unique_ptr<QuerySolutionNode> makeFetchSortMerge() {
        return makeMergeSort(makeFetch(makeIxScan()), makeFetch(makeIxScan(BSON("b" << 1))));
    }

    std::unique_ptr<QuerySolutionNode> makeSortMergeFetch() {
        return makeFetch(makeMergeSort(makeIxScan(), makeIxScan(BSON("b" << 1))));
    }

    std::unique_ptr<QuerySolutionNode> makeCoveredSortMerge() {
        return makeMergeSort(makeIxScan(), makeIxScan(BSON("b" << 1)));
    }

    // The table of (plan, expected shape) cases shared by the tests below.
    std::vector<ShapeTestCase> makeShapeTestCases() {
        std::vector<ShapeTestCase> cases;
        auto add = [&](std::unique_ptr<QuerySolutionNode> root,
                       boost::optional<PlanShapeCounter> expected) {
            cases.push_back({makePlan(std::move(root)), expected});
        };

        // Exact matches for every tracked shape, built root-to-leaf from the leaf-to-root shape
        // name.
        // COLLSCAN access path.
        add(makeCollScan(), PlanShapeCounter::kCollscan);
        add(makeProjection(makeCollScan()), PlanShapeCounter::kCollscanProject);
        add(makeSort(makeProjection(makeCollScan())), PlanShapeCounter::kCollscanProjectSort);
        add(makeProjection(makeSort(makeProjection(makeCollScan()))),
            PlanShapeCounter::kCollscanProjectSortProject);
        add(makeSort(makeCollScan()), PlanShapeCounter::kCollscanSort);
        add(makeProjection(makeSort(makeCollScan())), PlanShapeCounter::kCollscanSortProject);
        add(makeSort(makeProjection(makeSort(makeCollScan()))),
            PlanShapeCounter::kCollscanSortProjectSort);

        // Linear shapes rooted in IXSCAN-FETCH.
        add(makeFetch(makeIxScan()), PlanShapeCounter::kIxscanFetch);
        add(makeProjection(makeFetch(makeIxScan())), PlanShapeCounter::kIxscanFetchProject);
        add(makeSort(makeProjection(makeFetch(makeIxScan()))),
            PlanShapeCounter::kIxscanFetchProjectSort);
        add(makeProjection(makeSort(makeProjection(makeFetch(makeIxScan())))),
            PlanShapeCounter::kIxscanFetchProjectSortProject);
        add(makeSort(makeFetch(makeIxScan())), PlanShapeCounter::kIxscanFetchSort);
        add(makeProjection(makeSort(makeFetch(makeIxScan()))),
            PlanShapeCounter::kIxscanFetchSortProject);
        add(makeSort(makeProjection(makeSort(makeFetch(makeIxScan())))),
            PlanShapeCounter::kIxscanFetchSortProjectSort);

        // FETCH(IXSCAN) branches under an OR.
        add(makeFetchOr(), PlanShapeCounter::kIxscanFetchOr);
        add(makeProjection(makeFetchOr()), PlanShapeCounter::kIxscanFetchOrProject);
        add(makeSort(makeProjection(makeFetchOr())), PlanShapeCounter::kIxscanFetchOrProjectSort);
        add(makeSort(makeFetchOr()), PlanShapeCounter::kIxscanFetchOrSort);
        add(makeProjection(makeSort(makeFetchOr())), PlanShapeCounter::kIxscanFetchOrSortProject);

        // Fetched OR over IXSCAN branches.
        add(makeOrFetch(), PlanShapeCounter::kIxscanOrFetch);
        add(makeProjection(makeOrFetch()), PlanShapeCounter::kIxscanOrFetchProject);
        add(makeSort(makeProjection(makeOrFetch())), PlanShapeCounter::kIxscanOrFetchProjectSort);
        add(makeSort(makeOrFetch()), PlanShapeCounter::kIxscanOrFetchSort);
        add(makeProjection(makeSort(makeOrFetch())), PlanShapeCounter::kIxscanOrFetchSortProject);

        // Covered OR shapes (no fetch above the OR).
        add(makeProjection(makeCoveredOr()), PlanShapeCounter::kIxscanOrProject);
        add(makeSort(makeProjection(makeCoveredOr())), PlanShapeCounter::kIxscanOrProjectSort);

        // Covered single-index shapes.
        add(makeProjection(makeIxScan()), PlanShapeCounter::kIxscanProject);
        add(makeSort(makeProjection(makeIxScan())), PlanShapeCounter::kIxscanProjectSort);
        add(makeProjection(makeSort(makeProjection(makeIxScan()))),
            PlanShapeCounter::kIxscanProjectSortProject);

        // Shapes that sort index keys before fetching.
        add(makeFetch(makeSort(makeIxScan())), PlanShapeCounter::kIxscanSortFetch);
        add(makeProjection(makeFetch(makeSort(makeIxScan()))),
            PlanShapeCounter::kIxscanSortFetchProject);

        // SORT_MERGE shapes.
        add(makeFetchSortMerge(), PlanShapeCounter::kIxscanFetchSortMerge);
        add(makeProjection(makeFetchSortMerge()), PlanShapeCounter::kIxscanFetchSortMergeProject);
        add(makeSortMergeFetch(), PlanShapeCounter::kIxscanSortMergeFetch);
        add(makeProjection(makeSortMergeFetch()), PlanShapeCounter::kIxscanSortMergeFetchProject);
        add(makeProjection(makeCoveredSortMerge()), PlanShapeCounter::kIxscanSortMergeProject);

        // Vary the specific type of projection, both should match.
        add(std::make_unique<ProjectionNodeSimple>(makeCollScan(), nullptr, parseProjection()),
            PlanShapeCounter::kCollscanProject);
        add(std::make_unique<ProjectionNodeCovered>(
                makeIxScan(), nullptr, parseProjection(), BSON("a" << 1)),
            PlanShapeCounter::kIxscanProject);
        add(std::make_unique<SortNodeSimple>(
                makeCollScan(), BSON("a" << 1), 0, LimitSkipParameterization::Disabled),
            PlanShapeCounter::kCollscanSort);
        // A sort with an absorbed limit (top-N sort) is still a "SORT".
        add(makeSort(makeCollScan(), 5 /* limit */), PlanShapeCounter::kCollscanSort);

        // The shapes intentionally don't distinguish nodes with and without filters.
        {
            auto collScan = makeCollScan();
            collScan->filter = trivialFilter();
            add(std::move(collScan), PlanShapeCounter::kCollscan);
        }
        {
            auto fetch = makeFetch(makeIxScan());
            fetch->filter = trivialFilter();
            add(std::move(fetch), PlanShapeCounter::kIxscanFetch);
        }

        // Plan shape analysis ignored skip and limit, so we should still match `kCollscanSort`
        add(makeLimit(makeSkip(makeSort(makeCollScan()))), PlanShapeCounter::kCollscanSort);
        add(makeProjection(makeLimit(makeSort(makeSkip(makeFetch(makeIxScan()))))),
            PlanShapeCounter::kIxscanFetchSortProject);

        // Plans that should match no shape.
        // Without extension information (a plain solution whose root is a MATCH), the MATCH is
        // part of the tree being matched.
        add(std::make_unique<MatchNode>(makeFetch(makeIxScan()), trivialFilter()), boost::none);
        // The state machine requires every branch of an OR to match the pattern; a mixed OR (one
        // FETCH->IXSCAN branch and one COLLSCAN branch) matches no shape.
        add(makeOr(makeFetch(makeIxScan()), makeCollScan()), boost::none);
        // A branching node that mixes covered and fetched branches matches no shape either, even
        // though each branch on its own follows one: the branches complete different shapes
        // (IXSCAN-OR-PROJECT vs. IXSCAN-FETCH-OR-PROJECT, and the SORT_MERGE equivalents), and a
        // plan must follow a single shape end to end.
        add(makeProjection(makeOr(makeIxScan(), makeFetch(makeIxScan(BSON("b" << 1))))),
            boost::none);
        add(makeProjection(makeMergeSort(makeIxScan(), makeFetch(makeIxScan(BSON("b" << 1))))),
            boost::none);
        // Index intersection is not a tracked shape.
        {
            auto andSorted = std::make_unique<AndSortedNode>();
            andSorted->children.push_back(makeIxScan());
            andSorted->children.push_back(makeIxScan(BSON("b" << 1)));
            add(makeFetch(std::move(andSorted)), boost::none);
        }
        // A lone covered index scan is not a tracked shape.
        add(makeIxScan(), boost::none);
        // A fetched collection scan is not a tracked shape.
        add(makeFetch(makeCollScan()), boost::none);

        return cases;
    }

    // The nodes the matcher ignores, paired with a name for failure messages.
    std::vector<std::pair<std::string_view, NodeWrapper>> makeIgnoredWrappers() {
        return {
            {"LIMIT"sv,
             [this](std::unique_ptr<QuerySolutionNode> child) {
                 return makeLimit(std::move(child));
             }},
            {"SKIP"sv,
             [this](std::unique_ptr<QuerySolutionNode> child) {
                 return makeSkip(std::move(child));
             }},
            {"SHARDING_FILTER"sv,
             [this](std::unique_ptr<QuerySolutionNode> child) {
                 return makeShardingFilter(std::move(child));
             }},
            {"SORT_KEY_GENERATOR"sv,
             [this](std::unique_ptr<QuerySolutionNode> child) {
                 return makeSortKeyGenerator(std::move(child));
             }},
            {"RETURN_KEY"sv,
             [this](std::unique_ptr<QuerySolutionNode> child) {
                 return makeReturnKey(std::move(child));
             }},
        };
    }

    // QuerySolution is non-copyable, so the sweep rebuilds a fresh plan from a deep clone of
    // solutions tree wrapping the target-th node (preorder) with 'wrap'.
    std::unique_ptr<QuerySolution> insertIgnoredNode(const QuerySolution& solution,
                                                     size_t target,
                                                     const NodeWrapper& wrap) {
        return makePlan(insertNodeAtDepth(solution.root()->clone(), target, wrap));
    }

protected:
    NamespaceString _nss;
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};

// Every case sets exactly its expected counter (and no other), or no counter at all.
TEST_F(PlanShapeCountersTest, ExpectedShapeCounters) {
    auto cases = makeShapeTestCases();
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& [solution, expected] = cases[i];
        assertShape(*solution, expected, str::stream() << "expected " << counterToStr(expected));
    }
}

// This test will fail in case our plan shape counters are added to, but the new shape is not
// tested.
TEST_F(PlanShapeCountersTest, TestCasesCoverEveryShape) {
    std::bitset<kNumPlanShapeCounters> covered;
    for (auto&& testCase : makeShapeTestCases()) {
        if (testCase.expected) {
            covered.set(static_cast<size_t>(*testCase.expected));
        }
    }
    for (size_t i = 0; i < kNumPlanShapeCounters; ++i) {
        ASSERT(covered.test(i)) << "no test case expects shape "
                                << toStringData(static_cast<PlanShapeCounter>(i));
    }
}

// Inserting an ignored node above any node of any case's plan leaves the matched shape (or the
// absence of one) unchanged.
TEST_F(PlanShapeCountersTest, IgnoredNodesDoNotAffectShapeAtAnyPosition) {
    auto cases = makeShapeTestCases();
    auto wrappers = makeIgnoredWrappers();
    for (size_t i = 0; i < cases.size(); ++i) {
        const size_t numNodes = countNodes(*cases[i].solution->root());
        for (auto&& [name, wrap] : wrappers) {
            for (size_t pos = 0; pos < numNodes; ++pos) {
                assertShape(*insertIgnoredNode(*cases[i].solution, pos, wrap),
                            cases[i].expected,
                            str::stream() << "expected " << counterToStr(cases[i].expected)
                                          << " with " << name << " inserted at node " << pos);
            }
        }
    }
}

// Inserting a stage that participates in no shape and isn't ignored (MATCH) above any node of any
// case's plan prevents every shape from matching.
TEST_F(PlanShapeCountersTest, DisruptiveNodeAtAnyPositionMatchesNoShape) {
    auto cases = makeShapeTestCases();
    NodeWrapper wrapMatch = [this](std::unique_ptr<QuerySolutionNode> child) {
        return std::make_unique<MatchNode>(std::move(child), trivialFilter());
    };
    for (size_t i = 0; i < cases.size(); ++i) {
        const size_t numNodes = countNodes(*cases[i].solution->root());
        for (size_t pos = 0; pos < numNodes; ++pos) {
            auto root = insertNodeAtDepth(cases[i].solution->root()->clone(), pos, wrapMatch);
            assertShape(*makePlan(std::move(root)),
                        boost::none,
                        str::stream() << "expected none with MATCH inserted at node " << pos);
        }
    }
}

TEST_F(PlanShapeCountersTest, ExtensionStagesAreExcludedFromShape) {
    // A $match pushed down to SBE is layered on top of the find layer via extendWith; the shape
    // reflects the find layer below the extension.
    auto match = std::make_unique<MatchNode>(std::make_unique<SentinelNode>(), trivialFilter());
    assertShape(*makeExtendedPlan(makeFetch(makeIxScan()), std::move(match)),
                PlanShapeCounter::kIxscanFetch,
                "extended plan");
}

TEST_F(PlanShapeCountersTest, LookupForeignSideIsExcludedFromShape) {
    // $lookup pushed down to SBE: the local side is the find layer, the foreign side hangs off
    // the EQ_LOOKUP extension node. The foreign collection scan must not affect the shape.
    auto foreignNss = NamespaceString::createNamespaceString_forTest("test.foreign");
    std::vector<std::unique_ptr<QuerySolutionNode>> children;
    children.push_back(std::make_unique<SentinelNode>());
    children.push_back(std::make_unique<CollectionScanNode>(foreignNss));
    auto eqLookup = std::make_unique<EqLookupNode>(std::move(children),
                                                   foreignNss,
                                                   FieldPath("a"),
                                                   FieldPath("b"),
                                                   FieldPath("out"),
                                                   EqLookupNode::LookupStrategy::kNestedLoopJoin,
                                                   false /* shouldProduceBson */);
    assertShape(*makeExtendedPlan(makeFetch(makeIxScan()), std::move(eqLookup)),
                PlanShapeCounter::kIxscanFetch,
                "lookup-extended plan");
}

TEST_F(PlanShapeCountersTest, AllCounterNamesAreUnique) {
    StringMap<size_t> seen;
    for (size_t i = 0; i < kNumPlanShapeCounters; ++i) {
        const auto name = toStringData(static_cast<PlanShapeCounter>(i));
        ASSERT_FALSE(name.empty()) << "counter " << i << " has an empty name";
        ASSERT_EQ(seen.count(std::string{name}), 0u) << "duplicate counter name " << name;
        seen[std::string{name}] = i;
    }
}

}  // namespace
}  // namespace mongo::plan_shape_counters
