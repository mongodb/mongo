/**
 * Test that the telemetry metrics are updated correctly across getMores.
 * @tags: [
 *   #TODO SERVER-72406 Unblock this from the telemetry passthrough
 *   exclude_from_telemetry_passthrough,
 * ]
 */
load("jstests/libs/feature_flag_util.js");  // For FeatureFlagUtil.

(function() {
"use strict";

if (!FeatureFlagUtil.isEnabled(db, "Telemetry")) {
    return;
}

// Turn on the collecting of telemetry metrics.
let options = {
    setParameter: {internalQueryConfigureTelemetrySamplingRate: 2147483647},
};

const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB('test');
var coll = testDB[jsTestName()];
var collTwo = db[jsTestName() + 'Two'];
coll.drop();

function verifyMetrics(batch) {
    batch.forEach(element => {
        assert.gt(element.metrics.docsScanned.sum, element.metrics.docsScanned.min);
        assert.gte(element.metrics.docsScanned.sum, element.metrics.docsScanned.max);
        assert.lte(element.metrics.docsScanned.min, element.metrics.docsScanned.max);

        // Ensure execution count does not increase with subsequent getMore() calls.
        assert.eq(element.metrics.execCount.sum,
                  element.metrics.execCount.min,
                  element.metrics.execCount.max);

        if (element.metrics.execCount == 1) {
            // Ensure planning time is > 0 after first batch and does not change with subsequent
            // getMore() calls.
            assert.gt(element.metrics.queryOptMicros.min, 0);
            assert.eq(element.metrics.queryOptMicros.sum,
                      element.metrics.queryOptMicros.min,
                      element.metrics.queryOptMicros.max);
        }
        // Confirm that execution time increases with getMore() calls
        assert.gt(element.metrics.queryExecMicros.sum, element.metrics.queryExecMicros.min);
        assert.gt(element.metrics.queryExecMicros.sum, element.metrics.queryExecMicros.max);
        assert.lte(element.metrics.queryExecMicros.min, element.metrics.queryExecMicros.max);
    });
}

// Bulk insert documents to reduces roundtrips and make timeout on a slow machine less likely.
const bulk = coll.initializeUnorderedBulkOp();
const bulkTwo = collTwo.initializeUnorderedBulkOp();
for (let i = 0; i < 200; ++i) {
    bulk.insert({foo: 0, bar: Math.floor(Math.random() * 3)});
    bulk.insert({foo: 1, bar: Math.floor(Math.random() * -2)});
    bulkTwo.insert({foo: Math.floor(Math.random() * 2), bar: Math.floor(Math.random() * 2)});
}
assert.commandWorked(bulk.execute());
assert.commandWorked(bulkTwo.execute());

// Assert that two queries with identical structures are represented by the same key
coll.aggregate([{$match: {foo: 1}}], {cursor: {batchSize: 2}});
coll.aggregate([{$match: {foo: 0}}], {cursor: {batchSize: 2}});
// This command will return all telemetry store entires.
let telStore = testDB.adminCommand({aggregate: 1, pipeline: [{$telemetry: {}}], cursor: {}});
assert.eq(telStore.cursor.firstBatch.length, 1);
// Assert we update execution count for identically shaped queries.
assert.eq(telStore.cursor.firstBatch[0].metrics.execCount, 2);
verifyMetrics(telStore.cursor.firstBatch);

// Assert that options such as limit/sort create different keys
coll.find({foo: {$eq: 0}}).batchSize(2).toArray();
coll.find({foo: {$eq: 1}}).limit(50).batchSize(2).toArray();
coll.find().sort({"foo": 1}).batchSize(2).toArray();
// This filters telemetry entires to just the ones entered when running above find queries.
telStore = testDB.adminCommand({
    aggregate: 1,
    pipeline: [{$telemetry: {}}, {$match: {"key.find.find": {$eq: "###"}}}],
    cursor: {}
});
assert.eq(telStore.cursor.firstBatch.length, 3);
verifyMetrics(telStore.cursor.firstBatch);

// Ensure that for queries using an index, keys scanned is nonzero.
assert.commandWorked(coll.createIndex({bar: 1}));
coll.aggregate([{$match: {$or: [{bar: 1, foo: 1}]}}], {cursor: {batchSize: 2}});
// This filters telemetry entries to just the one entered for the above agg command.
telStore = testDB.adminCommand({
    aggregate: 1,
    pipeline: [
        {$telemetry: {}},
        {$match: {"key.pipeline.$match.$or": {$eq: [{'bar': '###', 'foo': '###'}]}}}
    ],
    cursor: {}
});
assert.gt(telStore.cursor.firstBatch[0].metrics.keysScanned.sum, 0);

MongoRunner.stopMongod(conn);
}());
