/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * Make a minimal IndexEntry from just a key pattern and a name.
 */
IndexEntry buildSimpleIndexEntry(const BSONObj& kp, const std::string& indexName) {
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            false,
            {},
            {},
            false,
            false,
            CoreIndexInfo::Identifier(indexName),
            nullptr,
            {},
            nullptr,
            nullptr};
}


//
// Min/Max
//

TEST_F(QueryPlannerTest, MinValid) {
    addIndex(BSON("a" << 1));
    runQueryHintMinMax(BSONObj(), BSONObj(fromjson("{a: 1}")), fromjson("{a: 1}"), BSONObj());

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, MinWithoutIndex) {
    runInvalidQueryHintMinMax(
        BSONObj(), BSONObj(fromjson("{a: 1}")), fromjson("{a: 1}"), BSONObj());
}

TEST_F(QueryPlannerTest, MinBadHint) {
    addIndex(BSON("b" << 1));
    runInvalidQueryHintMinMax(BSONObj(), fromjson("{b: 1}"), fromjson("{a: 1}"), BSONObj());
}

TEST_F(QueryPlannerTest, MaxValid) {
    addIndex(BSON("a" << 1));
    runQueryHintMinMax(BSONObj(), BSONObj(fromjson("{a: 1}")), BSONObj(), fromjson("{a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, MinMaxSameValue) {
    addIndex(BSON("a" << 1));
    runInvalidQueryHintMinMax(
        BSONObj(), BSONObj(fromjson("{a: 1}")), fromjson("{a: 1}"), fromjson("{a: 1}"));
}

TEST_F(QueryPlannerTest, MaxWithoutIndex) {
    runInvalidQueryHintMinMax(
        BSONObj(), BSONObj(fromjson("{a: 1}")), BSONObj(), fromjson("{a: 1}"));
}

TEST_F(QueryPlannerTest, MaxBadHint) {
    addIndex(BSON("b" << 1));
    runInvalidQueryHintMinMax(BSONObj(), fromjson("{b: 1}"), BSONObj(), fromjson("{a: 1}"));
}

TEST_F(QueryPlannerTest, MaxMinSort) {
    addIndex(BSON("a" << 1));

    // Run an empty query, sort {a: 1}, max/min arguments.
    runQueryFull(BSONObj(),
                 fromjson("{a: 1}"),
                 BSONObj(),
                 0,
                 0,
                 fromjson("{a: 1}"),
                 fromjson("{a: 2}"),
                 fromjson("{a: 8}"));

    assertNumSolutions(1);
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, MaxMinSortEqualityFirstSortSecond) {
    addIndex(BSON("a" << 1 << "b" << 1));

    // Run an empty query, sort {b: 1}, max/min arguments.
    runQueryFull(BSONObj(),
                 fromjson("{b: 1}"),
                 BSONObj(),
                 0,
                 0,
                 fromjson("{a: 1, b: 1}"),
                 fromjson("{a: 1, b: 1}"),
                 fromjson("{a: 1, b: 2}"));

    assertNumSolutions(1);
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, MaxMinSortInequalityFirstSortSecond) {
    addIndex(BSON("a" << 1 << "b" << 1));

    // Run an empty query, sort {b: 1}, max/min arguments.
    runQueryFull(BSONObj(),
                 fromjson("{b: 1}"),
                 BSONObj(),
                 0,
                 0,
                 fromjson("{a: 1, b: 1}"),
                 fromjson("{a: 1, b: 1}"),
                 fromjson("{a: 2, b: 2}"));

    assertNumSolutions(1);
    assertSolutionExists(
        "{fetch: {node: {sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node: {ixscan: "
        "{filter: null, pattern: {a: 1, b: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, MaxMinReverseSort) {
    addIndex(BSON("a" << 1));

    // Run an empty query, sort {a: -1}, max/min arguments.
    runQueryFull(BSONObj(),
                 fromjson("{a: -1}"),
                 BSONObj(),
                 0,
                 0,
                 fromjson("{a: 1}"),
                 fromjson("{a: 2}"),
                 fromjson("{a: 8}"));

    assertNumSolutions(1);
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, dir: -1, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, MaxMinReverseIndexDir) {
    addIndex(BSON("a" << -1));

    // Because the index is descending, the min is numerically larger than the max.
    runQueryFull(BSONObj(),
                 fromjson("{a: -1}"),
                 BSONObj(),
                 0,
                 0,
                 fromjson("{a: -1}"),
                 fromjson("{a: 8}"),
                 fromjson("{a: 2}"));

    assertNumSolutions(1);
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, dir: 1, pattern: {a: -1}}}}}");
}

