/**
 * Test that the telemetry metrics are updated correctly and persist across getMores.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod({});
const db = conn.getDB('test');

var coll = db[jsTestName()];
var collTwo = db[jsTestName() + 'Two'];
coll.drop();

for (var i = 0; i < 100; i++) {
    coll.insert({foo: 0});
    coll.insert({foo: 1});
    collTwo.insert({foo: Math.random(0, 1), bar: Math.random(0, 1)});
}

function verifyTelemetryMetrics() {
    const telStore = db.adminCommand({aggregate: 1, pipeline: [{$telemetry: {}}], cursor: {}});
    // print(tojson(telStore));
    const metrics = telStore.cursor.firstBatch[0].metrics;
    print(tojson(metrics));
    assert(metrics.execCount > 0);
    assert(metrics.firstSeenTimestamp);
    // assert(metrics.lastExecutionMicros > 0);
    // assert(metrics.queryOptMicros.sum > 0);
    // assert(metrics.queryExecMicros.sum > 0);
    // assert(metrics.docsReturned.sum > 0);
    // assert(metrics.docsScanned.sum > 0);
    // assert(metrics.keysScanned.sum > 0);
}

let query;

// agg query
query = {
    $setWindowFields: {
        sortBy: {_id: 1},
        output: {foo: {$linearFill: "$foo"}},
    }
};
coll.aggregate([query]);
verifyTelemetryMetrics();

// agg query with some stages pushed to find layer.
coll.aggregate([{$match: {foo: 0}}, {$group: {_id: null, count: {$sum: 1}}}]);
verifyTelemetryMetrics();

// agg query with all stages pushed to find layer.
coll.aggregate([{$sort: {foo: 1}}]);
verifyTelemetryMetrics();

// multiple batches require multiple plan executors. We want to confirm we are only storing the
// metrics for the outer executor associated with planning the query, and not a subsequent executor
// that is constructed when a new operation context gets created during getMore() calls.
// coll.aggregate([{$unionWith: collTwo.getName()}], {cursor: {batchSize: 2}});
// verifyTelemetryMetrics();

// $lookup has inner executor (cursor??), we want to confirm we are only reporting metrics from the
// outer executor associated with planning the query.
coll.aggregate({
    $lookup: {from: collTwo.getName(), localField: "foo", foreignField: "bar", as: "merged_docs"}
});
verifyTelemetryMetrics();

// Count and find have different entry points (eg different run() methods) from agg and we want to
// confirm we are starting the timer as planning begins in each of these workflows/paths.
coll.count({foo: 0});
verifyTelemetryMetrics();

query = coll.findOne({});
verifyTelemetryMetrics(query);
MongoRunner.stopMongod(conn);
})();
