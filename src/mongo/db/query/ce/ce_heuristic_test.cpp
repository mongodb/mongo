/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <string>

#include "mongo/db/query/ce/ce_test_utils.h"
#include "mongo/db/query/optimizer/cascades/ce_heuristic.h"
#include "mongo/db/query/optimizer/cascades/cost_derivation.h"
#include "mongo/db/query/optimizer/cascades/logical_props_derivation.h"
#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {
namespace {

using namespace optimizer;
using namespace optimizer::cascades;

constexpr double kCollCard = 10000.0;
const std::string collName = "test";

class HeuristicCETester : public CETester {
public:
    HeuristicCETester(
        std::string collName,
        const optimizer::OptPhaseManager::PhaseSet& optPhases = kDefaultCETestPhaseSet)
        : CETester(collName, kCollCard, optPhases) {}

protected:
    std::unique_ptr<CEInterface> getCETransport() const override {
        return std::make_unique<HeuristicCE>();
    }
};

TEST(CEHeuristicTest, CEWithoutOptimizationGtLtNum) {
    std::string query = "{a0 : {$gt : 14, $lt : 21}}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_MATCH_CE(ht, query, 1849.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationEqNum) {
    std::string query = "{a: 123}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 2.03205, 3.0);
    ASSERT_MATCH_CE_CARD(ht, query, 3.34575, 7.0);
    ASSERT_MATCH_CE_CARD(ht, query, 4.16228, 10.0);
    ASSERT_MATCH_CE_CARD(ht, query, 20.0, 100.0);
    ASSERT_MATCH_CE_CARD(ht, query, 1100.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationEqStr) {
    std::string query = "{a: 'foo'}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 2.03205, 3.0);
    ASSERT_MATCH_CE_CARD(ht, query, 3.34575, 7.0);
    ASSERT_MATCH_CE_CARD(ht, query, 4.16228, 10.0);
    ASSERT_MATCH_CE_CARD(ht, query, 20.0, 100.0);
    ASSERT_MATCH_CE_CARD(ht, query, 1100.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationGtNum) {
    std::string query = "{a: {$gt: 44}}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 7.2, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 54.45, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 430.0, 1000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationGtStr) {
    std::string query = "{a: {$gt: 'foo'}}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 7.2, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 54.45, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 430.0, 1000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationLtNum) {
    std::string query = "{a: {$lt: 44}}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 7.2, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 54.45, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 430.0, 1000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationDNF1pathSimple) {
    std::string query =
        "{$or: ["
        "{$and: [{a0: {$gt: 9}}, {a0: {$lt: 12}}]},"
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 44}}]}"
        "]}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_MATCH_CE_CARD(ht, query, 7.8336, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 50.8359, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 335.612, 1000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationNestedConjAndDisj1) {
    std::string query =
        "{$or: ["
        "{a: {$lt: 3}},"
        "{$and: [{b: {$gt:5}}, {c: {$lt: 10}}]}"
        "]}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 8.352, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 67.9264, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 535.393, 1000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationNestedConjAndDisj2) {
    std::string query =
        "{$and: ["
        "{a: {$lt: 3}},"
        "{$or: [{b: {$gt:5}}, {b: {$lt: 10}}]}"
        "]}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 6.912, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 43.4239, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 290.293, 1000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationNestedConjAndDisj3) {
    std::string query =
        "{$and: ["
        "{$and: [{a: {$gt: 5}}, {a: {$lt: 10}}]},"
        "{$and: ["
        "   {b: {$gt: 15}},"
        "   {c: {$lt: 110}},"
        "   {$or: [{a1: 1}, {b1: 2}, {c1: 3}]}"
        "]}"
        "]}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 3.01561, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 9.37173, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 19.3064, 1000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationNestedConjAndDisj4) {
    std::string query =
        "{$or: ["
        "{$or: [{a: {$gt: 5}}, {a: {$lt: 10}}]},"
        "{$or: ["
        "   {b: {$gt: 15}},"
        "   {c: {$lt: 110}},"
        "   {$and: [{a1: 1}, {b1: 2}, {c1: 3}]}"
        "]}"
        "]}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 8.98677, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 94.9731, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 894.681, 1000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationTraverseSelectivityDoesNotAccumulate) {
    std::string query =
        "{$or: ["
        "{a0: 1},"
        "{a0: {$lt: -4}},"
        "{b0: {$gt: 10}}"
        "]}";
    std::string queryWithLongPaths =
        "{$or: ["
        "{'a0.a1.a2.a3.a4.a5.a6.a7.a8.a9': 1},"
        "{'a0.a1.a2.a3.a4.a5.a6.a7.a8.a9': {$lt: -4}},"
        "{'b0.b1.b3': {$gt: 10}}"
        "]}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    auto ce1 = ht.getMatchCE<optimizer::RootNode>(query);
    auto ce2 = ht.getMatchCE<optimizer::RootNode>(queryWithLongPaths);
    ASSERT_APPROX_EQUAL(ce1, ce2, kMaxCEError);
}

