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

#include "mongo/db/query/compiler/optimizer/join/reorder_joins.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/db/query/compiler/optimizer/join/plan_enumerator_helpers.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {

unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/query/join"};

QuerySolutionMap cloneSolnMap(const QuerySolutionMap& qsm) {
    QuerySolutionMap ret;
    for (auto&& [cq, qs] : qsm) {
        auto newQs = std::make_unique<QuerySolution>();
        newQs->setRoot(qs->root()->clone());
        ret.insert({cq, std::move(newQs)});
    }
    return ret;
}

class ReorderGraphTest : public JoinOrderingTestFixture {
protected:
    // Helper struct for intializing a namespace, its embedding, any residual filter, and indexes
    // that are available.
    struct TestNamespaceParams {
        const std::string& collName;
        boost::optional<FieldPath> embedPath;
        BSONObj filter;
        std::vector<BSONObj> indexes;
    };

    NodeId addNssWithEmbedding(TestNamespaceParams&& params) {
        auto nss = NamespaceString::createNamespaceString_forTest("test", params.collName);
        namespaces.push_back(nss);
        auto cq = makeCanonicalQuery(nss, params.filter);
        auto cs = makeCollScanPlan(nss, cq->getPrimaryMatchExpression()->clone());
        auto p = std::pair<mongo::CanonicalQuery*, std::unique_ptr<mongo::QuerySolution>>{
            cq.get(), std::move(cs)};
        jCtx.cbrCqQsns.insert(std::move(p));
        jCtx.perCollIdxs.emplace(nss, makeIndexCatalogEntries(std::move(params.indexes)));
        return *graph.addNode(nss, std::move(cq), params.embedPath);
    }

    void outputSolutions(std::ostream& out) {
        // Ensure each solution has a different base node.
        std::set<NodeId> baseNodes;
        for (auto seed : seeds) {
            auto clonedMap = cloneSolnMap(jCtx.cbrCqQsns);
            auto r = constructSolutionWithRandomOrder(jCtx, seed);
            ASSERT(r.soln);
            // Ensure our seeds produce different base collections.
            ASSERT(!baseNodes.contains(r.baseNode));
            baseNodes.emplace(r.baseNode);

            out << "Solution with seed " << seed << ":" << std::endl;
            out << r.soln->toString() << std::endl;
        }
    }
};

TEST_F(ReorderGraphTest, SimpleGraph) {
    // Show that we can reorder the base in the simplest case.
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    goldenCtx.outStream() << "Graph:\nA -- B" << std::endl;
    seeds = {0, 1};

    auto id1 = addNssWithEmbedding({.collName = "a", .embedPath = {}, .filter = {}, .indexes = {}});
    auto id2 = addNssWithEmbedding(
        {.collName = "b", .embedPath = FieldPath{"b"}, .filter = {}, .indexes = {}});

    resolvedPaths = {
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.b*/);

    outputSolutions(goldenCtx.outStream());
}

