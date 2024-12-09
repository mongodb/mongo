/**
 * Test that the queryStats metrics are aggregated properly by distinct query shape over getMore
 * calls, for agg commands.
 * @tags: [requires_fcv_60]
 */
load("jstests/libs/query_stats_utils.js");  // For verifyMetrics and getQueryStatsAggCmd.

(function() {
"use strict";

// Turn on the collecting of queryStats metrics.
let options = {
    setParameter: {internalQueryStatsRateLimit: -1},
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
    // (when it writes to the queryStats store).
    coll.aggregate([{$match: {foo: 1}}], {cursor: {batchSize: 2}}).toArray();
    coll.aggregate([{$match: {foo: 0}}], {cursor: {batchSize: 2}}).toArray();

    // This command will return all queryStats store entires.
    const queryStatsResults = getQueryStatsAggCmd(testDB);
    // Assert there is only one entry.
    assert.eq(queryStatsResults.length, 1, queryStatsResults);
    const queryStatsEntry = queryStatsResults[0];
    assert.eq(queryStatsEntry.key.queryShape.cmdNs.db, "test");
    assert.eq(queryStatsEntry.key.queryShape.cmdNs.coll, jsTestName());
    assert.eq(queryStatsEntry.key.client.application.name, "MongoDB Shell");

    // Assert we update execution count for identically shaped queries.
    assert.eq(queryStatsEntry.metrics.execCount, 2);

    // Assert queryStats values are accurate for the two above queries.
    assert.eq(queryStatsEntry.metrics.docsReturned.sum, numDocs);
    assert.eq(queryStatsEntry.metrics.docsReturned.min, numDocs / 2);
    assert.eq(queryStatsEntry.metrics.docsReturned.max, numDocs / 2);

    verifyMetrics(queryStatsResults);
}

const fooEqBatchSize = 5;
const fooNeBatchSize = 3;
// Assert on batchSize-limited queries that killCursors will write metrics with partial results to
// the queryStats store.
{
    let cursor1 = coll.find({foo: {$eq: 0}}).batchSize(fooEqBatchSize);
    let cursor2 = coll.find({foo: {$ne: 0}}).batchSize(fooNeBatchSize);
    // Issue one getMore for the first query, so 2 * fooEqBatchSize documents are returned total.
    assert.commandWorked(testDB.runCommand(
        {getMore: cursor1.getId(), collection: coll.getName(), batchSize: fooEqBatchSize}));

    // Kill both cursors so the queryStats metrics are stored.
    assert.commandWorked(testDB.runCommand(
        {killCursors: coll.getName(), cursors: [cursor1.getId(), cursor2.getId()]}));

    // This filters queryStats entires to just the ones entered when running above find queries.
    const queryStatsResults = testDB.getSiblingDB("admin")
                                  .aggregate([
                                      {$queryStats: {}},
                                      {$match: {"key.queryShape.filter.foo": {$exists: true}}},
                                      {$sort: {key: 1}},
                                  ])
                                  .toArray();
    assert.eq(queryStatsResults.length, 2, queryStatsResults);
    assert.eq(queryStatsResults[0].key.queryShape.cmdNs.db, "test");
    assert.eq(queryStatsResults[0].key.queryShape.cmdNs.coll, jsTestName());
    assert.eq(queryStatsResults[0].key.client.application.name, "MongoDB Shell");
    assert.eq(queryStatsResults[1].key.queryShape.cmdNs.db, "test");
    assert.eq(queryStatsResults[1].key.queryShape.cmdNs.coll, jsTestName());
    assert.eq(queryStatsResults[1].key.client.application.name, "MongoDB Shell");

    assert.eq(queryStatsResults[0].metrics.execCount, 1);
    assert.eq(queryStatsResults[1].metrics.execCount, 1);
    assert.eq(queryStatsResults[0].metrics.docsReturned.sum, fooEqBatchSize * 2);
    assert.eq(queryStatsResults[1].metrics.docsReturned.sum, fooNeBatchSize);

    verifyMetrics(queryStatsResults);

    const distributionFields = ['sum', 'max', 'min', 'sumOfSquares'];
    for (const field of distributionFields) {
        // If there are getMore calls, queryExecMicros should be greater than or equal to
        // firstResponseExecMicros.
        assert(bsonWoCompare(queryStatsResults[0].metrics.totalExecMicros[field],
                             queryStatsResults[0].metrics.firstResponseExecMicros[field]) > 0);

        // If there is no getMore calls, firstResponseExecMicros and queryExecMicros should be
        // equal.
        assert.eq(queryStatsResults[1].metrics.totalExecMicros[field],
                  queryStatsResults[1].metrics.firstResponseExecMicros[field]);
    }
}

// Assert that options such as limit/sort create different keys, and that repeating a query shape
// ({foo: {$eq}}) aggregates metrics across executions.
{
    const query2Limit = 50;
    coll.find({foo: {$eq: 0}}).batchSize(2).toArray();
    coll.find({foo: {$eq: 1}}).limit(query2Limit).batchSize(2).toArray();
    coll.find().sort({"foo": 1}).batchSize(2).toArray();
    // This filters queryStats entires to just the ones entered when running above find queries.
    let queryStatsResults =
        testDB.getSiblingDB("admin")
            .aggregate([{$queryStats: {}}, {$match: {"key.queryShape.command": "find"}}])
            .toArray();
    assert.eq(queryStatsResults.length, 4, queryStatsResults);

    verifyMetrics(queryStatsResults);

    // This filters to just the queryStats for query coll.find().sort({"foo": 1}).batchSize(2).
    queryStatsResults =
        testDB.getSiblingDB("admin")
            .aggregate([{$queryStats: {}}, {$match: {"key.queryShape.sort.foo": 1}}])
            .toArray();
    assert.eq(queryStatsResults.length, 1, queryStatsResults);
    assert.eq(queryStatsResults[0].key.queryShape.cmdNs.db, "test");
    assert.eq(queryStatsResults[0].key.queryShape.cmdNs.coll, jsTestName());
    assert.eq(queryStatsResults[0].key.client.application.name, "MongoDB Shell");
    assert.eq(queryStatsResults[0].metrics.execCount, 1);
    assert.eq(queryStatsResults[0].metrics.docsReturned.sum, numDocs);

    // This filters to just the queryStats for query coll.find({foo: {$eq:
    // 1}}).limit(query2Limit).batchSize(2).
    queryStatsResults =
        testDB.getSiblingDB("admin")
            .aggregate([{$queryStats: {}}, {$match: {"key.queryShape.limit": '?number'}}])
            .toArray();
    assert.eq(queryStatsResults.length, 1, queryStatsResults);
    assert.eq(queryStatsResults[0].key.queryShape.cmdNs.db, "test");
    assert.eq(queryStatsResults[0].key.queryShape.cmdNs.coll, jsTestName());
    assert.eq(queryStatsResults[0].key.client.application.name, "MongoDB Shell");
    assert.eq(queryStatsResults[0].metrics.execCount, 1);
    assert.eq(queryStatsResults[0].metrics.docsReturned.sum, query2Limit);

    // This filters to just the queryStats for query coll.find({foo: {$eq: 0}}).batchSize(2).
    queryStatsResults = testDB.getSiblingDB("admin")
                            .aggregate([
                                {$queryStats: {}},
                                {
                                    $match: {
                                        "key.queryShape.filter.foo": {$eq: {$eq: "?number"}},
                                        "key.queryShape.limit": {$exists: false},
                                        "key.queryShape.sort": {$exists: false}
                                    }
                                }
                            ])
                            .toArray();
    assert.eq(queryStatsResults.length, 1, queryStatsResults);
    assert.eq(queryStatsResults[0].key.queryShape.cmdNs.db, "test");
    assert.eq(queryStatsResults[0].key.queryShape.cmdNs.coll, jsTestName());
    assert.eq(queryStatsResults[0].key.client.application.name, "MongoDB Shell");
    assert.eq(queryStatsResults[0].metrics.execCount, 2);
    assert.eq(queryStatsResults[0].metrics.docsReturned.sum, numDocs / 2 + 2 * fooEqBatchSize);
    assert.eq(queryStatsResults[0].metrics.docsReturned.max, numDocs / 2);
    assert.eq(queryStatsResults[0].metrics.docsReturned.min, 2 * fooEqBatchSize);
}

MongoRunner.stopMongod(conn);
}());
