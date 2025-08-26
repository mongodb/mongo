/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/parsed_distinct_command.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
class QueryPlannerDistinctTest : public QueryPlannerTest {
public:
    void setUp() final {
        QueryPlannerTest::setUp();
        params.mainCollectionInfo.options = QueryPlannerParams::DEFAULT;
    }

    void runDistinctQuery(const std::string& distinctKey,
                          const BSONObj& filter = BSONObj(),
                          const BSONObj& sort = BSONObj(),
                          const BSONObj& proj = BSONObj(),
                          const bool flipDistinctScanDirection = false) {
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(filter);
        findCommand->setSort(sort);
        findCommand->setProjection(proj);

        cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
            .parsedFind =
                ParsedFindCommandParams{.findCommand = std::move(findCommand),
                                        .allowedFeatures =
                                            MatchExpressionParser::kAllowAllSpecialFeatures},
            .pipeline = {},
            .isCountLike = isCountLike});
        cq->setDistinct(
            CanonicalDistinct(distinctKey,
                              false,
                              boost::none,
                              // In order to replicate what distinct() does, we set up our
                              // projection here for potential use in an optimization.
                              parsed_distinct_command::getDistinctProjection(distinctKey),
                              flipDistinctScanDirection));

        auto statusWithMultiPlanSolns = QueryPlanner::plan(*cq, params);
        if (statusWithMultiPlanSolns.getStatus().code() ==
            ErrorCodes::NoDistinctScansForDistinctEligibleQuery) {
            cq->resetDistinct();
            statusWithMultiPlanSolns = QueryPlanner::plan(*cq, params);
        }

        ASSERT_OK(statusWithMultiPlanSolns.getStatus());
        solns = std::move(statusWithMultiPlanSolns.getValue());
    }

    void assertCandidateExists(const std::string& candidate) {
        ASSERT_EQ(1, numSolutionMatches(candidate));
    }
};