TEST_F(ReorderGraphTest, TwoJoins) {
    // This graph looks like:
    // C -- A -- B
    // where A is the main collection.
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    goldenCtx.outStream() << "Graph:\nC -- A -- B" << std::endl;

    auto id1 = addNssWithEmbedding({.collName = "a", .embedPath = {}, .filter = {}, .indexes = {}});
    auto id2 = addNssWithEmbedding(
        {.collName = "b", .embedPath = FieldPath{"b"}, .filter = {}, .indexes = {}});
    auto id3 = addNssWithEmbedding(
        {.collName = "c", .embedPath = FieldPath{"c"}, .filter = {}, .indexes = {}});

    resolvedPaths = {
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
        ResolvedPath{.nodeId = id3, .fieldName = FieldPath{"c"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.b*/);
    graph.addSimpleEqualityEdge(id1, id3, 0 /*a*/, 2 /*c.c*/);

    seeds = {0, 4};

    // Demonstrate that different join orders are constructed with different seeds
    outputSolutions(goldenCtx.outStream());
}

TEST_F(ReorderGraphTest, SimpleINLJ) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    goldenCtx.outStream() << "Graph:\nA -- B" << std::endl;

    // Create namespaces with indexes.
    auto id1 = addNssWithEmbedding(
        {.collName = "a", .embedPath = {}, .filter = {}, .indexes = {BSON("a" << 1)}});
    auto id2 = addNssWithEmbedding(
        {.collName = "b", .embedPath = FieldPath{"b"}, .filter = {}, .indexes = {BSON("b" << 1)}});

    resolvedPaths = {
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.b*/);

    seeds = {0, 1};

    // Demonstrate that different join orders are constructed with different seeds
    outputSolutions(goldenCtx.outStream());
}

TEST_F(ReorderGraphTest, SimpleINLJSwapEdge) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    goldenCtx.outStream() << "Graph:\nB -- A" << std::endl;

    // Create namespaces with indexes.
    auto id1 = addNssWithEmbedding(
        {.collName = "a", .embedPath = {}, .filter = {}, .indexes = {BSON("a" << 1)}});
    auto id2 = addNssWithEmbedding(
        {.collName = "b", .embedPath = FieldPath{"b"}, .filter = {}, .indexes = {BSON("b" << 1)}});

    resolvedPaths = {
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
    };

    // Swap edge dir compared to previous.
    graph.addSimpleEqualityEdge(id2, id1, 1 /*b.b*/, 0 /*a*/);

    seeds = {0, 1};

    // Demonstrate that different join orders are constructed with different seeds
    outputSolutions(goldenCtx.outStream());
}

TEST_F(ReorderGraphTest, MultipleINLJ) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    // This graph looks like:
    // C -- A -- B
    // where A is the main collection.
    goldenCtx.outStream() << "Graph:\nC -- A -- B" << std::endl;

    auto id1 = addNssWithEmbedding({.collName = "a", .embedPath = {}, .filter = {}, .indexes = {}});
    auto id2 = addNssWithEmbedding(
        {.collName = "b", .embedPath = FieldPath{"b"}, .filter = {}, .indexes = {BSON("b" << 1)}});
    auto id3 = addNssWithEmbedding(
        {.collName = "c", .embedPath = FieldPath{"c"}, .filter = {}, .indexes = {BSON("c" << 1)}});

    resolvedPaths = {
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
        ResolvedPath{.nodeId = id3, .fieldName = FieldPath{"c"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.b*/);
    graph.addSimpleEqualityEdge(id1, id3, 0 /*a*/, 2 /*c.c*/);

    seeds = {5, 7};
    // Demonstrate that different join orders are constructed with different seeds
    outputSolutions(goldenCtx.outStream());
}

TEST_F(ReorderGraphTest, JoinWithDeps) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    /**
     * This graph looks like:
     * BASE -- F1 -- F2
     * where BASE is the main collection.
     *
     * Example agg on collection "base": [
     *  {$lookup: {from: “f1”, localField: “a”, foreignField: “a”, as: “f1”}},
     *  {$unwind: “$f1”},
     *  {$lookup: {from: “f2”, localField: “f1.c”, foreignField: “c”, as: “f2”}},
     *  {$unwind: “$f2”}
     * ]
     */
    goldenCtx.outStream() << "Graph:\nBASE -- F1 -- F2" << std::endl;

    auto id1 =
        addNssWithEmbedding({.collName = "base", .embedPath = {}, .filter = {}, .indexes = {}});
    auto id2 = addNssWithEmbedding(
        {.collName = "f1", .embedPath = FieldPath{"f1"}, .filter = {}, .indexes = {}});
    auto id3 = addNssWithEmbedding(
        {.collName = "f2", .embedPath = FieldPath{"f2"}, .filter = {}, .indexes = {}});

    resolvedPaths = {
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"c"}},
        ResolvedPath{.nodeId = id3, .fieldName = FieldPath{"c"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*f1.a*/);
    graph.addSimpleEqualityEdge(id2, id3, 2 /*f1.c*/, 3 /*f2.c*/);

    seeds = {0, 4};

    // Demonstrate that different join orders are constructed with different seeds
    outputSolutions(goldenCtx.outStream());
}

