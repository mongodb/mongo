/**
 * Test that the telemetry metrics are aggregated properly by distinct query shape over getMore
 * calls.
 * @tags: [featureFlagTelemetry]
 */
load("jstests/libs/telemetry_utils.js");  // For verifyMetrics.

(function() {
"use strict";

// Turn on the collecting of telemetry metrics.
let options = {
    setParameter: {internalQueryConfigureTelemetrySamplingRate: -1},
};

const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB('test');
var coll = testDB[jsTestName()];
coll.drop();

// Bulk insert documents to reduces roundtrips and make timeout on a slow machine less likely.
const bulk = coll.initializeUnorderedBulkOp();
const numDocs = 100;
for (let i = 0; i < numDocs / 2; ++i) {
    bulk.insert({foo: 0, bar: Math.floor(Math.random() * 3)});
    bulk.insert({foo: 1, bar: Math.floor(Math.random() * -2)});
}
assert.commandWorked(bulk.execute());

// Assert that two queries with identical structures are represented by the same key.
{
    // Note: toArray() is necessary for the batchSize-limited query to run to cursor exhaustion
    // (when it writes to the telemetry store).
    coll.aggregate([{$match: {foo: 1}}], {cursor: {batchSize: 2}}).toArray();
    coll.aggregate([{$match: {foo: 0}}], {cursor: {batchSize: 2}}).toArray();

    // This command will return all telemetry store entires.
    const telemetryResults = testDB.getSiblingDB("admin").aggregate([{$telemetry: {}}]).toArray();
    // Assert there is only one entry.
    assert.eq(telemetryResults.length, 1, telemetryResults);
    const telemetryEntry = telemetryResults[0];
    assert.eq(telemetryEntry.key.namespace, `test.${jsTestName()}`);
    assert.eq(telemetryEntry.key.applicationName, "MongoDB Shell");

    // Assert we update execution count for identically shaped queries.
    assert.eq(telemetryEntry.metrics.execCount, 2);

    // Assert telemetry values are accurate for the two above queries.
    assert.eq(telemetryEntry.metrics.docsReturned.sum, numDocs);
    assert.eq(telemetryEntry.metrics.docsReturned.min, numDocs / 2);
    assert.eq(telemetryEntry.metrics.docsReturned.max, numDocs / 2);

    verifyMetrics(telemetryResults);
}

const fooEqBatchSize = 5;
const fooNeBatchSize = 3;
// Assert on batchSize-limited queries that killCursors will write metrics with partial results to
// the telemetry store.
{
    let cursor1 = coll.find({foo: {$eq: 0}}).batchSize(fooEqBatchSize);
    let cursor2 = coll.find({foo: {$ne: 0}}).batchSize(fooNeBatchSize);
    // Issue one getMore for the first query, so 2 * fooEqBatchSize documents are returned total.
    assert.commandWorked(testDB.runCommand(
        {getMore: cursor1.getId(), collection: coll.getName(), batchSize: fooEqBatchSize}));

    // Kill both cursors so the telemetry metrics are stored.
    assert.commandWorked(testDB.runCommand(
        {killCursors: coll.getName(), cursors: [cursor1.getId(), cursor2.getId()]}));

    // This filters telemetry entires to just the ones entered when running above find queries.
    const telemetryResults = testDB.getSiblingDB("admin")
                                 .aggregate([
                                     {$telemetry: {}},
                                     {$match: {"key.queryShape.filter.foo": {$exists: true}}},
                                     {$sort: {key: 1}},
                                 ])
                                 .toArray();
    assert.eq(telemetryResults.length, 2, telemetryResults);
    assert.eq(telemetryResults[0].key.queryShape.cmdNs.db, "test");
    assert.eq(telemetryResults[0].key.queryShape.cmdNs.coll, jsTestName());
    assert.eq(telemetryResults[0].key.applicationName, "MongoDB Shell");
    assert.eq(telemetryResults[1].key.queryShape.cmdNs.db, "test");
    assert.eq(telemetryResults[1].key.queryShape.cmdNs.coll, jsTestName());
    assert.eq(telemetryResults[1].key.applicationName, "MongoDB Shell");

    assert.eq(telemetryResults[0].metrics.execCount, 1);
    assert.eq(telemetryResults[1].metrics.execCount, 1);
    assert.eq(telemetryResults[0].metrics.docsReturned.sum, fooEqBatchSize * 2);
    assert.eq(telemetryResults[1].metrics.docsReturned.sum, fooNeBatchSize);

    verifyMetrics(telemetryResults);
}

// Assert that options such as limit/sort create different keys, and that repeating a query shape
// ({foo: {$eq}}) aggregates metrics across executions.
{
    const query2Limit = 50;
    coll.find({foo: {$eq: 0}}).batchSize(2).toArray();
    coll.find({foo: {$eq: 1}}).limit(query2Limit).batchSize(2).toArray();
    coll.find().sort({"foo": 1}).batchSize(2).toArray();
    // This filters telemetry entires to just the ones entered when running above find queries.
    let telemetryResults =
        testDB.getSiblingDB("admin")
            .aggregate([{$telemetry: {}}, {$match: {"key.queryShape.find": {$exists: true}}}])
            .toArray();
    assert.eq(telemetryResults.length, 4, telemetryResults);

    verifyMetrics(telemetryResults);

    // This filters to just the telemetry for query coll.find().sort({"foo": 1}).batchSize(2).
    telemetryResults = testDB.getSiblingDB("admin")
                           .aggregate([{$telemetry: {}}, {$match: {"key.queryShape.sort.foo": 1}}])
                           .toArray();
    assert.eq(telemetryResults.length, 1, telemetryResults);
    assert.eq(telemetryResults[0].key.queryShape.cmdNs.db, "test");
    assert.eq(telemetryResults[0].key.queryShape.cmdNs.coll, jsTestName());
    assert.eq(telemetryResults[0].key.applicationName, "MongoDB Shell");
    assert.eq(telemetryResults[0].metrics.execCount, 1);
    assert.eq(telemetryResults[0].metrics.docsReturned.sum, numDocs);

    // This filters to just the telemetry for query coll.find({foo: {$eq:
    // 1}}).limit(query2Limit).batchSize(2).
    telemetryResults =
        testDB.getSiblingDB("admin")
            .aggregate([{$telemetry: {}}, {$match: {"key.queryShape.limit": '?number'}}])
            .toArray();
    assert.eq(telemetryResults.length, 1, telemetryResults);
    assert.eq(telemetryResults[0].key.queryShape.cmdNs.db, "test");
    assert.eq(telemetryResults[0].key.queryShape.cmdNs.coll, jsTestName());
    assert.eq(telemetryResults[0].key.applicationName, "MongoDB Shell");
    assert.eq(telemetryResults[0].metrics.execCount, 1);
    assert.eq(telemetryResults[0].metrics.docsReturned.sum, query2Limit);

    // This filters to just the telemetry for query coll.find({foo: {$eq: 0}}).batchSize(2).
    telemetryResults = testDB.getSiblingDB("admin")
                           .aggregate([
                               {$telemetry: {}},
                               {
                                   $match: {
                                       "key.queryShape.filter.foo": {$eq: {$eq: "?number"}},
                                       "key.queryShape.limit": {$exists: false},
                                       "key.queryShape.sort": {$exists: false}
                                   }
                               }
                           ])
                           .toArray();
    assert.eq(telemetryResults.length, 1, telemetryResults);
    assert.eq(telemetryResults[0].key.queryShape.cmdNs.db, "test");
    assert.eq(telemetryResults[0].key.queryShape.cmdNs.coll, jsTestName());
    assert.eq(telemetryResults[0].key.applicationName, "MongoDB Shell");
    assert.eq(telemetryResults[0].metrics.execCount, 2);
    assert.eq(telemetryResults[0].metrics.docsReturned.sum, numDocs / 2 + 2 * fooEqBatchSize);
    assert.eq(telemetryResults[0].metrics.docsReturned.max, numDocs / 2);
    assert.eq(telemetryResults[0].metrics.docsReturned.min, 2 * fooEqBatchSize);
}

MongoRunner.stopMongod(conn);
}());
