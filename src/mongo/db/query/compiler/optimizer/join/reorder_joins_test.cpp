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

#include "mongo/bson/json.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/index_catalog_entry_mock.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {

unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/query/join"};

class ReorderGraphTest : public CatalogTestFixture {
protected:
    std::unique_ptr<CanonicalQuery> makeCanonicalQuery(NamespaceString nss,
                                                       BSONObj filter = BSONObj::kEmptyObject) {
        auto expCtx = ExpressionContextBuilder{}.opCtx(operationContext()).build();
        if (!filter.isEmpty()) {
            auto swFindCmd = ParsedFindCommand::withExistingFilter(
                expCtx,
                nullptr,
                std::move(MatchExpressionParser::parse(filter, expCtx).getValue()),
                std::make_unique<FindCommandRequest>(nss),
                ProjectionPolicies::aggregateProjectionPolicies());
            ASSERT_OK(swFindCmd.getStatus());
            return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
                .expCtx = expCtx, .parsedFind = std::move(swFindCmd.getValue())});
        }
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = expCtx, .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    }

    std::unique_ptr<QuerySolution> makeCollScanPlan(
        NamespaceString nss, std::unique_ptr<MatchExpression> filter = nullptr) {
        auto scan = std::make_unique<CollectionScanNode>();
        scan->nss = nss;
        scan->filter = std::move(filter);
        auto soln = std::make_unique<QuerySolution>();
        soln->setRoot(std::move(scan));
        return soln;
    }

    void createCollection(NamespaceString nss) {
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));
    }
};

QuerySolutionMap cloneSolnMap(const QuerySolutionMap& qsm) {
    QuerySolutionMap ret;
    for (auto&& [cq, qs] : qsm) {
        auto newQs = std::make_unique<QuerySolution>();
        newQs->setRoot(qs->root()->clone());
        ret.insert({cq, std::move(newQs)});
    }
    return ret;
}

TEST_F(ReorderGraphTest, SimpleGraph) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    JoinGraph graph;

    auto nss1 = NamespaceString::createNamespaceString_forTest("test", "a");
    auto nss2 = NamespaceString::createNamespaceString_forTest("test", "b");
    createCollection(nss1);
    createCollection(nss2);

    QuerySolutionMap solnsPerQuery;

    auto cq1 = makeCanonicalQuery(nss1);
    solnsPerQuery.insert({cq1.get(), makeCollScanPlan(nss1)});
    auto id1 = graph.addNode(nss1, std::move(cq1), boost::none);
    auto cq2 = makeCanonicalQuery(nss2);
    solnsPerQuery.insert({cq2.get(), makeCollScanPlan(nss2)});
    auto id2 = graph.addNode(nss2, std::move(cq2), FieldPath{"b"});

    std::vector<ResolvedPath> resolvedPaths{
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.b*/);

    auto soln = constructSolutionWithRandomOrder(
        std::move(solnsPerQuery),
        graph,
        resolvedPaths,
        multipleCollectionAccessor(operationContext(), {nss1, nss2}),
        0);
    ASSERT(soln);

    goldenCtx.outStream() << "Graph:\nA -- B" << std::endl;
    goldenCtx.outStream() << soln->toString() << std::endl;
}

