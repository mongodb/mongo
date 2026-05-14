/**
 * Repro for SERVER-122866: $queryStats reports collectionType "timeseries" for a view defined over
 * a timeseries collection, when it should report "view" (matching the slow-query-log
 * collectionType and matching the behavior for a view over a regular collection).
 *
 * This test asserts the expected post-fix behavior and is therefore expected to FAIL on a master
 * build that still carries the bug. Once SERVER-122866 is fixed, the assertions should pass.
 *
 * @tags: [requires_fcv_72]
 */
import {getQueryStats} from "jstests/libs/query/query_stats_utils.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryStatsRateLimit: -1,
    },
});

const testDB = conn.getDB("test");
const tsName = jsTestName() + "_timeseries";
const viewName = jsTestName() + "_viewOverTimeseries";

// Underlying timeseries collection.
assert.commandWorked(testDB.createCollection(tsName, {timeseries: {timeField: "time"}}));

// View defined over the timeseries collection. Use a $match so the view pipeline is non-trivial
// and the resolved query shape is distinguishable from a query against the bucket collection.
assert.commandWorked(
    testDB.createView(viewName, tsName, [{$match: {v: {$gt: 0}}}]),
);

// Seed a few measurements so the view returns documents.
const tsColl = testDB[tsName];
assert.commandWorked(tsColl.insert({v: 1, time: ISODate("2026-01-01T00:00:00.000Z")}));
assert.commandWorked(tsColl.insert({v: 2, time: ISODate("2026-01-01T01:00:00.000Z")}));
assert.commandWorked(tsColl.insert({v: 3, time: ISODate("2026-01-01T02:00:00.000Z")}));

// Run a find and an aggregate against the VIEW (not the underlying timeseries) to generate two
// query stats entries with distinct shapes (find over a view is rewritten to aggregate, but the
// resulting shapes still differ between the two user-facing commands).
const viewColl = testDB[viewName];
assert.eq(3, viewColl.find({v: {$gt: 0}}).itcount());
assert.eq(3, viewColl.aggregate([{$match: {v: {$lt: 99}}}]).itcount());

// EXPECTED post-fix: both entries are tagged collectionType "view", because the user-facing
// namespace is a view. The bug today reports "timeseries" instead.
const viewEntries = getQueryStats(conn, {
    extraMatch: {
        "key.collectionType": "view",
        "key.queryShape.cmdNs.coll": viewName,
    },
});
assert.eq(
    2,
    viewEntries.length,
    "SERVER-122866: expected 2 query stats entries with collectionType=='view' for a view " +
        "defined over a timeseries collection, got " + viewEntries.length + ". " +
        "Full entries for this namespace: " +
        tojson(getQueryStats(conn, {extraMatch: {"key.queryShape.cmdNs.coll": viewName}})),
);

// And the bug-shape: there should be NO entries tagged "timeseries" under the view's namespace.
const wronglyTaggedEntries = getQueryStats(conn, {
    extraMatch: {
        "key.collectionType": "timeseries",
        "key.queryShape.cmdNs.coll": viewName,
    },
});
assert.eq(
    0,
    wronglyTaggedEntries.length,
    "SERVER-122866: a view over a timeseries collection must not surface as collectionType " +
        "'timeseries' in $queryStats. Offending entries: " + tojson(wronglyTaggedEntries),
);

MongoRunner.stopMongod(conn);