TEST_F(ReorderGraphTest, INLJResidualPred) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    auto id1 = addNssWithEmbedding(
        {.collName = "a", .embedPath = {}, .filter = {}, .indexes = {BSON("a" << 1)}});

    BSONObj filter = fromjson("{b: {$gt: 5}}");
    auto id2 = addNssWithEmbedding({.collName = "b",
                                    .embedPath = FieldPath{"b"},
                                    .filter = filter,
                                    .indexes = {BSON("b" << 1)}});

    resolvedPaths = {
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.b*/);

    seeds = {0};

    goldenCtx.outStream() << "Graph:\nA -- B" << std::endl;
    outputSolutions(goldenCtx.outStream());
}

// Probe using prefix
TEST_F(ReorderGraphTest, INLJUseIndexPrefix) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    auto id1 = addNssWithEmbedding(
        {.collName = "a", .embedPath = {}, .filter = {}, .indexes = {BSON("a" << 1)}});

    auto id2 = addNssWithEmbedding({.collName = "b",
                                    .embedPath = FieldPath{"b"},
                                    .filter = {},
                                    .indexes = {BSON("b" << 1 << "c" << 1)}});

    resolvedPaths = {
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.b*/);

    seeds = {0};

    goldenCtx.outStream() << "Graph:\nA -- B" << std::endl;
    outputSolutions(goldenCtx.outStream());
}

// Index {b: 1, c: 1} cannot be used to satisfy join predicate on c
TEST_F(ReorderGraphTest, AvoidINLJOverIneligibleIndex) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    auto id1 = addNssWithEmbedding(
        {.collName = "a", .embedPath = {}, .filter = {}, .indexes = {BSON("a" << 1)}});
    auto id2 = addNssWithEmbedding({.collName = "b",
                                    .embedPath = FieldPath{"c"},
                                    .filter = {},
                                    .indexes = {BSON("b" << 1 << "c" << 1)}});

    resolvedPaths = {
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"c"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.c*/);

    seeds = {0, 1};

    goldenCtx.outStream() << "Graph:\nA -- B" << std::endl;
    outputSolutions(goldenCtx.outStream());
}

TEST_F(ReorderGraphTest, INLJCompoundJoinPredicate) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    auto id1 = addNssWithEmbedding(
        {.collName = "a", .embedPath = {}, .filter = {}, .indexes = {BSON("a" << 1)}});

    auto id2 = addNssWithEmbedding({.collName = "b",
                                    .embedPath = FieldPath{"b"},
                                    .filter = {},
                                    .indexes = {BSON("c" << 1 << "d" << 1)}});

    resolvedPaths = {
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"b"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"c"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"d"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 2 /*b.c*/);
    graph.addSimpleEqualityEdge(id1, id2, 1 /*b*/, 3 /*b.d*/);

    seeds = {0};

    goldenCtx.outStream() << "Graph:\nA -- B" << std::endl;
    outputSolutions(goldenCtx.outStream());
}

IndexedJoinPredicate makeIndexedPredicate(std::string path) {
    return IndexedJoinPredicate{
        .op = QSNJoinPredicate::ComparisonOp::Eq,
        .field = path,
    };
}