TEST_F(ReorderGraphTest, TwoJoins) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    // This graph looks like:
    // C -- A -- B
    // where A is the main collection.
    JoinGraph graph;

    auto nss1 = NamespaceString::createNamespaceString_forTest("test", "a");
    auto nss2 = NamespaceString::createNamespaceString_forTest("test", "b");
    auto nss3 = NamespaceString::createNamespaceString_forTest("test", "c");
    createCollection(nss1);
    createCollection(nss2);
    createCollection(nss3);

    QuerySolutionMap solnsPerQuery;

    auto cq1 = makeCanonicalQuery(nss1);
    solnsPerQuery.insert({cq1.get(), makeCollScanPlan(nss1)});
    auto id1 = graph.addNode(nss1, std::move(cq1), boost::none);
    auto cq2 = makeCanonicalQuery(nss2);
    solnsPerQuery.insert({cq2.get(), makeCollScanPlan(nss2)});
    auto id2 = graph.addNode(nss2, std::move(cq2), FieldPath{"b"});
    auto cq3 = makeCanonicalQuery(nss3);
    solnsPerQuery.insert({cq3.get(), makeCollScanPlan(nss3)});
    auto id3 = graph.addNode(nss3, std::move(cq3), FieldPath{"c"});

    std::vector<ResolvedPath> resolvedPaths{
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
        ResolvedPath{.nodeId = id3, .fieldName = FieldPath{"c"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.b*/);
    graph.addSimpleEqualityEdge(id1, id3, 0 /*a*/, 2 /*c.c*/);

    auto solnsPerQueryCopy = cloneSolnMap(solnsPerQuery);

    auto soln = constructSolutionWithRandomOrder(
        std::move(solnsPerQuery),
        graph,
        resolvedPaths,
        multipleCollectionAccessor(operationContext(), {nss1, nss2, nss3}),
        0);
    auto soln2 = constructSolutionWithRandomOrder(
        std::move(solnsPerQueryCopy),
        graph,
        resolvedPaths,
        multipleCollectionAccessor(operationContext(), {nss1, nss2, nss3}),
        1);
    ASSERT(soln);
    ASSERT(soln2);

    goldenCtx.outStream() << "Graph:\nC -- A -- B" << std::endl;

    // Demonstrate that different join orders are constructed with different seeds
    goldenCtx.outStream() << "Solution with seed 0:" << std::endl;
    goldenCtx.outStream() << soln->toString() << std::endl;
    goldenCtx.outStream() << "Solution with seed 1:" << std::endl;
    goldenCtx.outStream() << soln2->toString() << std::endl;
}

TEST_F(ReorderGraphTest, SimpleINLJ) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    auto nss1 = NamespaceString::createNamespaceString_forTest("test", "a");
    auto nss2 = NamespaceString::createNamespaceString_forTest("test", "b");
    createCollection(nss1);
    createCollection(nss2);
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        operationContext(), nss1, {BSON("v" << 2 << "name" << "a_1" << "key" << BSON("a" << 1))}));
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        operationContext(), nss2, {BSON("v" << 2 << "name" << "b_1" << "key" << BSON("b" << 1))}));

    auto mca = multipleCollectionAccessor(operationContext(), {nss1, nss2});

    {
        JoinGraph graph;
        QuerySolutionMap solnsPerQuery;

        auto cq1 = makeCanonicalQuery(nss1);
        solnsPerQuery.insert({cq1.get(), makeCollScanPlan(nss1)});
        auto id1 = graph.addNode(nss1, std::move(cq1), boost::none);
        auto cq2 = makeCanonicalQuery(nss2);
        solnsPerQuery.insert({cq2.get(), makeCollScanPlan(nss2)});
        auto id2 = graph.addNode(nss2, std::move(cq2), FieldPath{"b"});

        std::vector<ResolvedPath> resolvedPaths{
            ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
            ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
        };

        graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.b*/);

        auto soln = constructSolutionWithRandomOrder(
            std::move(solnsPerQuery), graph, resolvedPaths, mca, 0);
        ASSERT(soln);

        goldenCtx.outStream() << "Graph:\nA -- B" << std::endl;
        goldenCtx.outStream() << soln->toString() << std::endl;
    }

    {
        JoinGraph graph;
        QuerySolutionMap solnsPerQuery;

        auto cq1 = makeCanonicalQuery(nss1);
        solnsPerQuery.insert({cq1.get(), makeCollScanPlan(nss1)});
        auto id1 = graph.addNode(nss1, std::move(cq1), boost::none);
        auto cq2 = makeCanonicalQuery(nss2);
        solnsPerQuery.insert({cq2.get(), makeCollScanPlan(nss2)});
        auto id2 = graph.addNode(nss2, std::move(cq2), FieldPath{"b"});

        std::vector<ResolvedPath> resolvedPaths{
            ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
            ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
        };

        // Swapped edge from above example
        graph.addSimpleEqualityEdge(id2, id1, 1 /*b.b*/, 0 /*a*/);

        auto soln = constructSolutionWithRandomOrder(
            std::move(solnsPerQuery), graph, resolvedPaths, mca, 0);
        ASSERT(soln);

        goldenCtx.outStream() << "Graph:\nB -- A" << std::endl;
        goldenCtx.outStream() << soln->toString() << std::endl;
    }
}