TEST(CEHeuristicTest, CEWithoutOptimizationIntervalWithEqOnSameValue) {
    std::string query =
        "{$or: ["
        "{a: 1},"
        "{$and: [{a: 2}, {a: 2}]}"
        "]}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 5.6, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 27.8048, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 159.083, 1000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationIntervalWithEqOnDifferentValues) {
    std::string query =
        "{$or: ["
        "{a: 1},"
        "{$and: [{a: 2}, {a: 3}]}"
        "]}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 3.9, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 19.8499, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 131.623, 1000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationConjunctionWithIn) {
    std::string query =
        "{$or: ["
        "{a: 1},"
        "{$and: [{a: 2}, {a: {$in: [2, 3, 4]}}]}"
        "]}";
    HeuristicCETester ht(collName, kNoOptPhaseSet);
    // Estimation for $in is not implemented yet, so we assume it has the default filter selectivity
    // of 0.1.
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 4.41, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 27.7649, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 218.46, 1000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationOneLowBoundWithoutTraverse) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("test", "test");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a", make<PathCompare>(Operations::Gt, Constant::int64(42))),
                         make<Variable>("test")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"test"}}, std::move(filterNode));

    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_CE_CARD(ht, rootNode, 0.0, 0.0);
    ASSERT_CE_CARD(ht, rootNode, 2.1, 3.0);
    ASSERT_CE_CARD(ht, rootNode, 4.9, 7.0);
    ASSERT_CE_CARD(ht, rootNode, 7.0, 10.0);
    ASSERT_CE_CARD(ht, rootNode, 33.0, 100.0);
    ASSERT_CE_CARD(ht, rootNode, 3300.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationOneHighBoundWithoutTraverse) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("test", "test");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a", make<PathCompare>(Operations::Lt, Constant::int64(42))),
                         make<Variable>("test")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"test"}}, std::move(filterNode));

    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_CE_CARD(ht, rootNode, 0.0, 0.0);
    ASSERT_CE_CARD(ht, rootNode, 2.1, 3.0);
    ASSERT_CE_CARD(ht, rootNode, 4.9, 7.0);
    ASSERT_CE_CARD(ht, rootNode, 7.0, 10.0);
    ASSERT_CE_CARD(ht, rootNode, 33.0, 100.0);
    ASSERT_CE_CARD(ht, rootNode, 3300.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationTwoLowBoundsWithoutTraverse) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("test", "test");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathComposeM>(
                                           make<PathCompare>(Operations::Gt, Constant::int64(5)),
                                           make<PathCompare>(Operations::Gt, Constant::int64(10)))),
                         make<Variable>("test")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"test"}}, std::move(filterNode));

    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_CE_CARD(ht, rootNode, 0.0, 0.0);
    ASSERT_CE_CARD(ht, rootNode, 2.1, 3.0);
    ASSERT_CE_CARD(ht, rootNode, 4.9, 7.0);
    ASSERT_CE_CARD(ht, rootNode, 7.0, 10.0);
    ASSERT_CE_CARD(ht, rootNode, 33.0, 100.0);
    ASSERT_CE_CARD(ht, rootNode, 3300.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationTwoHighBoundsWithoutTraverse) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("test", "test");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathComposeM>(
                                           make<PathCompare>(Operations::Lt, Constant::int64(5)),
                                           make<PathCompare>(Operations::Lt, Constant::int64(10)))),
                         make<Variable>("test")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"test"}}, std::move(filterNode));

    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_CE_CARD(ht, rootNode, 0.0, 0.0);
    ASSERT_CE_CARD(ht, rootNode, 2.1, 3.0);
    ASSERT_CE_CARD(ht, rootNode, 4.9, 7.0);
    ASSERT_CE_CARD(ht, rootNode, 7.0, 10.0);
    ASSERT_CE_CARD(ht, rootNode, 33.0, 100.0);
    ASSERT_CE_CARD(ht, rootNode, 3300.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationClosedRangeWithoutTraverse) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("test", "test");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathComposeM>(
                                           make<PathCompare>(Operations::Gt, Constant::int64(7)),
                                           make<PathCompare>(Operations::Lt, Constant::int64(13)))),
                         make<Variable>("test")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"test"}}, std::move(filterNode));

    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_CE_CARD(ht, rootNode, 0.0, 0.0);
    ASSERT_CE_CARD(ht, rootNode, 1.5, 3.0);
    ASSERT_CE_CARD(ht, rootNode, 3.5, 7.0);
    ASSERT_CE_CARD(ht, rootNode, 5.0, 10.0);
    ASSERT_CE_CARD(ht, rootNode, 20.0, 100.0);
    ASSERT_CE_CARD(ht, rootNode, 2000.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationIntervalWithDifferentTypes) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("test", "test");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>(
                "a",
                make<PathComposeM>(make<PathCompare>(Operations::Gt, Constant::int64(5)),
                                   make<PathCompare>(Operations::Lt, Constant::str("foo")))),
            make<Variable>("test")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"test"}}, std::move(filterNode));

    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_CE_CARD(ht, rootNode, 0.0, 0.0);
    ASSERT_CE_CARD(ht, rootNode, 2.1, 3.0);
    ASSERT_CE_CARD(ht, rootNode, 4.9, 7.0);
    ASSERT_CE_CARD(ht, rootNode, 7.0, 10.0);
    ASSERT_CE_CARD(ht, rootNode, 33.0, 100.0);
    ASSERT_CE_CARD(ht, rootNode, 3300.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationClosedRangeWithPathExpr) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("test", "test");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathComposeM>(
                make<PathGet>(
                    "a0",
                    make<PathTraverse>(
                        make<PathGet>("a1",
                                      make<PathTraverse>(
                                          make<PathCompare>(Operations::Gt, Constant::int64(5)),
                                          PathTraverse::kSingleLevel)),
                        PathTraverse::kSingleLevel)),
                make<PathGet>(
                    "a0",
                    make<PathTraverse>(
                        make<PathGet>("a1",
                                      make<PathTraverse>(
                                          make<PathCompare>(Operations::Lt, Constant::int64(10)),
                                          PathTraverse::kSingleLevel)),
                        PathTraverse::kSingleLevel))),
            make<Variable>("test")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"test"}}, std::move(filterNode));

    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_CE_CARD(ht, rootNode, 0.0, 0.0);
    ASSERT_CE_CARD(ht, rootNode, 1.5, 3.0);
    ASSERT_CE_CARD(ht, rootNode, 3.5, 7.0);
    ASSERT_CE_CARD(ht, rootNode, 5.0, 10.0);
    ASSERT_CE_CARD(ht, rootNode, 20.0, 100.0);
    ASSERT_CE_CARD(ht, rootNode, 2000.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationClosedRangeWith1Variable) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("test", "test");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathComposeM>(
                make<PathGet>(
                    "a0",
                    make<PathTraverse>(
                        make<PathGet>("a1",
                                      make<PathTraverse>(
                                          make<PathCompare>(Operations::Gt, Constant::int64(5)),
                                          PathTraverse::kSingleLevel)),
                        PathTraverse::kSingleLevel)),
                make<PathGet>(
                    "a0",
                    make<PathTraverse>(
                        make<PathGet>("a1",
                                      make<PathTraverse>(
                                          make<PathCompare>(Operations::Lt, make<Variable>("test")),
                                          PathTraverse::kSingleLevel)),
                        PathTraverse::kSingleLevel))),
            make<Variable>("test")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"test"}}, std::move(filterNode));

    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_CE_CARD(ht, rootNode, 0.0, 0.0);
    ASSERT_CE_CARD(ht, rootNode, 1.5, 3.0);
    ASSERT_CE_CARD(ht, rootNode, 3.5, 7.0);
    ASSERT_CE_CARD(ht, rootNode, 5.0, 10.0);
    ASSERT_CE_CARD(ht, rootNode, 20.0, 100.0);
    ASSERT_CE_CARD(ht, rootNode, 2000.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationOpenRangeWith1Variable) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("test", "test");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathComposeM>(
                make<PathGet>(
                    "a0",
                    make<PathTraverse>(
                        make<PathGet>("a1",
                                      make<PathTraverse>(
                                          make<PathCompare>(Operations::Lt, Constant::int64(5)),
                                          PathTraverse::kSingleLevel)),
                        PathTraverse::kSingleLevel)),
                make<PathGet>(
                    "a0",
                    make<PathTraverse>(
                        make<PathGet>("a1",
                                      make<PathTraverse>(
                                          make<PathCompare>(Operations::Lt, make<Variable>("test")),
                                          PathTraverse::kSingleLevel)),
                        PathTraverse::kSingleLevel))),
            make<Variable>("test")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"test"}}, std::move(filterNode));

    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_CE_CARD(ht, rootNode, 0.0, 0.0);
    ASSERT_CE_CARD(ht, rootNode, 2.1, 3.0);
    ASSERT_CE_CARD(ht, rootNode, 4.9, 7.0);
    ASSERT_CE_CARD(ht, rootNode, 7.0, 10.0);
    ASSERT_CE_CARD(ht, rootNode, 33.0, 100.0);
    ASSERT_CE_CARD(ht, rootNode, 3300.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationConjunctionOfBoundsWithDifferentPaths) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("test", "test");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathComposeM>(
                make<PathGet>(
                    "a0",
                    make<PathTraverse>(
                        make<PathGet>("a1",
                                      make<PathTraverse>(
                                          make<PathCompare>(Operations::Gt, Constant::int64(5)),
                                          PathTraverse::kSingleLevel)),
                        PathTraverse::kSingleLevel)),
                make<PathGet>(
                    "b0",
                    make<PathTraverse>(
                        make<PathGet>("b1",
                                      make<PathTraverse>(
                                          make<PathCompare>(Operations::Lt, Constant::int64(10)),
                                          PathTraverse::kSingleLevel)),
                        PathTraverse::kSingleLevel))),
            make<Variable>("test")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"test"}}, std::move(filterNode));

    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_CE_CARD(ht, rootNode, 0.0, 0.0);
    ASSERT_CE_CARD(ht, rootNode, 1.92, 3.0);
    ASSERT_CE_CARD(ht, rootNode, 4.48, 7.0);
    ASSERT_CE_CARD(ht, rootNode, 6.4, 10.0);
    ASSERT_CE_CARD(ht, rootNode, 18.49, 100.0);
    ASSERT_CE_CARD(ht, rootNode, 1849.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationDisjunctionOnSamePathWithoutTraverse) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("test", "test");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathComposeA>(
                make<PathGet>(
                    "a0",
                    make<PathGet>("a1", make<PathCompare>(Operations::Gt, Constant::int64(5)))),
                make<PathGet>(
                    "a0",
                    make<PathGet>("a1", make<PathCompare>(Operations::Eq, Constant::int64(100))))),
            make<Variable>("test")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"test"}}, std::move(filterNode));

    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_CE_CARD(ht, rootNode, 0.0, 0.0);
    ASSERT_CE_CARD(ht, rootNode, 2.61962, 3.0);
    ASSERT_CE_CARD(ht, rootNode, 5.69373, 7.0);
    ASSERT_CE_CARD(ht, rootNode, 7.94868, 10.0);
    ASSERT_CE_CARD(ht, rootNode, 39.7, 100.0);
    ASSERT_CE_CARD(ht, rootNode, 3367.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationDisjunctionOnDifferentPathsWithoutTraverse) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("test", "test");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathComposeA>(
                make<PathGet>(
                    "a0",
                    make<PathGet>("a1", make<PathCompare>(Operations::Gt, Constant::int64(5)))),
                make<PathGet>(
                    "b0",
                    make<PathGet>("b1", make<PathCompare>(Operations::Eq, Constant::int64(100))))),
            make<Variable>("test")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"test"}}, std::move(filterNode));

    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ASSERT_CE_CARD(ht, rootNode, 0.0, 0.0);
    ASSERT_CE_CARD(ht, rootNode, 2.61962, 3.0);
    ASSERT_CE_CARD(ht, rootNode, 5.69373, 7.0);
    ASSERT_CE_CARD(ht, rootNode, 7.94868, 10.0);
    ASSERT_CE_CARD(ht, rootNode, 39.7, 100.0);
    ASSERT_CE_CARD(ht, rootNode, 3367.0, 10000.0);
}

