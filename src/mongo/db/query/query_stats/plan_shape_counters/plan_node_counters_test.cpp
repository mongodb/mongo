// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution_test_util.h"
#include "mongo/db/query/query_stats/plan_shape_counters/plan_shape_counters.h"
#include "mongo/db/query/timeseries/bucket_spec.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::plan_shape_counters {
namespace {

// A solution paired with the set of node counters it should set.
struct NodeCountTestCase {
    std::unique_ptr<QuerySolution> solution;
    QsnNodeCounts expected;
};

QsnNodeCounts makeCounts(std::initializer_list<QsnNodeCounter> counters) {
    QsnNodeCounts counts;
    for (auto counter : counters) {
        counts.set(counter);
    }
    return counts;
}

std::string countsToString(const QsnNodeCounts& counts) {
    str::stream out;
    out << "{";
    for (size_t i = 0; i < kNumQsnNodeCounters; ++i) {
        if (counts.test(static_cast<QsnNodeCounter>(i))) {
            out << " " << toStringData(static_cast<QsnNodeCounter>(i));
        }
    }
    out << " }";
    return out;
}

class PlanNodeCountersTest : public unittest::Test {
public:
    PlanNodeCountersTest()
        : _nss(NamespaceString::createNamespaceString_forTest("test.coll")),
          _expCtx(make_intrusive<ExpressionContextForTest>()) {}

    void assertCounts(const QuerySolution& solution,
                      const QsnNodeCounts& expected,
                      const std::string& context) {
        const auto actual = analyzePlanShapeForCounters(solution).qsnNodeCounts;
        ASSERT(actual == expected)
            << "expected " << countsToString(expected) << " but got " << countsToString(actual)
            << " in " << context << " with qsn " << solution.toString();
    }

    std::unique_ptr<QuerySolution> makePlan(std::unique_ptr<QuerySolutionNode> root) {
        auto solution = std::make_unique<QuerySolution>();
        solution->setRoot(std::move(root));
        return solution;
    }

    std::unique_ptr<MatchExpression> trivialFilter() {
        return std::make_unique<AndMatchExpression>();
    }

    template <typename Node>
    std::unique_ptr<Node> addFilter(std::unique_ptr<Node> node) {
        node->filter = trivialFilter();
        return node;
    }

    std::unique_ptr<CollectionScanNode> makeCollScan(bool withFilter = false) {
        auto scan = std::make_unique<CollectionScanNode>(_nss);
        if (withFilter) {
            scan->filter = trivialFilter();
        }
        return scan;
    }

    std::unique_ptr<IndexScanNode> makeIxScan(bool withFilter = false,
                                              BSONObj keyPattern = BSON("a" << 1)) {
        auto scan = std::make_unique<IndexScanNode>(_nss, buildSimpleIndexEntry(keyPattern));
        if (withFilter) {
            scan->filter = trivialFilter();
        }
        return scan;
    }

    std::unique_ptr<FetchNode> makeFetch(std::unique_ptr<QuerySolutionNode> child,
                                         bool withFilter = false) {
        auto fetch = std::make_unique<FetchNode>(std::move(child), _nss);
        if (withFilter) {
            fetch->filter = trivialFilter();
        }
        return fetch;
    }

    std::unique_ptr<OrNode> makeOr(std::unique_ptr<QuerySolutionNode> left,
                                   std::unique_ptr<QuerySolutionNode> right) {
        auto orNode = std::make_unique<OrNode>();
        orNode->children.push_back(std::move(left));
        orNode->children.push_back(std::move(right));
        return orNode;
    }

    std::unique_ptr<OrNode> makeWideOr(size_t numChildren) {
        auto orNode = std::make_unique<OrNode>();
        for (size_t i = 0; i < numChildren; ++i) {
            orNode->children.push_back(makeIxScan());
        }
        return orNode;
    }

    std::unique_ptr<MergeSortNode> makeMergeSort(std::unique_ptr<QuerySolutionNode> left,
                                                 std::unique_ptr<QuerySolutionNode> right) {
        auto mergeSort = std::make_unique<MergeSortNode>();
        mergeSort->sort = BSON("a" << 1);
        mergeSort->children.push_back(std::move(left));
        mergeSort->children.push_back(std::move(right));
        return mergeSort;
    }

    std::unique_ptr<MergeSortNode> makeWideMergeSort(size_t numChildren) {
        auto mergeSort = std::make_unique<MergeSortNode>();
        mergeSort->sort = BSON("a" << 1);
        for (size_t i = 0; i < numChildren; ++i) {
            mergeSort->children.push_back(makeIxScan());
        }
        return mergeSort;
    }

