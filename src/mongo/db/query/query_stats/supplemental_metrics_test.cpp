// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/bsonobj.h"
#include "mongo/db/op_debug.h"
#include "mongo/db/query/query_stats/join_optimization_stats_entry.h"
#include "mongo/db/query/query_stats/optimizer_metrics_stats_entry.h"
#include "mongo/db/query/query_stats/supplemental_metrics_stats.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo::query_stats {
TEST(SupplementalMetricsStats, ClassicMetrics) {
    query_stats::SupplementalStatsMap metrics;
    auto entry = std::make_unique<query_stats::OptimizerMetricsClassicStatsEntry>(1);
    metrics.update(std::move(entry));
    BSONObj res1 = metrics.toBSON();
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "Classic": {
                "updateCount": 1,
                "optimizationTimeMicros": {
                    "sum": 1,
                    "max": 1,
                    "min": 1,
                    "sumOfSquares": {"$numberDecimal":"1.0000000000000000000000000000"}
                }
            }
        })",
        res1);
    auto entry2 = std::make_unique<query_stats::OptimizerMetricsClassicStatsEntry>(10);
    metrics.update(std::move(entry2));
    BSONObj res2 = metrics.toBSON();
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "Classic": {
                "updateCount": 2,
                "optimizationTimeMicros": {
                    "sum": 11,
                    "max": 10,
                    "min": 1,
                    "sumOfSquares": {"$numberDecimal":"101.0000000000000000000000000000"}
                }
            }
        })",
        res2);
}

TEST(SupplementalMetricsStats, SbeMetrics) {
    query_stats::SupplementalStatsMap metrics;
    auto entry = std::make_unique<query_stats::OptimizerMetricsClassicStatsEntry>(
        1, query_stats::SupplementalMetricType::SBE);
    metrics.update(std::move(entry));
    BSONObj res1 = metrics.toBSON();
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "SBE": {
                "updateCount": 1,
                "optimizationTimeMicros": {
                    "sum": 1,
                    "max": 1,
                    "min": 1,
                    "sumOfSquares": {"$numberDecimal":"1.0000000000000000000000000000"}
                }
            }
        })",
        res1);
}

TEST(SupplementalMetricsStats, BonsaiM2Metrics) {
    query_stats::SupplementalStatsMap metrics;
    auto entry = std::make_unique<query_stats::OptimizerMetricsBonsaiStatsEntry>(
        1, 10.5, 100.5, query_stats::SupplementalMetricType::BonsaiM2);
    metrics.update(std::move(entry));
    BSONObj res = metrics.toBSON();
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "BonsaiM2": {
                "updateCount": 1,
                "optimizationTimeMicros": {
                    "sum": 1,
                    "max": 1,
                    "min": 1,
                    "sumOfSquares": {"$numberDecimal":"1.0000000000000000000000000000"}
                },
                "estimatedCost": {
                    "sum": 10.5,
                    "max": 10.5,
                    "min": 10.5,
                    "sumOfSquares": {"$numberDecimal":"110.25000000000000000000000000"}
                },
                "estimatedCardinality": {
                    "sum": 100.5,
                    "max": 100.5,
                    "min": 100.5,
                    "sumOfSquares": {"$numberDecimal":"10100.250000000000000000000000"}
                }
            }
        })",
        res);
}

TEST(SupplementalMetricsStats, BonsaiM4Metrics) {
    query_stats::SupplementalStatsMap metrics;
    auto entry = std::make_unique<query_stats::OptimizerMetricsBonsaiStatsEntry>(
        1, 10, 101, query_stats::SupplementalMetricType::BonsaiM4);
    metrics.update(std::move(entry));
    BSONObj res = metrics.toBSON();
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "BonsaiM4": {
                "updateCount": 1,
                "optimizationTimeMicros": {
                    "sum": 1,
                    "max": 1,
                    "min": 1,
                    "sumOfSquares": {"$numberDecimal":"1.0000000000000000000000000000"}
                },
                "estimatedCost": {
                    "sum": 10,
                    "max": 10,
                    "min": 10,
                    "sumOfSquares": {"$numberDecimal":"100.00000000000000000000000000"}
                },
                "estimatedCardinality": {
                    "sum": 101,
                    "max": 101,
                    "min": 101,
                    "sumOfSquares": {"$numberDecimal":"10201.000000000000000000000000"}
                }
            }
        })",
        res);
}

