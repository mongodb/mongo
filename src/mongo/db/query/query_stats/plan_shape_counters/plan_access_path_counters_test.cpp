// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution_test_util.h"
#include "mongo/db/query/query_stats/plan_shape_counters/plan_shape_counters.h"
#include "mongo/db/query/record_id_bound.h"
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

// A solution paired with the set of access path counters it should set.
struct AccessPathTestCase {
    std::unique_ptr<QuerySolution> solution;
    AccessPathCounts expected;
};

AccessPathCounts makeCounts(std::initializer_list<AccessPathCounter> counters) {
    AccessPathCounts counts;
    for (auto counter : counters) {
        counts.set(counter);
    }
    return counts;
}

std::string countsToString(const AccessPathCounts& counts) {
    str::stream out;
    out << "{";
    for (size_t i = 0; i < kNumAccessPathCounters; ++i) {
        if (counts.test(static_cast<AccessPathCounter>(i))) {
            out << " " << toStringData(static_cast<AccessPathCounter>(i));
        }
    }
    out << " }";
    return out;
}

class PlanAccessPathCountersTest : public unittest::Test {
public:
    PlanAccessPathCountersTest()
        : _nss(NamespaceString::createNamespaceString_forTest("test.coll")) {}

    void assertCounts(const QuerySolution& solution,
                      const AccessPathCounts& expected,
                      const std::string& context) {
        const auto actual = analyzePlanShapeForCounters(solution).accessPathCounts;
        ASSERT(actual == expected)
            << "expected " << countsToString(expected) << " but got " << countsToString(actual)
            << " in " << context << " with qsn " << solution.toString();
    }

    std::unique_ptr<QuerySolution> makePlan(std::unique_ptr<QuerySolutionNode> root) {
        auto solution = std::make_unique<QuerySolution>();
        solution->setRoot(std::move(root));
        return solution;
    }

    std::unique_ptr<CollectionScanNode> makeCollScan() {
        return std::make_unique<CollectionScanNode>(_nss);
    }

    std::unique_ptr<CollectionScanNode> makeClusteredCollScan() {
        auto scan = makeCollScan();
        scan->isClustered = true;
        scan->minRecord = RecordIdBound(RecordId(1));
        return scan;
    }

    std::unique_ptr<IndexScanNode> makeIxScan(BSONObj keyPattern = BSON("a" << 1)) {
        return std::make_unique<IndexScanNode>(_nss, buildSimpleIndexEntry(keyPattern));
    }

    std::unique_ptr<IndexScanNode> makeIxScanWithBounds(
        std::vector<std::vector<Interval>> fieldIntervals) {
        auto scan = makeIxScan();
        for (auto& intervals : fieldIntervals) {
            OrderedIntervalList oil;
            oil.intervals = std::move(intervals);
            scan->bounds.fields.push_back(std::move(oil));
        }
        return scan;
    }

    Interval makeInterval(BSONObj base) {
        return Interval(base, true /* startIncluded */, true /* endIncluded */);
    }

    Interval makePointInterval(int value) {
        return makeInterval(BSON("" << value << "" << value));
    }

    std::unique_ptr<GeoNear2DNode> makeGeoNear2d() {
        auto node =
            std::make_unique<GeoNear2DNode>(_nss, buildSimpleIndexEntry(BSON("loc" << "2d")));
        node->nq = &_geoNearQuery;
        return node;
    }

    std::unique_ptr<GeoNear2DSphereNode> makeGeoNear2dSphere() {
        auto node = std::make_unique<GeoNear2DSphereNode>(
            _nss, buildSimpleIndexEntry(BSON("loc" << "2dsphere")));
        node->nq = &_geoNearQuery;
        return node;
    }

    std::unique_ptr<TextMatchNode> makeTextMatch(std::unique_ptr<QuerySolutionNode> child) {
        auto node =
            std::make_unique<TextMatchNode>(_nss,
                                            buildSimpleIndexEntry(BSON("_fts" << "text"
                                                                              << "_ftsx" << 1)),
                                            std::make_unique<fts::FTSQueryImpl>(),
                                            false /* wantTextScore */);
        node->children.push_back(std::move(child));
        return node;
    }