TEST_F(QueryPlannerTest, MaxMinReverseIndexDirSort) {
    addIndex(BSON("a" << -1));

    // Min/max specifies a forward scan with bounds [{a: 8}, {a: 2}]. Asking for
    // an ascending sort reverses the direction of the scan to [{a: 2}, {a: 8}].
    runQueryFull(BSONObj(),
                 fromjson("{a: 1}"),
                 BSONObj(),
                 0,
                 0,
                 fromjson("{a: -1}"),
                 fromjson("{a: 8}"),
                 fromjson("{a: 2}"));

    assertNumSolutions(1);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {filter: null, dir: -1,"
        "pattern: {a: -1}}}}}");
}

TEST_F(QueryPlannerTest, MaxMinNoMatchingIndexDir) {
    addIndex(BSON("a" << -1));
    runInvalidQueryHintMinMax(
        BSONObj(), fromjson("{a: 1}"), BSONObj(fromjson("{a: 2}")), fromjson("{a: 8}"));
}

TEST_F(QueryPlannerTest, MaxMinBadHintIfIndexOrderDoesNotMatch) {
    // There are both ascending and descending indices on 'a'.
    addIndex(BSON("a" << 1));

    // A query hinting on {a: 1} is bad if min is {a: 8} and {a: 2} because this
    // min/max pairing requires a descending index.
    runInvalidQueryFull(BSONObj(),
                        BSONObj(),
                        BSONObj(),
                        0,
                        0,
                        fromjson("{a: 1}"),
                        fromjson("{a: 8}"),
                        fromjson("{a: 2}"));
}


//
// Hint tests
//

TEST_F(QueryPlannerTest, NaturalHint) {
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuerySortHint(BSON("a" << 1), BSON("b" << 1), BSON("$natural" << 1));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {filter: {a: 1}, dir: 1}}}}}}");
}