TEST(SupplementalMetricsStats, JoinOptimizationMetrics) {
    query_stats::SupplementalStatsMap metrics;

    OpDebug::JoinOptimizationMetrics m1;
    m1.joinOptimizable = true;
    m1.numNamespaces = 5;
    m1.numLookupsInSuffix = 3;
    m1.numJoinGraphNodes = 4;
    m1.numSyntacticEdges = 3;
    m1.numInferredEdges = 1;
    m1.numSyntacticExprJoinPredicates = 1;
    m1.numSyntacticEqJoinPredicates = 2;
    m1.numInferredEqJoinPredicates = 1;
    m1.numInferredSingleTablePredicates = 1;
    metrics.update(std::make_unique<query_stats::JoinOptimizationStatsEntry>(m1));

    BSONObj res1 = metrics.toBSON();
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "JoinOptimization": {
                "updateCount": 1,
                "joinOptimizable": {"true": 1, "false": 0},
                "numNamespaces": {"sum": 5, "max": 5, "min": 5, "sumOfSquares": {"$numberDecimal":"25"}},
                "numLookupsInSuffix": {"sum": 3, "max": 3, "min": 3, "sumOfSquares": {"$numberDecimal":"9"}},
                "numJoinGraphNodes": {"sum": 4, "max": 4, "min": 4, "sumOfSquares": {"$numberDecimal":"16"}},
                "numSyntacticEdges": {"sum": 3, "max": 3, "min": 3, "sumOfSquares": {"$numberDecimal":"9"}},
                "numInferredEdges": {"sum": 1, "max": 1, "min": 1, "sumOfSquares": {"$numberDecimal":"1"}},
                "numSyntacticExprJoinPredicates": {"sum": 1, "max": 1, "min": 1, "sumOfSquares": {"$numberDecimal":"1"}},
                "numSyntacticEqJoinPredicates": {"sum": 2, "max": 2, "min": 2, "sumOfSquares": {"$numberDecimal":"4"}},
                "numInferredEqJoinPredicates": {"sum": 1, "max": 1, "min": 1, "sumOfSquares": {"$numberDecimal":"1"}},
                "numInferredSingleTablePredicates": {"sum": 1, "max": 1, "min": 1, "sumOfSquares": {"$numberDecimal":"1"}}
            }
        })",
        res1);

    // Aggregate a second data point into the same entry.
    OpDebug::JoinOptimizationMetrics m2;
    m2.joinOptimizable = false;
    m2.numNamespaces = 2;
    m2.numLookupsInSuffix = 1;
    m2.numJoinGraphNodes = 2;
    m2.numSyntacticEdges = 1;
    m2.numInferredEdges = 0;
    m2.numSyntacticExprJoinPredicates = 0;
    m2.numSyntacticEqJoinPredicates = 1;
    m2.numInferredEqJoinPredicates = 1;
    m2.numInferredSingleTablePredicates = 0;
    metrics.update(std::make_unique<query_stats::JoinOptimizationStatsEntry>(m2));

    BSONObj res2 = metrics.toBSON();
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "JoinOptimization": {
                "updateCount": 2,
                "joinOptimizable": {"true": 1, "false": 1},
                "numNamespaces": {"sum": 7, "max": 5, "min": 2, "sumOfSquares": {"$numberDecimal":"29"}},
                "numLookupsInSuffix": {"sum": 4, "max": 3, "min": 1, "sumOfSquares": {"$numberDecimal":"10"}},
                "numJoinGraphNodes": {"sum": 6, "max": 4, "min": 2, "sumOfSquares": {"$numberDecimal":"20"}},
                "numSyntacticEdges": {"sum": 4, "max": 3, "min": 1, "sumOfSquares": {"$numberDecimal":"10"}},
                "numInferredEdges": {"sum": 1, "max": 1, "min": 0, "sumOfSquares": {"$numberDecimal":"1"}},
                "numSyntacticExprJoinPredicates": {"sum": 1, "max": 1, "min": 0, "sumOfSquares": {"$numberDecimal":"1"}},
                "numSyntacticEqJoinPredicates": {"sum": 3, "max": 2, "min": 1, "sumOfSquares": {"$numberDecimal":"5"}},
                "numInferredEqJoinPredicates": {"sum": 2, "max": 1, "min": 1, "sumOfSquares": {"$numberDecimal":"2"}},
                "numInferredSingleTablePredicates": {"sum": 1, "max": 1, "min": 0, "sumOfSquares": {"$numberDecimal":"1"}}
            }
        })",
        res2);
}
}  // namespace mongo::query_stats