    std::unique_ptr<TextOrNode> makeTextOr(std::unique_ptr<QuerySolutionNode> left,
                                           std::unique_ptr<QuerySolutionNode> right) {
        auto node = std::make_unique<TextOrNode>();
        node->children.push_back(std::move(left));
        node->children.push_back(std::move(right));
        return node;
    }

    std::unique_ptr<CountScanNode> makeCountScan() {
        return std::make_unique<CountScanNode>(_nss, buildSimpleIndexEntry(BSON("a" << 1)));
    }

    std::unique_ptr<DistinctNode> makeDistinctScan(bool isFetching = false) {
        auto distinct = std::make_unique<DistinctNode>(_nss, buildSimpleIndexEntry(BSON("a" << 1)));
        distinct->isFetching = isFetching;
        return distinct;
    }

    std::unique_ptr<FetchNode> makeFetch(std::unique_ptr<QuerySolutionNode> child) {
        return std::make_unique<FetchNode>(std::move(child), _nss);
    }

    std::unique_ptr<OrNode> makeOr(std::vector<std::unique_ptr<QuerySolutionNode>> children) {
        auto orNode = std::make_unique<OrNode>();
        orNode->children = std::move(children);
        return orNode;
    }

    std::unique_ptr<OrNode> makeSingleChildOr(std::unique_ptr<QuerySolutionNode> child) {
        std::vector<std::unique_ptr<QuerySolutionNode>> children;
        children.push_back(std::move(child));
        return makeOr(std::move(children));
    }

    std::unique_ptr<OrNode> makeOr(std::unique_ptr<QuerySolutionNode> left,
                                   std::unique_ptr<QuerySolutionNode> right) {
        std::vector<std::unique_ptr<QuerySolutionNode>> children;
        children.push_back(std::move(left));
        children.push_back(std::move(right));
        return makeOr(std::move(children));
    }