// Test $natural sort and its interaction with $natural hint.
TEST_F(QueryPlannerTest, NaturalSortAndHint) {
    addIndex(BSON("x" << 1));

    // Non-empty query, -1 sort, no hint.
    runQuerySortHint(fromjson("{x: {$exists: true}}"), BSON("$natural" << -1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: -1}}");

    // Non-empty query, 1 sort, no hint.
    runQuerySortHint(fromjson("{x: {$exists: true}}"), BSON("$natural" << 1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");

    // Non-empty query, -1 sort, -1 hint.
    runQuerySortHint(
        fromjson("{x: {$exists: true}}"), BSON("$natural" << -1), BSON("$natural" << -1));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: -1}}");

    // Non-empty query, 1 sort, 1 hint.
    runQuerySortHint(
        fromjson("{x: {$exists: true}}"), BSON("$natural" << 1), BSON("$natural" << 1));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");

    // Empty query, -1 sort, no hint.
    runQuerySortHint(BSONObj(), BSON("$natural" << -1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: -1}}");

    // Empty query, 1 sort, no hint.
    runQuerySortHint(BSONObj(), BSON("$natural" << 1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");

    // Empty query, -1 sort, -1 hint.
    runQuerySortHint(BSONObj(), BSON("$natural" << -1), BSON("$natural" << -1));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: -1}}");

    // Empty query, 1 sort, 1 hint.
    runQuerySortHint(BSONObj(), BSON("$natural" << 1), BSON("$natural" << 1));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, HintValid) {
    addIndex(BSON("a" << 1));
    runQueryHint(BSONObj(), fromjson("{a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, HintValidWithPredicate) {
    addIndex(BSON("a" << 1));
    runQueryHint(fromjson("{a: {$gt: 1}}"), fromjson("{a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, HintValidWithSort) {
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuerySortHint(fromjson("{a: 100, b: 200}"), fromjson("{b: 1}"), fromjson("{a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {filter: {b: 200}, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, HintElemMatch) {
    // true means multikey
    addIndex(fromjson("{'a.b': 1}"), true);
    runQueryHint(fromjson("{'a.b': 1, a: {$elemMatch: {b: 2}}}"), fromjson("{'a.b': 1}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a:{$elemMatch:{b:2}}}, {'a.b': 1}]}, "
        "node: {ixscan: {filter: null, pattern: {'a.b': 1}, bounds: "
        "{'a.b': [[2, 2, true, true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:2}}}, "
        "node: {ixscan: {filter: null, pattern: {'a.b': 1}, bounds: "
        "{'a.b': [[1, 1, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, HintInvalid) {
    addIndex(BSON("a" << 1));
    runInvalidQueryHint(BSONObj(), fromjson("{b: 1}"));
}

TEST_F(QueryPlannerTest, HintedNotCoveredProjectionIndexFilteredOut) {
    params.options = QueryPlannerParams::NO_UNCOVERED_PROJECTIONS;
    addIndex(BSON("a" << 1));
    addIndex(BSON("a" << 1 << "b" << 1));
    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: 1}, projection: {a: 1, b: 1, _id: 0}, hint: {a: 1}}"));
    assertNumSolutions(0U);
}


//
// Test the "split limited sort stages" hack.
//

TEST_F(QueryPlannerTest, SplitLimitedSort) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    params.options |= QueryPlannerParams::SPLIT_LIMITED_SORT;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    runQuerySortProjSkipNToReturn(fromjson("{a: 1}"), fromjson("{b: 1}"), BSONObj(), 0, 3);

    assertNumSolutions(2U);
    // First solution has no blocking stage; no need to split.
    assertSolutionExists(
        "{fetch: {filter: {a:1}, node: "
        "{ixscan: {filter: null, pattern: {b: 1}}}}}");
    // Second solution has a blocking sort with a limit: it gets split and
    // joined with an OR stage.
    assertSolutionExists(
        "{ensureSorted: {pattern: {b: 1}, node: "
        "{or: {nodes: ["
        "{sort: {pattern: {b: 1}, limit: 3, node: {sortKeyGen: {node: "
        "{fetch: {node: {ixscan: {pattern: {a: 1}}}}}}}}}, "
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {node: {ixscan: {pattern: {a: 1}}}}}}}}}]}}}}");
}

// The same query run as a find command with a limit should not require the "split limited sort"
// hack.
TEST_F(QueryPlannerTest, NoSplitLimitedSortAsCommand) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    params.options |= QueryPlannerParams::SPLIT_LIMITED_SORT;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: 1}, sort: {b: 1}, limit: 3}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{limit: {n: 3, node: {fetch: {filter: {a:1}, node: "
        "{ixscan: {filter: null, pattern: {b: 1}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 3, node: {sortKeyGen: {node: {fetch: {filter: null,"
        "node: {ixscan: {pattern: {a: 1}}}}}}}}}");
}

// Same query run as a find command with a batchSize rather than a limit should not require
// the "split limited sort" hack, and should not have any limit represented inside the plan.
TEST_F(QueryPlannerTest, NoSplitLimitedSortAsCommandBatchSize) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    params.options |= QueryPlannerParams::SPLIT_LIMITED_SORT;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: 1}, sort: {b: 1}, batchSize: 3}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {a: 1}, node: {ixscan: "
        "{filter: null, pattern: {b: 1}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node: {fetch: {filter: null,"
        "node: {ixscan: {pattern: {a: 1}}}}}}}}}");
}


