/**
 * Test that the bucket unpacking with sorting rewrite is performed and doesn't cause incorrect
 * results to be created. This test is focused on timeseries with compound indexes and compound sorts
 * on meta sub-fields, with point queries and sorts on the same fields.
 *
 * @tags: [
 *     # Explain of a resolved view must be executed by mongos.
 *     directly_against_shardsvrs_incompatible,
 *     # This complicates aggregation extraction.
 *     do_not_wrap_aggregations_in_facets,
 *     # Refusing to run a test that issues an aggregation command with explain because it may
 *     # return incomplete results if interrupted by a stepdown.
 *     does_not_support_stepdowns,
 *     # We need a timeseries collection.
 *     requires_timeseries,
 *     # TODO (SERVER-104171) the timeseries setup runs a migration. Remove the upgrade-downgrade
 *     # incompatible tag once migrations work during downgrade.
 *     cannot_run_during_upgrade_downgrade,
 *     requires_getmore,
 *     # Bounded sort optimization for match+sort on the same field only works as of v9.0
 *     requires_fcv_90,
 * ]
 */
import {
    backwardIxscan,
    forwardIxscan,
    runRewritesTest,
    runDoesntRewriteTest,
    setupColl,
} from "jstests/core/timeseries/libs/timeseries_sort_util.js";

const collName = jsTestName();
const coll = db[collName];
const subFields = ["a", "b", "c"];

setupColl(coll, collName, true, subFields);

const index = {"m.a": 1, "m.b": 1, "m.c": 1, t: -1};
// Point predicates on all three meta sub-fields, with a time range filter.
// Data timestamps start at new Date(0) with 6-second intervals; this range covers roughly
// the first 50 minutes and guarantees non-empty results regardless of bucket max count.
const timeFilter = {t: {$gt: new Date(1000), $lte: new Date(3000000)}};
const pointQueryMatch = [{$match: {"m.a": 5, "m.b": 5, "m.c": 5, ...timeFilter}}];

// Sort matches index direction: forward scan.
runRewritesTest(
    {"m.a": 1, "m.b": 1, "m.c": 1, t: -1},
    index,
    index,
    forwardIxscan,
    coll,
    true,
    pointQueryMatch,
);

// Sort is fully reversed from index: backward scan.
runRewritesTest(
    {"m.a": -1, "m.b": -1, "m.c": -1, t: 1},
    index,
    index,
    backwardIxscan,
    coll,
    false,
    pointQueryMatch,
);

// All meta fields have point predicates so only t direction determines the scan:
// t:1 requires a backward scan since the index has t:-1
runRewritesTest(
    {"m.a": 1, "m.b": 1, "m.c": 1, t: 1},
    index,
    index,
    backwardIxscan,
    coll,
    false,
    pointQueryMatch,
);
runRewritesTest(
    {"m.a": -1, "m.b": 1, "m.c": -1, t: -1},
    index,
    index,
    forwardIxscan,
    coll,
    true,
    pointQueryMatch,
);

// Point queries on m.a and m.c, only sort on m.b has to match index traversal direction
const pointQueryMatchOnAC = [{$match: {"m.a": 5, "m.c": 5, ...timeFilter}}];
// m.b has correct direction, m.a and m.c wrong but shouldn't affect traversal
runRewritesTest(
    {"m.a": -1, "m.b": 1, "m.c": 1, t: -1},
    index,
    index,
    forwardIxscan,
    coll,
    true,
    pointQueryMatchOnAC,
);
runRewritesTest(
    {"m.a": -1, "m.b": -1, "m.c": 1, t: 1},
    index,
    index,
    backwardIxscan,
    coll,
    false,
    pointQueryMatchOnAC,
);
// m.b has wrong direction, m.a or m.c wrong
runDoesntRewriteTest(
    {"m.a": 1, "m.b": -1, "m.c": -1, t: -1},
    index,
    index,
    coll,
    pointQueryMatchOnAC,
);
runDoesntRewriteTest(
    {"m.a": 1, "m.b": 1, "m.c": -1, t: 1},
    index,
    index,
    coll,
    pointQueryMatchOnAC,
);
// m.b has wrong direction, m.a and m.c both correct
runDoesntRewriteTest(
    {"m.a": 1, "m.b": -1, "m.c": 1, t: -1},
    index,
    index,
    coll,
    pointQueryMatchOnAC,
);
runDoesntRewriteTest(
    {"m.a": -1, "m.b": 1, "m.c": -1, t: 1},
    index,
    index,
    coll,
    pointQueryMatchOnAC,
);