    // The table of (plan, expected counters) cases shared by the tests below.
    std::vector<AccessPathTestCase> makeAccessPathTestCases() {
        std::vector<AccessPathTestCase> cases;
        auto add = [&](std::unique_ptr<QuerySolutionNode> root,
                       std::initializer_list<AccessPathCounter> expected) {
            cases.push_back({makePlan(std::move(root)), makeCounts(expected)});
        };

        // Collection scans
        add(makeCollScan(), {AccessPathCounter::kCollscan});
        add(makeClusteredCollScan(),
            {AccessPathCounter::kClusteredCollscan, AccessPathCounter::kBoundsValueToMaxKey});
        // The presence of a filter doesn't change the access path.
        {
            auto scan = makeCollScan();
            scan->filter = std::make_unique<AndMatchExpression>();
            add(std::move(scan), {AccessPathCounter::kCollscan});
        }

        // Index scans: covered only when no FETCH sits above them.
        add(makeIxScan(), {AccessPathCounter::kCoveredIxscan, AccessPathCounter::kBtreeIxscan});
        add(makeFetch(makeIxScan()),
            {AccessPathCounter::kIxscanFetch, AccessPathCounter::kBtreeIxscan});
        add(makeFetch(makeOr(makeIxScan(), makeIxScan(BSON("b" << 1)))),
            {AccessPathCounter::kIxscanFetch, AccessPathCounter::kBtreeIxscan});
        add(makeOr(makeIxScan(), makeFetch(makeIxScan(BSON("b" << 1)))),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kIxscanFetch,
             AccessPathCounter::kBtreeIxscan});
        add(makeOr(makeFetch(makeIxScan()), makeIxScan(BSON("b" << 1))),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kIxscanFetch,
             AccessPathCounter::kBtreeIxscan});
        // FETCH(OR( FETCH(IXSCAN), IXSCAN )) should not increment `kCoveredIxscan`
        add(makeFetch(makeOr(makeFetch(makeIxScan()), makeIxScan(BSON("b" << 1)))),
            {AccessPathCounter::kIxscanFetch, AccessPathCounter::kBtreeIxscan});
        // Single-child OR should still increment the expected counters
        add(makeSingleChildOr(makeFetch(makeDistinctScan())),
            {AccessPathCounter::kDistinctScanFetch});
        // An OR can union any number of children; the same counters should still apply.
        for (size_t numChildren : {3, 5, 100, 1000}) {
            std::vector<std::unique_ptr<QuerySolutionNode>> children;
            for (size_t i = 0; i < numChildren; ++i) {
                if (i % 2 == 0) {
                    children.push_back(makeIxScan(BSON("a" << 1)));
                } else {
                    children.push_back(makeDistinctScan());
                }
            }
            add(makeOr(std::move(children)),
                {AccessPathCounter::kCoveredIxscan,
                 AccessPathCounter::kBtreeIxscan,
                 AccessPathCounter::kDistinctScan});
        }

        // Count and distinct scans. The distinct scan can fetch by itself or under a FETCH stage.
        add(makeCountScan(), {AccessPathCounter::kCountScan});
        add(makeDistinctScan(), {AccessPathCounter::kDistinctScan});
        add(makeDistinctScan(true /* isFetching */), {AccessPathCounter::kDistinctScanFetch});
        add(makeFetch(makeDistinctScan()), {AccessPathCounter::kDistinctScanFetch});

        // Special access paths.
        add(makeGeoNear2d(), {AccessPathCounter::kGeoNear2d});
        add(makeGeoNear2dSphere(), {AccessPathCounter::kGeoNear2dSphere});
        add(std::make_unique<EofNode>(eof_node::PredicateEvalsToFalse),
            {AccessPathCounter::kOtherAccessPath});
        // A text plan that needs the text score unions its index scans under a TEXT_OR, which
        // fetches the documents it outputs to score them, so its index scans are not covered.
        add(makeTextMatch(makeTextOr(makeIxScan(), makeIxScan())),
            {AccessPathCounter::kTextMatch,
             AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan});
        // A text plan that doesn't need the text score fetches with a plain FETCH stage.
        add(makeTextMatch(makeFetch(makeIxScan())),
            {AccessPathCounter::kTextMatch,
             AccessPathCounter::kIxscanFetch,
             AccessPathCounter::kBtreeIxscan});

        // Index types, derived from the key pattern or set on the index entry directly.
        add(makeIxScan(BSON("$**" << 1)),
            {AccessPathCounter::kCoveredIxscan, AccessPathCounter::kWildcardIxscan});
        add(makeIxScan(BSON("a" << "hashed")),
            {AccessPathCounter::kCoveredIxscan, AccessPathCounter::kHashedIxscan});
        {
            auto scan = makeIxScan();
            scan->index.sparse = true;
            add(std::move(scan),
                {AccessPathCounter::kCoveredIxscan,
                 AccessPathCounter::kBtreeIxscan,
                 AccessPathCounter::kSparseIxscan});
        }
        {
            auto scan = makeIxScan();
            scan->index.unique = true;
            add(std::move(scan),
                {AccessPathCounter::kCoveredIxscan,
                 AccessPathCounter::kBtreeIxscan,
                 AccessPathCounter::kUniqueIxscan});
        }
        {
            auto scan = makeIxScan();
            scan->index.multikey = true;
            add(std::move(scan),
                {AccessPathCounter::kCoveredIxscan,
                 AccessPathCounter::kBtreeIxscan,
                 AccessPathCounter::kMultikeyIxscan});
        }
        // The index type counters are not mutually exclusive and also apply to fetched scans.
        {
            auto scan = makeIxScan();
            scan->index.sparse = true;
            scan->index.unique = true;
            add(makeFetch(std::move(scan)),
                {AccessPathCounter::kIxscanFetch,
                 AccessPathCounter::kBtreeIxscan,
                 AccessPathCounter::kSparseIxscan,
                 AccessPathCounter::kUniqueIxscan});
        }

        // Index bounds with a single interval per field, in both scan directions.
        add(makeIxScanWithBounds({{makeInterval(BSON("" << MINKEY << "" << MAXKEY))}}),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan,
             AccessPathCounter::kBoundsFullScan});
        add(makeIxScanWithBounds({{makeInterval(BSON("" << MAXKEY << "" << MINKEY))}}),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan,
             AccessPathCounter::kBoundsFullScan});
        add(makeIxScanWithBounds({{makePointInterval(5)}}),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan,
             AccessPathCounter::kBoundsPoint});
        add(makeIxScanWithBounds({{makeInterval(BSON("" << 3 << "" << 7))}}),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan,
             AccessPathCounter::kBoundsBoundedRange});
        add(makeIxScanWithBounds({{makeInterval(BSON("" << MINKEY << "" << 7))}}),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan,
             AccessPathCounter::kBoundsMinKeyToValue});
        add(makeIxScanWithBounds({{makeInterval(BSON("" << 7 << "" << MINKEY))}}),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan,
             AccessPathCounter::kBoundsMinKeyToValue});
        add(makeIxScanWithBounds({{makeInterval(BSON("" << 3 << "" << MAXKEY))}}),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan,
             AccessPathCounter::kBoundsValueToMaxKey});
        add(makeIxScanWithBounds({{makeInterval(BSON("" << MAXKEY << "" << 3))}}),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan,
             AccessPathCounter::kBoundsValueToMaxKey});

        // Compound bounds classify every field's intervals together: a scan whose intervals all
        // share one bound type sets that type, but a mix of types sets kBoundsMixture instead.
        add(makeIxScanWithBounds(
                {{makePointInterval(5)}, {makeInterval(BSON("" << MINKEY << "" << MAXKEY))}}),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan,
             AccessPathCounter::kBoundsMixture});
        add(makeIxScanWithBounds(
                {{makePointInterval(5)}, {makeInterval(BSON("" << 3 << "" << 7))}}),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan,
             AccessPathCounter::kBoundsMixture});
        add(makeIxScanWithBounds({{makeInterval(BSON("" << MINKEY << "" << MAXKEY))},
                                  {makeInterval(BSON("" << MINKEY << "" << MAXKEY))}}),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan,
             AccessPathCounter::kBoundsFullScan});

        // Composite bounds, e.g. from $in: at most 50 intervals is small, 51+ is large. The
        // intervals are still classified by type, so these point-valued bounds also set
        // kBoundsPoint.
        add(makeIxScanWithBounds({{makePointInterval(1), makePointInterval(2)}}),
            {AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan,
             AccessPathCounter::kBoundsPoint,
             AccessPathCounter::kBoundsUnionedSmall});
        {
            std::vector<Interval> fifty;
            for (int i = 0; i < 50; ++i) {
                fifty.push_back(makePointInterval(i));
            }
            add(makeIxScanWithBounds({std::move(fifty)}),
                {AccessPathCounter::kCoveredIxscan,
                 AccessPathCounter::kBtreeIxscan,
                 AccessPathCounter::kBoundsPoint,
                 AccessPathCounter::kBoundsUnionedSmall});
        }
        {
            std::vector<Interval> fiftyOne;
            for (int i = 0; i < 51; ++i) {
                fiftyOne.push_back(makePointInterval(i));
            }
            add(makeIxScanWithBounds({std::move(fiftyOne)}),
                {AccessPathCounter::kCoveredIxscan,
                 AccessPathCounter::kBtreeIxscan,
                 AccessPathCounter::kBoundsPoint,
                 AccessPathCounter::kBoundsUnionedLarge});
        }

        // Simple-range bounds.
        {
            auto scan = makeIxScan();
            scan->bounds.isSimpleRange = true;
            scan->bounds.startKey = BSON("" << 3);
            scan->bounds.endKey = BSON("" << 7);
            add(std::move(scan),
                {AccessPathCounter::kCoveredIxscan,
                 AccessPathCounter::kBtreeIxscan,
                 AccessPathCounter::kBoundsBoundedRange});
        }
        {
            auto scan = makeIxScan();
            scan->bounds.isSimpleRange = true;
            scan->bounds.startKey = BSON("" << MINKEY);
            scan->bounds.endKey = BSON("" << MAXKEY);
            add(std::move(scan),
                {AccessPathCounter::kCoveredIxscan,
                 AccessPathCounter::kBtreeIxscan,
                 AccessPathCounter::kBoundsFullScan});
        }
        {
            auto scan = makeIxScan();
            scan->bounds.isSimpleRange = true;
            scan->bounds.startKey = BSON("" << "term"
                                            << "" << 100);
            scan->bounds.endKey = BSON("" << "term"
                                          << "" << 0);
            add(std::move(scan),
                {AccessPathCounter::kCoveredIxscan,
                 AccessPathCounter::kBtreeIxscan,
                 AccessPathCounter::kBoundsMixture});
        }

        // Bounds counters also apply to fetched index scans.
        add(makeFetch(makeIxScanWithBounds({{makePointInterval(5)}})),
            {AccessPathCounter::kIxscanFetch,
             AccessPathCounter::kBtreeIxscan,
             AccessPathCounter::kBoundsPoint});

        // One plan can use several access paths.
        add(makeOr(makeCollScan(), makeIxScan()),
            {AccessPathCounter::kCollscan,
             AccessPathCounter::kCoveredIxscan,
             AccessPathCounter::kBtreeIxscan});

        return cases;
    }