//
// Test shard filter query planning
//

TEST_F(QueryPlannerTest, ShardFilterCollScan) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1);
    addIndex(BSON("a" << 1));

    runQuery(fromjson("{b: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sharding_filter: {node: "
        "{cscan: {dir: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterBasicIndex) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1);
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    runQuery(fromjson("{b: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sharding_filter: {node: "
        "{fetch: {node: "
        "{ixscan: {pattern: {b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterBasicCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1);
    addIndex(BSON("a" << 1));

    runQuery(fromjson("{a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: "
        "{sharding_filter: {node: "
        "{ixscan: {pattern: {a: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterBasicProjCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1);
    addIndex(BSON("a" << 1));

    runQuerySortProj(fromjson("{a: 1}"), BSONObj(), fromjson("{_id : 0, a : 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, type: 'coveredIndex', node: "
        "{sharding_filter: {node: "
        "{ixscan: {pattern: {a: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterCompoundProjCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1 << "b" << 1);
    addIndex(BSON("a" << 1 << "b" << 1));

    runQuerySortProj(fromjson("{a: 1}"), BSONObj(), fromjson("{_id: 0, a: 1, b: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1 }, type: 'coveredIndex', node: "
        "{sharding_filter: {node: "
        "{ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterNestedProjCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1 << "b.c" << 1);
    addIndex(BSON("a" << 1 << "b.c" << 1));

    runQuerySortProj(fromjson("{a: 1}"), BSONObj(), fromjson("{_id: 0, a: 1, 'b.c': 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, 'b.c': 1 }, type: 'default', node: "
        "{sharding_filter: {node: "
        "{ixscan: {filter: null, pattern: {a: 1, 'b.c': 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterHashProjNotCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a"
                           << "hashed");
    addIndex(BSON("a"
                  << "hashed"));

    runQuerySortProj(fromjson("{a: 1}"), BSONObj(), fromjson("{_id : 0, a : 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0,a: 1}, type: 'simple', node: "
        "{sharding_filter : {node: "
        "{fetch: {node: "
        "{ixscan: {pattern: {a: 'hashed'}}}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterKeyPrefixIndexCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1);
    addIndex(BSON("a" << 1 << "b" << 1 << "_id" << 1));

    runQuerySortProj(fromjson("{a: 1}"), BSONObj(), fromjson("{a : 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {a: 1}, type: 'coveredIndex', node: "
        "{sharding_filter : {node: "
        "{ixscan: {pattern: {a: 1, b: 1, _id: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterNoIndexNotCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a"
                           << "hashed");
    addIndex(BSON("b" << 1));

    runQuerySortProj(fromjson("{b: 1}"), BSONObj(), fromjson("{_id : 0, a : 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0,a: 1}, type: 'simple', node: "
        "{sharding_filter : {node: "
        "{fetch: {node: "
        "{ixscan: {pattern: {b: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, CannotTrimIxisectParam) {
    params.options = QueryPlannerParams::INDEX_INTERSECTION;
    params.options |= QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    runQuery(fromjson("{a: 1, b: 1, c: 1}"));

    assertNumSolutions(3U);
    assertSolutionExists(
        "{fetch: {filter: {b: 1, c: 1}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: 1, c: 1}, node: "
        "{ixscan: {filter: null, pattern: {b: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:1,b:1,c:1}, node: {andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1}}},"
        "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
}

TEST_F(QueryPlannerTest, CannotTrimIxisectParamBeneathOr) {
    params.options = QueryPlannerParams::INDEX_INTERSECTION;
    params.options |= QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));

    runQuery(fromjson("{d: 1, $or: [{a: 1}, {b: 1, c: 1}]}"));

    assertNumSolutions(3U);

    assertSolutionExists(
        "{fetch: {filter: {d: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {c: 1}, node: {ixscan: {filter: null,"
        "pattern: {b: 1}, bounds: {b: [[1,1,true,true]]}}}}},"
        "{ixscan: {filter: null, pattern: {a: 1},"
        "bounds: {a: [[1,1,true,true]]}}}]}}}}");

    assertSolutionExists(
        "{fetch: {filter: {d: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {b: 1}, node: {ixscan: {filter: null,"
        "pattern: {c: 1}, bounds: {c: [[1,1,true,true]]}}}}},"
        "{ixscan: {filter: null, pattern: {a: 1},"
        "bounds: {a: [[1,1,true,true]]}}}]}}}}");

    assertSolutionExists(
        "{fetch: {filter: {d: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {b: 1, c: 1}, node: {andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {b: 1}}},"
        "{ixscan: {filter: null, pattern: {c: 1}}}]}}}},"
        "{ixscan: {filter: null, pattern: {a: 1}}}]}}}}");
}

TEST_F(QueryPlannerTest, CannotTrimIxisectAndHashWithOrChild) {
    params.options = QueryPlannerParams::INDEX_INTERSECTION;
    params.options |= QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));

    runQuery(fromjson("{c: 1, $or: [{a: 1}, {b: 1, d: 1}]}"));

    assertNumSolutions(3U);

    assertSolutionExists(
        "{fetch: {filter: {c: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {d: 1}, node: {ixscan: {filter: null,"
        "pattern: {b: 1}, bounds: {b: [[1,1,true,true]]}}}}},"
        "{ixscan: {filter: null, pattern: {a: 1},"
        "bounds: {a: [[1,1,true,true]]}}}]}}}}");

    assertSolutionExists(
        "{fetch: {filter: {$or:[{b:1,d:1},{a:1}]}, node:"
        "{ixscan: {filter: null, pattern: {c: 1}}}}}");

    assertSolutionExists(
        "{fetch: {filter: {c:1,$or:[{a:1},{b:1,d:1}]}, node:{andHash:{nodes:["
        "{or: {nodes: ["
        "{fetch: {filter: {d:1}, node: {ixscan: {pattern: {b: 1}}}}},"
        "{ixscan: {filter: null, pattern: {a: 1}}}]}},"
        "{ixscan: {filter: null, pattern: {c: 1}}}]}}}}");
}

TEST_F(QueryPlannerTest, CannotTrimIxisectParamSelfIntersection) {
    params.options = QueryPlannerParams::INDEX_INTERSECTION;
    params.options |= QueryPlannerParams::NO_TABLE_SCAN;

    // true means multikey
    addIndex(BSON("a" << 1), true);

    runQuery(fromjson("{a: {$all: [1, 2, 3]}}"));

    assertNumSolutions(4U);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a:2}, {a:3}]}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a:1}, {a:3}]}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a:2}, {a:3}]}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$all: [1, 2, 3]}}, node: {andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1},"
        "bounds: {a: [[1,1,true,true]]}}},"
        "{ixscan: {filter: null, pattern: {a:1},"
        "bounds: {a: [[2,2,true,true]]}}},"
        "{ixscan: {filter: null, pattern: {a:1},"
        "bounds: {a: [[3,3,true,true]]}}}]}}}}");
}