TEST(CEHeuristicTest, CEWithoutOptimizationEquivalentConjunctions) {
    using namespace properties;

    ABT rootNode1 = make<RootNode>(
        ProjectionRequirement{ProjectionNameVector{"test"}},
        make<FilterNode>(
            make<EvalFilter>(
                make<PathComposeM>(
                    make<PathGet>(
                        "a0",
                        make<PathTraverse>(make<PathCompare>(Operations::Gt, Constant::int64(5)),
                                           PathTraverse::kSingleLevel)),
                    make<PathGet>(
                        "b0",
                        make<PathTraverse>(make<PathCompare>(Operations::Gt, Constant::int64(10)),
                                           PathTraverse::kSingleLevel))),
                make<Variable>("test")),
            make<ScanNode>("test", "test")));

    ABT rootNode2 = make<RootNode>(
        ProjectionRequirement{ProjectionNameVector{"test"}},
        make<FilterNode>(
            make<EvalFilter>(make<PathGet>("a0",
                                           make<PathTraverse>(make<PathCompare>(Operations::Gt,
                                                                                Constant::int64(5)),
                                                              PathTraverse::kSingleLevel)),
                             make<Variable>("test")),
            make<FilterNode>(
                make<EvalFilter>(
                    make<PathGet>(
                        "b0",
                        make<PathTraverse>(make<PathCompare>(Operations::Gt, Constant::int64(10)),
                                           PathTraverse::kSingleLevel)),
                    make<Variable>("test")),
                make<ScanNode>("test", "test"))));

    HeuristicCETester ht(collName, kNoOptPhaseSet);
    ht.setCollCard(kCollCard);
    auto ce1 = ht.getCE<optimizer::RootNode>(rootNode1);
    auto ce2 = ht.getCE<optimizer::RootNode>(rootNode2);
    ASSERT_APPROX_EQUAL(ce1, ce2, kMaxCEError);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_Eq) {
    std::string query = "{a : 123}";
    HeuristicCETester ht(collName);
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 0.1, 0.1);
    ASSERT_MATCH_CE_CARD(ht, query, 1.73205, 3.0);
    ASSERT_MATCH_CE_CARD(ht, query, 2.64575, 7.0);
    ASSERT_MATCH_CE_CARD(ht, query, 3.16228, 10.0);
    ASSERT_MATCH_CE_CARD(ht, query, 10.0, 100.0);
    ASSERT_MATCH_CE_CARD(ht, query, 100.0, 10000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_Gt) {
    std::string query = "{a: {$gt: 44}}";
    HeuristicCETester ht(collName);
    ASSERT_MATCH_CE_CARD(ht, query, 0.01, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 0.7, 1.0);
    ASSERT_MATCH_CE_CARD(ht, query, 6.3, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 44.55, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 330, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_Gt_Lt) {
    std::string query = "{a: {$gt: 44, $lt: 99}}";
    HeuristicCETester ht(collName);
    ASSERT_MATCH_CE_CARD(ht, query, 0.585662, 1.0);
    ASSERT_MATCH_CE_CARD(ht, query, 5.27096, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 29.885, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 189.571, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_AND2Eq) {
    std::string query = "{a : 13, b : 42}";
    HeuristicCETester ht(collName);
    ASSERT_MATCH_CE_CARD(ht, query, 1.31607, 3.0);
    ASSERT_MATCH_CE_CARD(ht, query, 1.62658, 7.0);
    ASSERT_MATCH_CE_CARD(ht, query, 1.77828, 10.0);
    ASSERT_MATCH_CE_CARD(ht, query, 3.16228, 100.0);
    ASSERT_MATCH_CE_CARD(ht, query, 10.0, 10000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_AND3Eq) {
    std::string query = "{a : 13, b : 42, c : 69}";
    HeuristicCETester ht(collName);
    ASSERT_MATCH_CE_CARD(ht, query, 1.1472, 3.0);
    ASSERT_MATCH_CE_CARD(ht, query, 1.27537, 7.0);
    ASSERT_MATCH_CE_CARD(ht, query, 1.33352, 10.0);
    ASSERT_MATCH_CE_CARD(ht, query, 1.77828, 100.0);
    ASSERT_MATCH_CE_CARD(ht, query, 3.16228, 10000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_OR1path) {
    std::string query = "{$or: [{a0: {$gt: 44}}, {a0: {$lt: 9}}]}";
    HeuristicCETester ht(collName);
    ASSERT_MATCH_CE_CARD(ht, query, 7.52115, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 58.6188, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 451.581, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_OR2paths) {
    std::string query = "{$or: [{a0: {$gt:44}}, {b0: {$lt: 9}}]}";
    HeuristicCETester ht(collName, kOnlySubPhaseSet);
    // Disjunctions on different paths are not SARGable.
    ASSERT_MATCH_CE_CARD(ht, query, 8.64, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 78.9525, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 675.1, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_DNF1pathSimple) {
    std::string query =
        "{$or: ["
        "{$and: [{a0: {$gt: 9}}, {a0: {$lt: 12}}]},"
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 44}}]}"
        "]}";
    HeuristicCETester ht(collName);
    ASSERT_MATCH_CE_CARD(ht, query, 6.42792, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 37.0586, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 225.232, 1000.0);
}


TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_DNF1pathComplex) {
    HeuristicCETester ht(collName, kOnlySubPhaseSet);
    // Each disjunct has different number of conjuncts,
    // so that its selectivity is different. We need 5 disjuncts to test exponential backoff which
    // cuts off at the first 4. The conjuncts are in selectivity order.
    std::string query1 =
        "{$or: ["
        "{$and: [{a0: {$gt: 9}}, {a0: {$lt: 12}}]},"
        "{$and: [{a0: {$gt: 9}}, {a0: {$lt: 12}}, {a0: {$gt: 42}}]},"
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 99}}, {a0: {$gt: 42}}, {a0: {$lt: 88}}]},"
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 99}}, {a0: {$gt: 42}}, {a0: {$lt: 88}}, {a0: {$lt: "
        "81}}]},"
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 99}}, {a0: {$gt: 42}}, {a0: {$lt: 88}}, {a0: {$lt: "
        "81}}, {a0: {$lt: 77}}]}"
        "]}";
    auto ce1 = ht.getMatchCE<optimizer::RootNode>(query1);
    // The conjuncts are in inverse selectivity order.
    std::string query2 =
        "{$or: ["
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 99}}, {a0: {$gt: 42}}, {a0: {$lt: 88}}, {a0: {$lt: "
        "81}}, {a0: {$lt: 77}}]},"
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 99}}, {a0: {$gt: 42}}, {a0: {$lt: 88}}, {a0: {$lt: "
        "81}}]},"
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 99}}, {a0: {$gt: 42}}, {a0: {$lt: 88}}]},"
        "{$and: [{a0: {$gt: 9}}, {a0: {$lt: 12}}, {a0: {$gt: 42}}]},"
        "{$and: [{a0: {$gt: 9}}, {a0: {$lt: 12}}]}"
        "]}";
    auto ce2 = ht.getMatchCE<optimizer::RootNode>(query2);
    ASSERT_APPROX_EQUAL(ce1, ce2, kMaxCEError);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_DNF2paths) {
    std::string query =
        "{$or: ["
        "{$and: [{a0: {$gt: 9}}, {a0: {$lt: 12}}]},"
        "{$and: [{b0: {$gt:40}}, {b0: {$lt: 44}}]}"
        "]}";
    HeuristicCETester ht(collName, kOnlySubPhaseSet);
    // Disjunctions on different paths are not SARGable.
    ASSERT_MATCH_CE_CARD(ht, query, 7.8336, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 50.8359, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 335.612, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_CNF1path) {
    std::string query =
        "{$and : ["
        "{$or : [ {a0 : {$gt : 11}}, {a0 : {$lt : 44}} ]},"
        "{$or : [ {a0 : {$gt : 77}}, {a0 : {$eq : 51}} ]}"
        "]}";
    HeuristicCETester ht(collName);
    ASSERT_MATCH_CE_CARD(ht, query, 6.21212, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 36.4418, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 228.935, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_CNF2paths) {
    std::string query =
        "{$and : ["
        "{$or : [ {a0 : {$gt : 11}}, {a0 : {$lt : 44}} ]},"
        "{$or : [ {b0 : {$gt : 77}}, {b0 : {$eq : 51}} ]}"
        "]}";
    HeuristicCETester ht(collName);
    ASSERT_MATCH_CE_CARD(ht, query, 6.21212, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 36.4418, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 228.935, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionExplorationPhases) {
    HeuristicCETester ht(collName);
    ASSERT_MATCH_CE(ht, "{a : 13, b : 42}", 10.0);
}

TEST(CEHeuristicTest, CENotEquality) {
    double collCard = kCollCard;
    HeuristicCETester opt(collName);

    // We avoid optimizing in order to verify heuristic estimate of FilterNode subtree. Note that we
    // do not generate SargableNodes for $not predicates, but we do generate SargableNodes without
    // it; for the purposes of this test, we want to demonstrate that $not returns the inverse of
    // the FilterNode estimate.
    HeuristicCETester noOpt(collName, kNoOptPhaseSet);

    // Equality selectivity is sqrt(kCollCard)/kCollCard = 0.01. We then add 0.1 when we see a
    // Traverse, obtaining a selectivity of 0.11. When we see a UnaryOp [Not] above this subtree, we
    // invert the selectivity 1.0 - 0.11 = 0.89.
    double ce = 1100.0;
    double inverseCE = collCard - ce;
    ASSERT_MATCH_CE(noOpt, "{a: {$eq: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$eq: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{'validate.long.path.estimate': {$eq: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{'validate.long.path.estimate': {$not: {$eq: 1}}}", inverseCE);

    // Update cardinality to 25.
    collCard = 25;
    opt.setCollCard(collCard);
    noOpt.setCollCard(collCard);

    // Selectivity is sqrt(25)/25 + 0.1 for traverse.
    ce = 7.5;
    inverseCE = collCard - ce;
    ASSERT_MATCH_CE(noOpt, "{a: {$eq: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$eq: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{'validate.long.path.estimate': {$eq: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{'validate.long.path.estimate': {$not: {$eq: 1}}}", inverseCE);

    // Update cardinality to 9.
    collCard = 9;
    opt.setCollCard(collCard);
    noOpt.setCollCard(collCard);

    // Selectivity is sqrt(3)/9 + 0.1 for traverse.
    ce = 3.90;
    inverseCE = collCard - ce;
    ASSERT_MATCH_CE(noOpt, "{a: {$eq: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$eq: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{'validate.long.path.estimate': {$eq: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{'validate.long.path.estimate': {$not: {$eq: 1}}}", inverseCE);
}

TEST(CEHeuristicTest, CENotOpenRange) {
    // Repeat the above test for open ranges; the $not cardinality estimate should add up with the
    // non-$not estimate to the collection cardinality.
    double collCard = kCollCard;
    HeuristicCETester opt(collName);
    HeuristicCETester noOpt(collName, kNoOptPhaseSet);

    // Expect open-range selectivity for input card > 100 (0.33 + 0.10 for traverse).
    double ce = 4300;
    double inverseCE = collCard - ce;

    ASSERT_MATCH_CE(noOpt, "{a: {$lt: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$lt: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$lte: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$lte: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gt: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gt: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gte: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gte: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{'validate.long.path.estimate': {$gte: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{'validate.long.path.estimate': {$not: {$gte: 1}}}", inverseCE);

    // Update cardinality to 25.
    collCard = 25;
    opt.setCollCard(collCard);
    noOpt.setCollCard(collCard);

    // Expect open-range selectivity for input card in range (20, 100) (0.45 + 0.10 for traverse).
    ce = 13.75;
    inverseCE = collCard - ce;

    ASSERT_MATCH_CE(noOpt, "{a: {$lt: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$lt: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$lte: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$lte: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gt: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gt: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gte: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gte: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{'validate.long.path.estimate': {$gte: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{'validate.long.path.estimate': {$not: {$gte: 1}}}", inverseCE);

    // Update cardinality to 10.
    collCard = 10.0;
    opt.setCollCard(collCard);
    noOpt.setCollCard(collCard);

    // Expect open-range selectivity for input card < 20 (0.70 + 0.10 for traverse).
    ce = 8.0;
    inverseCE = collCard - ce;

    ASSERT_MATCH_CE(noOpt, "{a: {$lt: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$lt: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$lte: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$lte: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gt: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gt: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gte: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gte: 1}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{'validate.long.path.estimate': {$gte: 1}}", ce);
    ASSERT_MATCH_CE(opt, "{'validate.long.path.estimate': {$not: {$gte: 1}}}", inverseCE);
}

TEST(CEHeuristicTest, CENotClosedRange) {
    // Repeat the above test for closed ranges; the $not cardinality estimate should add up with the
    // non-$not estimate to the collection cardinality.
    double collCard = kCollCard;
    double ce = 1849.0;
    double inverseCE = collCard - ce;
    HeuristicCETester opt(collName);
    HeuristicCETester noOpt(collName, kNoOptPhaseSet);

    ASSERT_MATCH_CE(noOpt, "{a: {$gt: 10, $lt: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gt: 10, $lt: 20}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gte: 10, $lt: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gte: 10, $lt: 20}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gte: 10, $lte: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gte: 10, $lte: 20}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gt: 10, $lte: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gt: 10, $lte: 20}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{'validate.long.path.estimate': {$gte: 10, $lt: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{'validate.long.path.estimate': {$not: {$gte: 10, $lt: 20}}}", inverseCE);

    /*
     * Update cardinality to 25. Here we observe an interesting edge case where the estimated
     * cardinality is not the inverse of the actual cardinality.
     *
     * Consider the predicate {a: {$gt: 10, $lt: 20}}. This generates two FilterNodes stacked on top
     * of each other. However, the predicate {a: {$not: {$gt: 10, $lt: 20}}} generates just one
     * FilterNode.
     *
     * We always use input cardinality to determine which interval selectivity we're going to use.
     * However, we have a different input cardinality for the one FilterNode case (collCard) than
     * for the two FilterNodes case: the first node gets collCard, and the second node gets a
     * smaller value after the selectivity of the first filter is applied.
     *
     * Because we use a piecewise function to pick the selectivity, and because we go from inputCard
     * < 100 to inputCard < 20, we choose different selectivities for the intervals in the second
     * FilterNode (0.50) than in the first (0.33).
     */
    collCard = 25;
    ce = 11.0;
    inverseCE = 17.4375;
    opt.setCollCard(collCard);
    noOpt.setCollCard(collCard);

    ASSERT_MATCH_CE(noOpt, "{a: {$gt: 10, $lt: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gt: 10, $lt: 20}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gte: 10, $lt: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gte: 10, $lt: 20}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gte: 10, $lte: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gte: 10, $lte: 20}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gt: 10, $lte: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gt: 10, $lte: 20}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{'validate.long.path.estimate': {$gte: 10, $lt: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{'validate.long.path.estimate': {$not: {$gte: 10, $lt: 20}}}", inverseCE);

    // Update cardinality to 10.
    collCard = 10.0;
    ce = 6.4;
    inverseCE = collCard - ce;
    opt.setCollCard(collCard);
    noOpt.setCollCard(collCard);

    ASSERT_MATCH_CE(noOpt, "{a: {$gt: 10, $lt: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gt: 10, $lt: 20}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gte: 10, $lt: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gte: 10, $lt: 20}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gte: 10, $lte: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gte: 10, $lte: 20}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{a: {$gt: 10, $lte: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{a: {$not: {$gt: 10, $lte: 20}}}", inverseCE);
    ASSERT_MATCH_CE(noOpt, "{'validate.long.path.estimate': {$gte: 10, $lt: 20}}", ce);
    ASSERT_MATCH_CE(opt, "{'validate.long.path.estimate': {$not: {$gte: 10, $lt: 20}}}", inverseCE);
}

}  // namespace
}  // namespace mongo::ce