namespace {

/**
 * A query solution that contains a FETCH with a filter is not eligible for a DISTINCT_SCAN.
 */
TEST_F(QueryPlannerDistinctTest, PredicateNotCovered) {
    RAIIServerParameterControllerForTest shardFiltering("featureFlagShardFilteringDistinctScan",
                                                        true);
    addIndex(fromjson("{x: 1}"));
    addIndex(fromjson("{y: 1}"));
    addIndex(fromjson("{z: 1}"));
    addIndex(fromjson("{x: 1, y: 1}"));
    addIndex(fromjson("{y: 1, z: 1}"));
    runDistinctQuery("x", fromjson("{x: {$gt: 3}, y: 2, z: {$lt: 5}}"));

    // There is no index that covers the entire predicate and distinct scans do not currently
    // support residual filters. Therefore, the IXSCAN candidates will not be transformed.
    assertNumSolutions(5);
    assertCandidateExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
    assertCandidateExists("{fetch: {node: {ixscan: {pattern: {y: 1}}}}}");
    assertCandidateExists("{fetch: {node: {ixscan: {pattern: {z: 1}}}}}");
    assertCandidateExists("{fetch: {node: {ixscan: {pattern: {x: 1, y: 1}}}}}");
    assertCandidateExists("{fetch: {node: {ixscan: {pattern: {y: 1, z: 1}}}}}");
};

/**
 * We cannot have a filter on top of a DISTINCT_SCAN, but when a predicate can be converted to index
 * bounds (namely, covered by a DISTINCT_SCAN), it is eligible for conversion.
 */
TEST_F(QueryPlannerDistinctTest, PredicateCovered) {
    RAIIServerParameterControllerForTest shardFiltering("featureFlagShardFilteringDistinctScan",
                                                        true);
    addIndex(fromjson("{x: 1}"));
    addIndex(fromjson("{x: 1, y: 1}"));
    addIndex(fromjson("{y: 1, z: 1}"));
    runDistinctQuery("x", fromjson("{y: 2, x: {$gt: 2}}"));

    assertNumSolutions(3);
    // Index {x: 1, y: 1} is transformed since it covers the filter.
    assertCandidateExists(
        "{proj: {spec: {_id: 0, x: 1}, node: {distinct: {key: 'x', indexPattern: {x: 1, y: 1}}}}}");
    assertCandidateExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
    assertCandidateExists("{fetch: {node: {ixscan: {pattern: {y: 1, z: 1}}}}}");
}

/**
 * The distinct transition can be done just when the leaf node is an IXSCAN. In case of a sort, we
 * can avoid a COLLSCAN + SORT, and have just an IXSCAN in case that there is an index that covers
 * the sort pattern.
 */
TEST_F(QueryPlannerDistinctTest, SortCovered) {
    RAIIServerParameterControllerForTest shardFiltering("featureFlagShardFilteringDistinctScan",
                                                        true);
    addIndex(fromjson("{x: 1}"));
    addIndex(fromjson("{x: 1, y: 1}"));
    addIndex(fromjson("{y: 1, z: 1}"));
    runDistinctQuery("y", BSONObj(), fromjson("{x: 1}"));

    assertNumSolutions(2);
    // Index {x: 1, y: 1} is transformed since it covers the sort.
    assertCandidateExists(
        "{proj: {spec: {_id: 0, y: 1}, node: {distinct: {key: 'y', indexPattern: {x: 1, y: 1}}}}}");
    assertCandidateExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
}

/**
 * When the 'strictDistinctOnly` argument is enabled, DISTINCT_SCAN can be done just on an
 * underlying IXSCAN that contains the distinct key as the first field, or any other previous fields
 * to have single-point bounds.
 */
TEST_F(QueryPlannerDistinctTest, StrictDistinctOnlyRequirements) {
    params.mainCollectionInfo.options |= QueryPlannerParams::STRICT_DISTINCT_ONLY;
    RAIIServerParameterControllerForTest shardFiltering("featureFlagShardFilteringDistinctScan",
                                                        true);
    addIndex(fromjson("{x: 1}"));
    addIndex(fromjson("{x: 1, y: 1}"));
    addIndex(fromjson("{y: 1, x: 1}"));
    runDistinctQuery("y", fromjson("{x: 3, y: {$gt: 5}}"));

    assertNumSolutions(3);
    // Index {x: 1, y: 1} is transformed even though 'y' is not first in a STRICT_DISTINCT_ONLY
    // situation because x has a single-point bound.
    assertCandidateExists(
        "{proj: {spec: {_id: 0, y: 1}, node: {distinct: {key: 'y', indexPattern: {x: 1, y: 1}}}}}");
    // Index {y: 1, x: 1} is transformed since 'y' is the first field.
    assertCandidateExists(
        "{proj: {spec: {_id: 0, y: 1}, node: {distinct: {key: 'y', indexPattern: {y: 1, x: 1}}}}}");
    assertCandidateExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
}

/**
 * Test wheter the `direction` field of the distinct scan stage is updated accordingly to the
 * direction of the sort pattern.
 */
TEST_F(QueryPlannerDistinctTest, DifferentSortDirections) {
    RAIIServerParameterControllerForTest shardFiltering("featureFlagShardFilteringDistinctScan",
                                                        true);
    addIndex(fromjson("{x: 1, y: 1}"));
    addIndex(fromjson("{x: 1, y: -1}"));
    addIndex(fromjson("{x: -1, y: 1}"));
    runDistinctQuery("x", fromjson("{x: {$gt: 3}}"), fromjson("{x: -1, y: 1}"));

    assertNumSolutions(3);
    assertCandidateExists(
        "{proj: {spec: {_id: 0, x: 1}, node: {distinct: {indexPattern: {x: 1, y: -1}, direction: "
        "'-1'}}}}");
    assertCandidateExists(
        "{proj: {spec: {_id: 0, x: 1}, node: {distinct: {indexPattern: {x: -1, y: 1}, direction: "
        "'1'}}}}");
    assertCandidateExists(
        "{proj: {spec: {_id: 0, x: 1}, node: {sort: {pattern: {x: -1, y: 1}, limit: 0, type: "
        "'default', node: {ixscan: {pattern: {x: 1, y: 1}}}}}}}");
}

/**
 * Test that both forms of the query solution PROJECT + IXSCAN and PROJECT + FETCH + IXSCAN can be
 * transformed to have a distinct scan.
 */
TEST_F(QueryPlannerDistinctTest, DistinctScanWithProjection) {
    RAIIServerParameterControllerForTest shardFiltering("featureFlagShardFilteringDistinctScan",
                                                        true);
    addIndex(fromjson("{x: 1, y: 1}"));
    addIndex(fromjson("{x: 1, z: 1}"));

    // Get DISTINCT_SCAN from PROJECT + IXSCAN.
    runDistinctQuery(
        "x", fromjson("{x: {$gt: 3}, y: {$lt: 5}}"), BSONObj(), fromjson("{_id: 0, x: 1, y: 1}"));
    assertNumSolutions(2);
    assertCandidateExists(
        "{proj: {spec: {_id: 0, x: 1, y: 1}, node: {distinct: {key: 'x', indexPattern: {x: 1, y: "
        "1}}}}}");
    assertCandidateExists(
        "{proj: {spec: {_id: 0, x: 1, y: 1}, node: {fetch: {node: {ixscan: {pattern: {x: 1, z: "
        "1}}}}}}}");

    // Get DISTINCT_SCAN from PROJECT + FETCH + IXSCAN.
    runDistinctQuery(
        "x", fromjson("{x: {$gt: 3}, y: {$lt: 5}}"), BSONObj(), fromjson("{_id: 0, x: 1, z: 1}"));
    assertNumSolutions(2);
    assertCandidateExists("{distinct: {key: 'x', indexPattern: {x: 1, y: 1}, isFetching: true}}");
    assertCandidateExists(
        "{proj: {spec: {_id: 0, x: 1, z: 1}, node: {fetch: {node: {ixscan: {pattern: {x: 1, z: "
        "1}}}}}}}");
}

/**
 * In aggregation, when the query gets rewritten, we can have the situation in which we need to flip
 * the direction in which the results based on a sort pattern will come. In case an IXSCAN is
 * generated instead, it doesn't need to flip its direction since it is not combined with the
 * rewritten pipeline.
 */
TEST_F(QueryPlannerDistinctTest, FlipDistinctScanDirection) {
    RAIIServerParameterControllerForTest shardFiltering("featureFlagShardFilteringDistinctScan",
                                                        true);
    addIndex(fromjson("{x: 1, y: 1}"));
    addIndex(fromjson("{x: 1, z: 1}"));

    // flag off
    runDistinctQuery("x", fromjson("{x: {$gt: 3}}"), fromjson("{x: 1, y: 1}"), BSONObj(), false);
    assertNumSolutions(2);
    assertCandidateExists(
        "{proj: {spec: {_id: 0, x: 1}, node: {distinct: {indexPattern: {x: 1, y: 1}, direction: "
        "'1'}}}}");
    assertCandidateExists(
        "{sort: {pattern: {x: 1, y: 1}, limit: 0, type: 'simple', node: {fetch: {node: {ixscan: "
        "{pattern: {x: 1, z: 1}, dir: 1}}}}}}");

    // flag on
    runDistinctQuery("x", fromjson("{x: {$gt: 3}}"), fromjson("{x: 1, y: 1}"), BSONObj(), true);
    assertNumSolutions(2);
    assertCandidateExists(
        "{proj: {spec: {_id: 0, x: 1}, node: {distinct: {indexPattern: {x: 1, y: 1}, direction: "
        "'-1'}}}}");
    assertCandidateExists(
        "{sort: {pattern: {x: 1, y: 1}, limit: 0, type: 'simple', node: {fetch: {node: {ixscan: "
        "{pattern: {x: 1, z: 1}, dir: 1}}}}}}");
}

}  // namespace
}  // namespace mongo