// If a lookup against a unique index is available as a possible plan, then the planner
// should not generate other possibilities.
TEST_F(QueryPlannerTest, UniqueIndexLookup) {
    params.options = QueryPlannerParams::INDEX_INTERSECTION;
    params.options |= QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1),
             false,  // multikey
             false,  // sparse,
             true);  // unique

    runQuery(fromjson("{a: 1, b: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: 1}, node: "
        "{ixscan: {filter: null, pattern: {b: 1}}}}}");
}

TEST_F(QueryPlannerTest, HintOnNonUniqueIndex) {
    params.options = QueryPlannerParams::INDEX_INTERSECTION;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1),
             false,  // multikey
             false,  // sparse,
             true);  // unique

    runQueryHint(fromjson("{a: 1, b: 1}"), BSON("a" << 1));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {b: 1}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, UniqueIndexLookupBelowOr) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));
    addIndex(BSON("d" << 1),
             false,  // multikey
             false,  // sparse,
             true);  // unique

    runQuery(fromjson("{$or: [{a: 1, b: 1}, {c: 1, d: 1}]}"));

    // Only two plans because we throw out plans for the right branch of the $or that do not
    // use equality over the unique index.
    assertNumSolutions(2U);
    assertSolutionExists(
        "{or: {nodes: ["
        "{fetch: {filter: {a: 1}, node: {ixscan: {pattern: {b: 1}}}}},"
        "{fetch: {filter: {c: 1}, node: {ixscan: {pattern: {d: 1}}}}}]}}");
    assertSolutionExists(
        "{or: {nodes: ["
        "{fetch: {filter: {b: 1}, node: {ixscan: {pattern: {a: 1}}}}},"
        "{fetch: {filter: {c: 1}, node: {ixscan: {pattern: {d: 1}}}}}]}}");
}