TEST_F(ReorderGraphTest, MultipleINLJ) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    // This graph looks like:
    // C -- A -- B
    // where A is the main collection.
    JoinGraph graph;

    auto nss1 = NamespaceString::createNamespaceString_forTest("test", "a");
    auto nss2 = NamespaceString::createNamespaceString_forTest("test", "b");
    auto nss3 = NamespaceString::createNamespaceString_forTest("test", "c");
    createCollection(nss1);
    createCollection(nss2);
    createCollection(nss3);
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        operationContext(), nss2, {BSON("v" << 2 << "name" << "b_1" << "key" << BSON("b" << 1))}));
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        operationContext(), nss3, {BSON("v" << 2 << "name" << "c_1" << "key" << BSON("c" << 1))}));

    QuerySolutionMap solnsPerQuery;

    auto cq1 = makeCanonicalQuery(nss1);
    solnsPerQuery.insert({cq1.get(), makeCollScanPlan(nss1)});
    auto id1 = graph.addNode(nss1, std::move(cq1), boost::none);
    auto cq2 = makeCanonicalQuery(nss2);
    solnsPerQuery.insert({cq2.get(), makeCollScanPlan(nss2)});
    auto id2 = graph.addNode(nss2, std::move(cq2), FieldPath{"b"});
    auto cq3 = makeCanonicalQuery(nss3);
    solnsPerQuery.insert({cq3.get(), makeCollScanPlan(nss3)});
    auto id3 = graph.addNode(nss3, std::move(cq3), FieldPath{"c"});

    std::vector<ResolvedPath> resolvedPaths{
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
        ResolvedPath{.nodeId = id3, .fieldName = FieldPath{"c"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.b*/);
    graph.addSimpleEqualityEdge(id1, id3, 0 /*a*/, 2 /*c.c*/);

    auto solnsPerQueryCopy = cloneSolnMap(solnsPerQuery);

    auto soln = constructSolutionWithRandomOrder(
        std::move(solnsPerQuery),
        graph,
        resolvedPaths,
        multipleCollectionAccessor(operationContext(), {nss1, nss2, nss3}),
        0);
    auto soln2 = constructSolutionWithRandomOrder(
        std::move(solnsPerQueryCopy),
        graph,
        resolvedPaths,
        multipleCollectionAccessor(operationContext(), {nss1, nss2, nss3}),
        1);
    ASSERT(soln);
    ASSERT(soln2);

    goldenCtx.outStream() << "Graph:\nC -- A -- B" << std::endl;

    // Demonstrate that different join orders are constructed with different seeds
    goldenCtx.outStream() << "Solution with seed 0:" << std::endl;
    goldenCtx.outStream() << soln->toString() << std::endl;
    goldenCtx.outStream() << "Solution with seed 1:" << std::endl;
    goldenCtx.outStream() << soln2->toString() << std::endl;
}

TEST_F(ReorderGraphTest, INLJResidualPred) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    auto nss1 = NamespaceString::createNamespaceString_forTest("test", "a");
    auto nss2 = NamespaceString::createNamespaceString_forTest("test", "b");
    createCollection(nss1);
    createCollection(nss2);
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        operationContext(), nss1, {BSON("v" << 2 << "name" << "a_1" << "key" << BSON("a" << 1))}));
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        operationContext(), nss2, {BSON("v" << 2 << "name" << "b_1" << "key" << BSON("b" << 1))}));

    auto mca = multipleCollectionAccessor(operationContext(), {nss1, nss2});

    JoinGraph graph;
    QuerySolutionMap solnsPerQuery;

    auto cq1 = makeCanonicalQuery(nss1);
    solnsPerQuery.insert({cq1.get(), makeCollScanPlan(nss1)});
    auto id1 = graph.addNode(nss1, std::move(cq1), boost::none);

    BSONObj filter = fromjson("{b: {$gt: 5}}");
    auto cq2 = makeCanonicalQuery(nss2, filter);
    auto expCtx = ExpressionContextBuilder{}.opCtx(operationContext()).build();
    auto me = std::move(MatchExpressionParser::parse(filter, expCtx).getValue());
    solnsPerQuery.insert({cq2.get(), makeCollScanPlan(nss2, std::move(me))});

    auto id2 = graph.addNode(nss2, std::move(cq2), FieldPath{"b"});

    std::vector<ResolvedPath> resolvedPaths{
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.b*/);

    auto soln =
        constructSolutionWithRandomOrder(std::move(solnsPerQuery), graph, resolvedPaths, mca, 0);
    ASSERT(soln);

    goldenCtx.outStream() << "Graph:\nA -- B" << std::endl;
    goldenCtx.outStream() << soln->toString() << std::endl;
}