TEST(IndexSatisfiesJoinPredicates, CompoundIndex) {
    ASSERT_TRUE(indexSatisfiesJoinPredicates(
        fromjson("{a: 1, b: 1}"), std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a")}));
    ASSERT_TRUE(indexSatisfiesJoinPredicates(
        fromjson("{a: 1, b: 1}"),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a"), makeIndexedPredicate("b")}));
    // Predicates in different order than index components
    ASSERT_TRUE(indexSatisfiesJoinPredicates(
        fromjson("{a: 1, b: 1}"),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("b"), makeIndexedPredicate("a")}));
    ASSERT_TRUE(indexSatisfiesJoinPredicates(
        fromjson("{a: 1, b: 1, c: 1}"),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a"), makeIndexedPredicate("b")}));

    // Not using prefix
    ASSERT_FALSE(indexSatisfiesJoinPredicates(
        fromjson("{a: 1, b: 1}"), std::vector<IndexedJoinPredicate>{makeIndexedPredicate("b")}));
    ASSERT_FALSE(indexSatisfiesJoinPredicates(
        fromjson("{a: 1, b: 1, c: 1}"),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("b"), makeIndexedPredicate("c")}));
    ASSERT_FALSE(indexSatisfiesJoinPredicates(
        fromjson("{a: 1, b: 1, c: 1}"),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a"), makeIndexedPredicate("c")}));
    // Not all components eligle to be probed
    ASSERT_FALSE(indexSatisfiesJoinPredicates(
        fromjson("{a: 1, b: 1}"), std::vector<IndexedJoinPredicate>{makeIndexedPredicate("c")}));
    ASSERT_FALSE(indexSatisfiesJoinPredicates(
        fromjson("{a: 1, b: 1}"),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a"), makeIndexedPredicate("c")}));
}

TEST(IndexSatisfiesJoinPredicates, DottedPaths) {
    ASSERT_TRUE(indexSatisfiesJoinPredicates(
        fromjson("{'a.a': 1, 'b.b': 1}"),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a.a")}));
    ASSERT_TRUE(indexSatisfiesJoinPredicates(
        fromjson("{'a.a': 1, 'b.b': 1}"),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a.a"),
                                          makeIndexedPredicate("b.b")}));
    ASSERT_FALSE(indexSatisfiesJoinPredicates(
        fromjson("{'a.a': 1, 'b.b': 1}"),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("b.b")}));
}

TEST(IndexSatisfyingJoinPredicates, PreferShorterKeyPattern) {
    auto indexEntries = makeIndexCatalogEntries({
        fromjson("{a: 1, b: 1, c: 1}"),
        fromjson("{a: 1, b: 1}"),
    });
    auto res = bestIndexSatisfyingJoinPredicates(indexEntries,
                                                 std::vector<IndexedJoinPredicate>{
                                                     makeIndexedPredicate("a"),
                                                     makeIndexedPredicate("b"),
                                                 });
    ASSERT_NE(res, nullptr);
    ASSERT_BSONOBJ_EQ(BSON("a" << 1 << "b" << 1), res->descriptor()->keyPattern());
}

TEST(IndexSatisfyingJoinPredicates, SameNumberOfKeys) {
    auto indexEntries = makeIndexCatalogEntries({
        fromjson("{a: 1, b: 1, d: 1}"),
        fromjson("{a: 1, b: 1, c: 1}"),
    });
    auto res = bestIndexSatisfyingJoinPredicates(indexEntries,
                                                 std::vector<IndexedJoinPredicate>{
                                                     makeIndexedPredicate("a"),
                                                     makeIndexedPredicate("b"),
                                                 });
    ASSERT_NE(res, nullptr);
    ASSERT_BSONOBJ_EQ(fromjson("{a: 1, b: 1, c: 1}"), res->descriptor()->keyPattern());
}

TEST(IndexSatisfyingJoinPredicates, NoSatisfyingIndex) {
    auto indexEntries = makeIndexCatalogEntries({
        fromjson("{a: 1, b: 1, d: 1}"),
        fromjson("{a: 1, b: 1, c: 1}"),
    });
    auto res = bestIndexSatisfyingJoinPredicates(indexEntries,
                                                 std::vector<IndexedJoinPredicate>{
                                                     makeIndexedPredicate("a"),
                                                     makeIndexedPredicate("c"),
                                                 });
    ASSERT_EQ(res, nullptr);
}

}  // namespace mongo::join_ordering