TEST_F(QueryPlannerTest, UniqueIndexLookupBelowOrBelowAnd) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));
    addIndex(BSON("d" << 1),
             false,  // multikey
             false,  // sparse,
             true);  // unique

    runQuery(fromjson("{e: 1, $or: [{a: 1, b: 1}, {c: 1, d: 1}]}"));

    // Only two plans because we throw out plans for the right branch of the $or that do not
    // use equality over the unique index.
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {e: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {a: 1}, node: {ixscan: {pattern: {b: 1}}}}},"
        "{fetch: {filter: {c: 1}, node: {ixscan: {pattern: {d: 1}}}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {e: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {b: 1}, node: {ixscan: {pattern: {a: 1}}}}},"
        "{fetch: {filter: {c: 1}, node: {ixscan: {pattern: {d: 1}}}}}"
        "]}}}}");
}

TEST_F(QueryPlannerTest, CoveredOrUniqueIndexLookup) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("a" << 1),
             false,  // multikey
             false,  // sparse,
             true);  // unique

    runQuerySortProj(fromjson("{a: 1, b: 1}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{fetch: {filter: {b: 1}, node: {ixscan: {pattern: {a: 1}}}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, SortKeyMetaProjection) {
    addIndex(BSON("a" << 1));

    runQuerySortProj(BSONObj(), fromjson("{a: 1}"), fromjson("{b: {$meta: 'sortKey'}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{proj: {spec: {b: {$meta: 'sortKey'}}, node: "
        "{sort: {limit: 0, pattern: {a: 1}, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}}}");
    assertSolutionExists(
        "{proj: {spec: {b: {$meta: 'sortKey'}}, node: "
        "{sortKeyGen: {node: {fetch: {filter: null, node: "
        "{ixscan: {pattern: {a: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, SortKeyMetaProjectionCovered) {
    addIndex(BSON("a" << 1));

    runQuerySortProj(
        BSONObj(), fromjson("{a: 1}"), fromjson("{_id: 0, a: 1, b: {$meta: 'sortKey'}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: {$meta: 'sortKey'}}, node: "
        "{sort: {limit: 0, pattern: {a: 1}, node: "
        "{sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: {$meta: 'sortKey'}}, node: "
        "{sortKeyGen: {node: "
        "{ixscan: {pattern: {a: 1}}}}}}}");
}


//
// Test bad input to query planner helpers.
//

TEST_F(QueryPlannerTest, CacheDataFromTaggedTreeFailsOnBadInput) {
    // Null match expression.
    std::vector<IndexEntry> relevantIndices;
    ASSERT_NOT_OK(QueryPlanner::cacheDataFromTaggedTree(nullptr, relevantIndices).getStatus());

    // No relevant index matching the index tag.
    relevantIndices.push_back(buildSimpleIndexEntry(BSON("a" << 1), "a_1"));

    auto qr = std::make_unique<QueryRequest>(NamespaceString("test.collection"));
    qr->setFilter(BSON("a" << 3));
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx.get(), std::move(qr));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> scopedCq = std::move(statusWithCQ.getValue());
    scopedCq->root()->setTag(new IndexTag(1));

    ASSERT_NOT_OK(
        QueryPlanner::cacheDataFromTaggedTree(scopedCq->root(), relevantIndices).getStatus());
}

TEST_F(QueryPlannerTest, TagAccordingToCacheFailsOnBadInput) {
    const NamespaceString nss("test.collection");

    auto qr = std::make_unique<QueryRequest>(nss);
    qr->setFilter(BSON("a" << 3));
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx.get(), std::move(qr));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> scopedCq = std::move(statusWithCQ.getValue());

    std::unique_ptr<PlanCacheIndexTree> indexTree(new PlanCacheIndexTree());
    indexTree->setIndexEntry(buildSimpleIndexEntry(BSON("a" << 1), "a_1"));

    std::map<IndexEntry::Identifier, size_t> indexMap;

    // Null filter.
    Status s = QueryPlanner::tagAccordingToCache(nullptr, indexTree.get(), indexMap);
    ASSERT_NOT_OK(s);

    // Null indexTree.
    s = QueryPlanner::tagAccordingToCache(scopedCq->root(), nullptr, indexMap);
    ASSERT_NOT_OK(s);

    // Index not found.
    s = QueryPlanner::tagAccordingToCache(scopedCq->root(), indexTree.get(), indexMap);
    ASSERT_NOT_OK(s);

    // Index found once added to the map.
    indexMap[IndexEntry::Identifier{"a_1"}] = 0;
    s = QueryPlanner::tagAccordingToCache(scopedCq->root(), indexTree.get(), indexMap);
    ASSERT_OK(s);

    // Regenerate canonical query in order to clear tags.
    auto newQR = std::make_unique<QueryRequest>(nss);
    newQR->setFilter(BSON("a" << 3));
    statusWithCQ = CanonicalQuery::canonicalize(opCtx.get(), std::move(newQR));
    ASSERT_OK(statusWithCQ.getStatus());
    scopedCq = std::move(statusWithCQ.getValue());

    // Mismatched tree topology.
    PlanCacheIndexTree* child = new PlanCacheIndexTree();
    child->setIndexEntry(buildSimpleIndexEntry(BSON("a" << 1), "a_1"));
    indexTree->children.push_back(child);
    s = QueryPlanner::tagAccordingToCache(scopedCq->root(), indexTree.get(), indexMap);
    ASSERT_NOT_OK(s);
}


// A query run as a find command with a sort and ntoreturn should generate a plan implementing
// the 'ntoreturn hack'.
TEST_F(QueryPlannerTest, NToReturnHackWithFindCommand) {
    params.options |= QueryPlannerParams::SPLIT_LIMITED_SORT;

    runQueryAsCommand(fromjson("{find: 'testns', sort: {a:1}, ntoreturn:3}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{ensureSorted: {pattern: {a: 1}, node: "
        "{or: {nodes: ["
        "{sort: {limit:3, pattern: {a:1}, node: {sortKeyGen: {node: {cscan: {dir:1}}}}}}, "
        "{sort: {limit:0, pattern: {a:1}, node: {sortKeyGen: {node: {cscan: {dir:1}}}}}}"
        "]}}}}");
}

TEST_F(QueryPlannerTest, NToReturnHackWithSingleBatch) {
    params.options |= QueryPlannerParams::SPLIT_LIMITED_SORT;

    runQueryAsCommand(fromjson("{find: 'testns', sort: {a:1}, ntoreturn:3, singleBatch:true}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a:1}, limit:3, node: {sortKeyGen: {node: "
        "{cscan: {dir:1, filter: {}}}}}}}");
}


}  // namespace
}  // namespace mongo