// Probe using prefix
TEST_F(ReorderGraphTest, INLJUseIndexPrefix) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    auto nss1 = NamespaceString::createNamespaceString_forTest("test", "a");
    auto nss2 = NamespaceString::createNamespaceString_forTest("test", "b");
    createCollection(nss1);
    createCollection(nss2);
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        operationContext(), nss1, {BSON("v" << 2 << "name" << "a_1" << "key" << BSON("a" << 1))}));
    // Index on {b: 1, c: 1}
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        operationContext(),
        nss2,
        {BSON("v" << 2 << "name" << "b_1_c_1" << "key" << BSON("b" << 1 << "c" << 1))}));

    auto mca = multipleCollectionAccessor(operationContext(), {nss1, nss2});

    JoinGraph graph;
    QuerySolutionMap solnsPerQuery;

    auto cq1 = makeCanonicalQuery(nss1);
    solnsPerQuery.insert({cq1.get(), makeCollScanPlan(nss1)});
    auto id1 = graph.addNode(nss1, std::move(cq1), boost::none);
    auto cq2 = makeCanonicalQuery(nss2);
    solnsPerQuery.insert({cq2.get(), makeCollScanPlan(nss2)});

    auto id2 = graph.addNode(nss2, std::move(cq2), FieldPath{"b"});

    std::vector<ResolvedPath> resolvedPaths{
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.b*/);

    auto soln =
        constructSolutionWithRandomOrder(std::move(solnsPerQuery), graph, resolvedPaths, mca, 0);
    ASSERT(soln);

    goldenCtx.outStream() << "Graph:\nA -- B" << std::endl;
    goldenCtx.outStream() << soln->toString() << std::endl;
}

// Index {b: 1, c: 1} cannot be used to satisfy join predicate on c
TEST_F(ReorderGraphTest, AvoidINLJOverIneligibleIndex) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    auto nss1 = NamespaceString::createNamespaceString_forTest("test", "a");
    auto nss2 = NamespaceString::createNamespaceString_forTest("test", "b");
    createCollection(nss1);
    createCollection(nss2);
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        operationContext(), nss1, {BSON("v" << 2 << "name" << "a_1" << "key" << BSON("a" << 1))}));
    // Index on {b: 1, c: 1}
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        operationContext(),
        nss2,
        {BSON("v" << 2 << "name" << "b_1_c_1" << "key" << BSON("b" << 1 << "c" << 1))}));

    auto mca = multipleCollectionAccessor(operationContext(), {nss1, nss2});

    JoinGraph graph;
    QuerySolutionMap solnsPerQuery;

    auto cq1 = makeCanonicalQuery(nss1);
    solnsPerQuery.insert({cq1.get(), makeCollScanPlan(nss1)});
    auto id1 = graph.addNode(nss1, std::move(cq1), boost::none);
    auto cq2 = makeCanonicalQuery(nss2);
    solnsPerQuery.insert({cq2.get(), makeCollScanPlan(nss2)});
    auto id2 = graph.addNode(nss2, std::move(cq2), FieldPath{"c"});

    std::vector<ResolvedPath> resolvedPaths{
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"c"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.c*/);

    auto soln =
        constructSolutionWithRandomOrder(std::move(solnsPerQuery), graph, resolvedPaths, mca, 0);
    ASSERT(soln);

    goldenCtx.outStream() << "Graph:\nA -- B" << std::endl;
    goldenCtx.outStream() << soln->toString() << std::endl;
}