    std::unique_ptr<AndHashNode> makeAndHash() {
        auto andHash = std::make_unique<AndHashNode>();
        andHash->children.push_back(makeIxScan());
        andHash->children.push_back(makeIxScan(false, BSON("b" << 1)));
        return andHash;
    }

    std::unique_ptr<AndSortedNode> makeAndSorted() {
        auto andSorted = std::make_unique<AndSortedNode>();
        andSorted->children.push_back(makeIxScan());
        andSorted->children.push_back(makeIxScan(false, BSON("b" << 1)));
        return andSorted;
    }

    std::unique_ptr<TextOrNode> makeTextOr() {
        auto textOr = std::make_unique<TextOrNode>();
        textOr->children.push_back(makeIxScan());
        textOr->children.push_back(makeIxScan(false, BSON("b" << 1)));
        return textOr;
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

    projection_ast::Projection parseProjection() {
        return projection_ast::parseAndAnalyze(
            _expCtx, BSON("a" << 1), ProjectionPolicies::findProjectionPolicies());
    }

    std::unique_ptr<SortKeyGeneratorNode> makeSortKeyGenerator(
        std::unique_ptr<QuerySolutionNode> child) {
        auto node = std::make_unique<SortKeyGeneratorNode>();
        node->children.push_back(std::move(child));
        node->sortSpec = BSON("a" << 1);
        return node;
    }

    template <typename SortType>
    std::unique_ptr<SortType> makeSort(std::unique_ptr<QuerySolutionNode> child,
                                       BSONObj pattern,
                                       size_t limit,
                                       LimitSkipParameterization canBeParameterized) {
        return std::make_unique<SortType>(
            std::move(child), pattern, limit, canBeParameterized, 0 /*maxMemoryUsageBytes*/);
    }

    std::unique_ptr<ShardingFilterNode> makeShardingFilter(
        std::unique_ptr<QuerySolutionNode> child) {
        auto node = std::make_unique<ShardingFilterNode>();
        node->children.push_back(std::move(child));
        return node;
    }

    std::unique_ptr<GroupNode> makeGroup(std::unique_ptr<QuerySolutionNode> child) {
        return std::make_unique<GroupNode>(std::move(child),
                                           ExpressionConstant::create(_expCtx.get(), Value(1)),
                                           std::vector<AccumulationStatement>{},
                                           false /* merging */,
                                           false /* willBeMerged */,
                                           false /* shouldProduceBson */);
    }

    std::unique_ptr<UnpackTsBucketNode> makeUnpackTsBucket(
        std::unique_ptr<QuerySolutionNode> child) {
        return std::make_unique<UnpackTsBucketNode>(
            std::move(child),
            timeseries::BucketSpec(
                "time", boost::none, {}, timeseries::BucketSpec::Behavior::kInclude),
            nullptr /* eventFilter */,
            nullptr /* wholeBucketFilter */,
            false /* includeMeta */);
    }

    std::unique_ptr<EqLookupNode> makeEqLookup(bool withUnwind) {
        auto foreignNss = NamespaceString::createNamespaceString_forTest("test.foreign");
        std::vector<std::unique_ptr<QuerySolutionNode>> children;
        children.push_back(std::make_unique<SentinelNode>());
        children.push_back(std::make_unique<CollectionScanNode>(foreignNss));
        if (withUnwind) {
            return std::make_unique<EqLookupNode>(std::move(children),
                                                  foreignNss,
                                                  FieldPath("a"),
                                                  FieldPath("b"),
                                                  FieldPath("out"),
                                                  EqLookupNode::LookupStrategy::kNestedLoopJoin,
                                                  false /* shouldProduceBson */,
                                                  false /* preserveNullAndEmptyArrays */,
                                                  boost::none /* indexPath */);
        }
        return std::make_unique<EqLookupNode>(std::move(children),
                                              foreignNss,
                                              FieldPath("a"),
                                              FieldPath("b"),
                                              FieldPath("out"),
                                              EqLookupNode::LookupStrategy::kNestedLoopJoin,
                                              false /* shouldProduceBson */);
    }

    std::unique_ptr<IndexProbeNode> makeIndexProbe() {
        return std::make_unique<IndexProbeNode>(_nss, buildSimpleIndexEntry(BSON("a" << 1)));
    }

    // The table of (plan, expected counters) cases shared by the tests below.
    std::vector<NodeCountTestCase> makeNodeCountTestCases() {
        std::vector<NodeCountTestCase> cases;
        auto add = [&](std::unique_ptr<QuerySolutionNode> root,
                       std::initializer_list<QsnNodeCounter> expected) {
            cases.push_back({makePlan(std::move(root)), makeCounts(expected)});
        };
        auto addExtended = [&](std::unique_ptr<QuerySolutionNode> extension,
                               std::initializer_list<QsnNodeCounter> expected) {
            auto solution = makePlan(makeCollScan());
            solution->extendWith(std::move(extension));
            cases.push_back({std::move(solution), makeCounts(expected)});
        };

        // Each counted node type, with and without a filter.
        add(makeCollScan(), {QsnNodeCounter::kCollscanNoFilter});
        add(makeCollScan(true /* withFilter */), {QsnNodeCounter::kCollscanWithFilter});
        add(makeIxScan(), {QsnNodeCounter::kIxscanNoFilter});
        add(makeIxScan(true /* withFilter */), {QsnNodeCounter::kIxscanWithFilter});
        add(makeFetch(makeIxScan()),
            {QsnNodeCounter::kFetchNoFilter, QsnNodeCounter::kIxscanNoFilter});
        add(makeFetch(makeIxScan(), true /* withFilter */),
            {QsnNodeCounter::kFetchWithFilter, QsnNodeCounter::kIxscanNoFilter});

        // Index intersection nodes.
        add(makeAndHash(), {QsnNodeCounter::kAndHashNoFilter, QsnNodeCounter::kIxscanNoFilter});
        add(addFilter(makeAndHash()),
            {QsnNodeCounter::kAndHashWithFilter, QsnNodeCounter::kIxscanNoFilter});
        add(makeAndSorted(), {QsnNodeCounter::kAndSorted, QsnNodeCounter::kIxscanNoFilter});

        // ORs and SORT_MERGEs are counted by whether they have a filter and by how many children
        // they have. 100 children is still "lte 100"; 101 is "gt 100".
        add(makeOr(makeIxScan(), makeIxScan()),
            {QsnNodeCounter::kOrNoFilterLte100Children, QsnNodeCounter::kIxscanNoFilter});
        add(addFilter(makeOr(makeIxScan(), makeIxScan())),
            {QsnNodeCounter::kOrWithFilterLte100Children, QsnNodeCounter::kIxscanNoFilter});
        add(makeWideOr(100),
            {QsnNodeCounter::kOrNoFilterLte100Children, QsnNodeCounter::kIxscanNoFilter});
        add(makeWideOr(101),
            {QsnNodeCounter::kOrNoFilterGt100Children, QsnNodeCounter::kIxscanNoFilter});
        add(addFilter(makeWideOr(101)),
            {QsnNodeCounter::kOrWithFilterGt100Children, QsnNodeCounter::kIxscanNoFilter});
        add(makeMergeSort(makeIxScan(), makeIxScan()),
            {QsnNodeCounter::kSortMergeNoFilterLte100Children, QsnNodeCounter::kIxscanNoFilter});
        add(addFilter(makeMergeSort(makeIxScan(), makeIxScan())),
            {QsnNodeCounter::kSortMergeWithFilterLte100Children, QsnNodeCounter::kIxscanNoFilter});
        add(makeWideMergeSort(101),
            {QsnNodeCounter::kSortMergeNoFilterGt100Children, QsnNodeCounter::kIxscanNoFilter});
        add(addFilter(makeWideMergeSort(101)),
            {QsnNodeCounter::kSortMergeWithFilterGt100Children, QsnNodeCounter::kIxscanNoFilter});

        // Simple wrapper nodes.
        add(std::make_unique<ReturnKeyNode>(makeIxScan(), std::vector<FieldPath>{}),
            {QsnNodeCounter::kReturnKey, QsnNodeCounter::kIxscanNoFilter});
        add(makeShardingFilter(makeCollScan()),
            {QsnNodeCounter::kShardingFilter, QsnNodeCounter::kCollscanNoFilter});
        add(makeSortKeyGenerator(makeCollScan()),
            {QsnNodeCounter::kSortKeyGenerator, QsnNodeCounter::kCollscanNoFilter});

        // The three projection implementations.
        add(std::make_unique<ProjectionNodeDefault>(makeCollScan(), nullptr, parseProjection()),
            {QsnNodeCounter::kProjectionDefault, QsnNodeCounter::kCollscanNoFilter});
        add(std::make_unique<ProjectionNodeCovered>(
                makeIxScan(), nullptr, parseProjection(), BSON("a" << 1)),
            {QsnNodeCounter::kProjectionCovered, QsnNodeCounter::kIxscanNoFilter});
        add(std::make_unique<ProjectionNodeSimple>(makeCollScan(), nullptr, parseProjection()),
            {QsnNodeCounter::kProjectionSimple, QsnNodeCounter::kCollscanNoFilter});

        // Sorts are counted by implementation and by whether they have a limit.
        add(makeSort<SortNodeDefault>(
                makeCollScan(), BSON("a" << 1), 0 /* limit */, LimitSkipParameterization::Disabled),
            {QsnNodeCounter::kSortDefaultNoLimit, QsnNodeCounter::kCollscanNoFilter});
        add(makeSort<SortNodeDefault>(
                makeCollScan(), BSON("a" << 1), 5 /* limit */, LimitSkipParameterization::Disabled),
            {QsnNodeCounter::kSortDefaultWithLimit, QsnNodeCounter::kCollscanNoFilter});
        add(makeSort<SortNodeSimple>(
                makeCollScan(), BSON("a" << 1), 0 /* limit */, LimitSkipParameterization::Disabled),
            {QsnNodeCounter::kSortSimpleNoLimit, QsnNodeCounter::kCollscanNoFilter});
        add(makeSort<SortNodeSimple>(
                makeCollScan(), BSON("a" << 1), 5 /* limit */, LimitSkipParameterization::Disabled),
            {QsnNodeCounter::kSortSimpleWithLimit, QsnNodeCounter::kCollscanNoFilter});

        // Limits and skips are counted by size. Small is up to 100, medium up to 10000, and
        // large is everything else.
        add(makeLimit(makeCollScan(), 100),
            {QsnNodeCounter::kLimitSmall, QsnNodeCounter::kCollscanNoFilter});
        add(makeLimit(makeCollScan(), 101),
            {QsnNodeCounter::kLimitMedium, QsnNodeCounter::kCollscanNoFilter});
        add(makeLimit(makeCollScan(), 10000),
            {QsnNodeCounter::kLimitMedium, QsnNodeCounter::kCollscanNoFilter});
        add(makeLimit(makeCollScan(), 10001),
            {QsnNodeCounter::kLimitLarge, QsnNodeCounter::kCollscanNoFilter});
        add(makeSkip(makeCollScan(), 100),
            {QsnNodeCounter::kSkipSmall, QsnNodeCounter::kCollscanNoFilter});
        add(makeSkip(makeCollScan(), 101),
            {QsnNodeCounter::kSkipMedium, QsnNodeCounter::kCollscanNoFilter});
        add(makeSkip(makeCollScan(), 10000),
            {QsnNodeCounter::kSkipMedium, QsnNodeCounter::kCollscanNoFilter});
        add(makeSkip(makeCollScan(), 10001),
            {QsnNodeCounter::kSkipLarge, QsnNodeCounter::kCollscanNoFilter});

        // TEXT_OR is a subclass of OR but counts as its own node type, not as an OR.
        add(makeTextOr(), {QsnNodeCounter::kTextOr, QsnNodeCounter::kIxscanNoFilter});

        // SBE-specific nodes that represent pushed-down aggregation stages.
        add(std::make_unique<MatchNode>(makeCollScan(), trivialFilter()),
            {QsnNodeCounter::kMatch, QsnNodeCounter::kCollscanNoFilter});
        add(std::make_unique<ReplaceRootNode>(makeCollScan(), nullptr),
            {QsnNodeCounter::kReplaceRoot, QsnNodeCounter::kCollscanNoFilter});
        add(makeGroup(makeCollScan()), {QsnNodeCounter::kGroup, QsnNodeCounter::kCollscanNoFilter});
        add(makeUnpackTsBucket(makeCollScan()),
            {QsnNodeCounter::kUnpackTsBucket, QsnNodeCounter::kCollscanNoFilter});
        addExtended(makeEqLookup(false /* withUnwind */),
                    {QsnNodeCounter::kEqLookupNoUnwind, QsnNodeCounter::kCollscanNoFilter});
        addExtended(makeEqLookup(true /* withUnwind */),
                    {QsnNodeCounter::kEqLookupWithUnwind, QsnNodeCounter::kCollscanNoFilter});

        // Join nodes.
        add(std::make_unique<HashJoinEmbeddingNode>(makeCollScan(),
                                                    makeCollScan(),
                                                    std::vector<QSNJoinPredicate>{},
                                                    boost::none,
                                                    boost::none),
            {QsnNodeCounter::kHashJoin, QsnNodeCounter::kCollscanNoFilter});
        add(std::make_unique<NestedLoopJoinEmbeddingNode>(makeCollScan(),
                                                          makeCollScan(),
                                                          std::vector<QSNJoinPredicate>{},
                                                          boost::none,
                                                          boost::none),
            {QsnNodeCounter::kNlj, QsnNodeCounter::kCollscanNoFilter});
        add(std::make_unique<IndexedNestedLoopJoinEmbeddingNode>(makeCollScan(),
                                                                 makeIndexProbe(),
                                                                 std::vector<QSNJoinPredicate>{},
                                                                 boost::none,
                                                                 boost::none),
            {QsnNodeCounter::kInlj,
             QsnNodeCounter::kIndexProbe,
             QsnNodeCounter::kCollscanNoFilter});

        // One plan can set several counters, including the no_filter and with_filter variants of
        // the same node type.
        add(makeOr(makeFetch(makeIxScan()), makeFetch(makeIxScan(true /* withFilter */), true)),
            {QsnNodeCounter::kOrNoFilterLte100Children,
             QsnNodeCounter::kFetchNoFilter,
             QsnNodeCounter::kFetchWithFilter,
             QsnNodeCounter::kIxscanNoFilter,
             QsnNodeCounter::kIxscanWithFilter});

        // Seen-at-least-once semantics: many nodes satisfying the same counter set it once.
        add(makeOr(makeCollScan(), makeCollScan()),
            {QsnNodeCounter::kOrNoFilterLte100Children, QsnNodeCounter::kCollscanNoFilter});

        // Nodes without a counter contribute nothing.
        add(std::make_unique<SentinelNode>(), {});

        return cases;
    }

protected:
    NamespaceString _nss;
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};

// Every case sets exactly its expected counters.
TEST_F(PlanNodeCountersTest, ExpectedNodeCounters) {
    for (auto&& [solution, expected] : makeNodeCountTestCases()) {
        assertCounts(*solution, expected, str::stream() << "expected " << countsToString(expected));
    }
}

// This test will fail in case node counters are added to, but the new counter is not tested.
TEST_F(PlanNodeCountersTest, TestCasesCoverEveryCounter) {
    QsnNodeCounts covered;
    for (auto&& testCase : makeNodeCountTestCases()) {
        covered.flags |= testCase.expected.flags;
    }
    for (size_t i = 0; i < kNumQsnNodeCounters; ++i) {
        ASSERT(covered.test(static_cast<QsnNodeCounter>(i)))
            << "no test case expects counter " << toStringData(static_cast<QsnNodeCounter>(i));
    }
}

// Unlike the plan shape pattern, node counters cover the stages layered on top of the find layer
// by pushdown: a $match pushed down above the find layer sets the MATCH counter.
TEST_F(PlanNodeCountersTest, ExtensionStagesAreCounted) {
    auto solution = makePlan(makeIxScan());
    solution->extendWith(
        std::make_unique<MatchNode>(std::make_unique<SentinelNode>(), trivialFilter()));

    assertCounts(*solution,
                 makeCounts({QsnNodeCounter::kMatch, QsnNodeCounter::kIxscanNoFilter}),
                 "match-extended plan");
}

// The subtrees hanging off of pushdown stages target other collections, so their nodes must not
// be counted: a $lookup pushed down to SBE counts the EQ_LOOKUP stage itself, but only the
// winning plan on the local collection, not the foreign side hanging off the EQ_LOOKUP stage.
TEST_F(PlanNodeCountersTest, ForeignSideOfLookupIsNotCounted) {
    auto solution = makePlan(makeFetch(makeIxScan()));
    solution->extendWith(makeEqLookup(false /* withUnwind */));

    // The foreign-side collection scan is not counted; the EQ_LOOKUP, find-layer fetch, and index
    // scan are.
    assertCounts(*solution,
                 makeCounts({QsnNodeCounter::kEqLookupNoUnwind,
                             QsnNodeCounter::kFetchNoFilter,
                             QsnNodeCounter::kIxscanNoFilter}),
                 "lookup-extended plan");

    // The plan shape pattern is still matched against the find layer only, from the same
    // analysis.
    const auto result = analyzePlanShapeForCounters(*solution);
    ASSERT(result.pattern == PlanShapeCounter::kIxscanFetch);
}

TEST_F(PlanNodeCountersTest, AllCounterNamesAreUnique) {
    std::vector<std::string_view> seen;
    for (size_t i = 0; i < kNumQsnNodeCounters; ++i) {
        const auto name = toStringData(static_cast<QsnNodeCounter>(i));
        ASSERT_FALSE(name.empty()) << "counter " << i << " has an empty name";
        ASSERT(std::find(seen.begin(), seen.end(), name) == seen.end())
            << "duplicate counter name " << name;
        seen.push_back(name);
    }
}

}  // namespace
}  // namespace mongo::plan_shape_counters