protected:
    NamespaceString _nss;
    // Geo near nodes hold an unowned pointer to their query, so the test owns one here.
    GeoNearExpression _geoNearQuery;
};

// Every case sets exactly its expected counters.
TEST_F(PlanAccessPathCountersTest, ExpectedAccessPathCounters) {
    for (auto&& [solution, expected] : makeAccessPathTestCases()) {
        assertCounts(*solution, expected, str::stream() << "expected " << countsToString(expected));
    }
}

// This test will fail in case access path counters are added to, but the new counter is not
// tested.
TEST_F(PlanAccessPathCountersTest, TestCasesCoverEveryCounter) {
    AccessPathCounts covered;
    for (auto&& testCase : makeAccessPathTestCases()) {
        covered.flags |= testCase.expected.flags;
    }
    for (size_t i = 0; i < kNumAccessPathCounters; ++i) {
        ASSERT(covered.test(static_cast<AccessPathCounter>(i)))
            << "no test case expects counter " << toStringData(static_cast<AccessPathCounter>(i));
    }
}

// Access path counters cover the find layer only: subtrees hanging off of stages layered on top of
// the find layer by pushdown are not counted.
TEST_F(PlanAccessPathCountersTest, CountsExcludeExtensionNodes) {
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
    auto solution = makePlan(makeFetch(makeIxScan()));
    solution->extendWith(std::move(eqLookup));

    // The foreign-side collection scan is not part of the find layer, so it is not counted.
    assertCounts(*solution,
                 makeCounts({AccessPathCounter::kIxscanFetch, AccessPathCounter::kBtreeIxscan}),
                 "lookup-extended plan");
}

TEST_F(PlanAccessPathCountersTest, AllCounterNamesAreUnique) {
    std::vector<std::string_view> seen;
    for (size_t i = 0; i < kNumAccessPathCounters; ++i) {
        const auto name = toStringData(static_cast<AccessPathCounter>(i));
        ASSERT_FALSE(name.empty()) << "counter " << i << " has an empty name";
        ASSERT(std::find(seen.begin(), seen.end(), name) == seen.end())
            << "duplicate counter name " << name;
        seen.push_back(name);
    }
}

}  // namespace
}  // namespace mongo::plan_shape_counters