TEST_F(ReorderGraphTest, INLJCompoundJoinPredicate) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    auto nss1 = NamespaceString::createNamespaceString_forTest("test", "a");
    auto nss2 = NamespaceString::createNamespaceString_forTest("test", "b");
    createCollection(nss1);
    createCollection(nss2);
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        operationContext(), nss1, {BSON("v" << 2 << "name" << "a_1" << "key" << BSON("a" << 1))}));
    // Index on {c: 1, d: 1}
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        operationContext(),
        nss2,
        {BSON("v" << 2 << "name" << "c_1_d_1" << "key" << BSON("c" << 1 << "d" << 1))}));

    auto mca = multipleCollectionAccessor(operationContext(), {nss1, nss2});

    JoinGraph graph;
    QuerySolutionMap solnsPerQuery;

    auto cq1 = makeCanonicalQuery(nss1);
    solnsPerQuery.insert({cq1.get(), makeCollScanPlan(nss1)});
    auto id1 = graph.addNode(nss1, std::move(cq1), boost::none);
    auto cq2 = makeCanonicalQuery(nss2);
    solnsPerQuery.insert({cq2.get(), makeCollScanPlan(nss2)});
    auto id2 = graph.addNode(nss2, std::move(cq2), FieldPath{"b"});

    std::vector<ResolvedPath> resolvedPaths{
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"b"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"c"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"d"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 2 /*b.c*/);
    graph.addSimpleEqualityEdge(id1, id2, 1 /*b*/, 3 /*b.d*/);

    auto soln =
        constructSolutionWithRandomOrder(std::move(solnsPerQuery), graph, resolvedPaths, mca, 0);
    ASSERT(soln);

    goldenCtx.outStream() << "Graph:\nA -- B" << std::endl;
    goldenCtx.outStream() << soln->toString() << std::endl;
}

IndexCatalogEntryMock makeIndexEntry(BSONObj indexSpec) {
    IndexSpec spec;
    spec.version(2).name("name").addKeys(indexSpec);
    auto desc = IndexDescriptor(IndexNames::BTREE, spec.toBSON());
    return IndexCatalogEntryMock{
        nullptr /*opCtx*/, CollectionPtr{}, "" /*ident*/, std::move(desc), false /*isFrozen*/};
}

IndexedJoinPredicate makeIndexedPredicate(std::string path) {
    return IndexedJoinPredicate{
        .op = QSNJoinPredicate::ComparisonOp::Eq,
        .field = path,
    };
}

TEST(IndexSatisfiesJoinPredicates, CompoundIndex) {
    ASSERT_TRUE(
        indexSatisfiesJoinPredicates(makeIndexEntry(fromjson("{a: 1, b: 1}")),
                                     std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a")}));
    ASSERT_TRUE(indexSatisfiesJoinPredicates(
        makeIndexEntry(fromjson("{a: 1, b: 1}")),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a"), makeIndexedPredicate("b")}));
    // Predicates in different order than index components
    ASSERT_TRUE(indexSatisfiesJoinPredicates(
        makeIndexEntry(fromjson("{a: 1, b: 1}")),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("b"), makeIndexedPredicate("a")}));
    ASSERT_TRUE(indexSatisfiesJoinPredicates(
        makeIndexEntry(fromjson("{a: 1, b: 1, c: 1}")),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a"), makeIndexedPredicate("b")}));

    // Not using prefix
    ASSERT_FALSE(
        indexSatisfiesJoinPredicates(makeIndexEntry(fromjson("{a: 1, b: 1}")),
                                     std::vector<IndexedJoinPredicate>{makeIndexedPredicate("b")}));
    ASSERT_FALSE(indexSatisfiesJoinPredicates(
        makeIndexEntry(fromjson("{a: 1, b: 1, c: 1}")),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("b"), makeIndexedPredicate("c")}));
    ASSERT_FALSE(indexSatisfiesJoinPredicates(
        makeIndexEntry(fromjson("{a: 1, b: 1, c: 1}")),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a"), makeIndexedPredicate("c")}));
    // Not all components eligle to be probed
    ASSERT_FALSE(
        indexSatisfiesJoinPredicates(makeIndexEntry(fromjson("{a: 1, b: 1}")),
                                     std::vector<IndexedJoinPredicate>{makeIndexedPredicate("c")}));
    ASSERT_FALSE(indexSatisfiesJoinPredicates(
        makeIndexEntry(fromjson("{a: 1, b: 1}")),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a"), makeIndexedPredicate("c")}));
}

TEST(IndexSatisfiesJoinPredicates, DottedPaths) {
    ASSERT_TRUE(indexSatisfiesJoinPredicates(
        makeIndexEntry(fromjson("{'a.a': 1, 'b.b': 1}")),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a.a")}));
    ASSERT_TRUE(indexSatisfiesJoinPredicates(
        makeIndexEntry(fromjson("{'a.a': 1, 'b.b': 1}")),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("a.a"),
                                          makeIndexedPredicate("b.b")}));
    ASSERT_FALSE(indexSatisfiesJoinPredicates(
        makeIndexEntry(fromjson("{'a.a': 1, 'b.b': 1}")),
        std::vector<IndexedJoinPredicate>{makeIndexedPredicate("b.b")}));
}

}  // namespace mongo::join_ordering
