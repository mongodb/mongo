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

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {

unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/query/join"};

class ReorderGraphTest : public AggregationContextFixture {
protected:
    std::unique_ptr<CanonicalQuery> makeCanonicalQuery(NamespaceString nss) {
        auto expCtx = getExpCtx();
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = expCtx, .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    }

    std::unique_ptr<QuerySolution> makeCollScanPlan(NamespaceString nss) {
        auto scan = std::make_unique<CollectionScanNode>();
        scan->nss = nss;
        auto soln = std::make_unique<QuerySolution>();
        soln->setRoot(std::move(scan));
        return soln;
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

    QuerySolutionMap solnsPerQuery;

    auto cq1 = makeCanonicalQuery(nss1);
    solnsPerQuery.insert({cq1.get(), makeCollScanPlan(nss1)});
    auto id1 = graph.addNode(nss1, std::move(cq1), boost::none);
    auto cq2 = makeCanonicalQuery(nss2);
    solnsPerQuery.insert({cq2.get(), makeCollScanPlan(nss2)});
    auto id2 = graph.addNode(nss1, std::move(cq2), FieldPath{"b"});

    std::vector<ResolvedPath> resolvedPaths{
        ResolvedPath{.nodeId = id1, .fieldName = FieldPath{"a"}},
        ResolvedPath{.nodeId = id2, .fieldName = FieldPath{"b"}},
    };

    graph.addSimpleEqualityEdge(id1, id2, 0 /*a*/, 1 /*b.b*/);

    auto soln = constructSolutionWithRandomOrder(std::move(solnsPerQuery), graph, resolvedPaths, 0);
    ASSERT(soln);

    // TODO: invoke proper graph serialization
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

    auto soln = constructSolutionWithRandomOrder(std::move(solnsPerQuery), graph, resolvedPaths, 0);
    auto soln2 =
        constructSolutionWithRandomOrder(std::move(solnsPerQueryCopy), graph, resolvedPaths, 1);
    ASSERT(soln);
    ASSERT(soln2);

    // TODO: invoke proper graph serialization
    goldenCtx.outStream() << "Graph:\nC -- A -- B" << std::endl;

    // Demonstrate that different join orders are constructed with different seeds
    goldenCtx.outStream() << "Solution with seed 0:" << std::endl;
    goldenCtx.outStream() << soln->toString() << std::endl;
    goldenCtx.outStream() << "Solution with seed 1:" << std::endl;
    goldenCtx.outStream() << soln2->toString() << std::endl;
}

}  // namespace mongo::join_ordering
