/**
 * Test that the queryStats metrics are aggregated properly for queries run on a mongod where the
 * results fit into a single batch (and thus don't require a cursor), for find commands.
 * @tags: [requires_fcv_71]
 */
import {getQueryStatsFindCmd, verifyMetrics} from "jstests/libs/query/query_stats_utils.js";

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

// Note that toArray is necessary to guarantee the query finishes executing on the server (at
// which point an entry is finally written to the queryStats store).
coll.find({foo: {$eq: 0}}).toArray();
coll.find({foo: {$eq: 1}}).toArray();

// This command will return all queryStats store entires.
const queryStatsResults = getQueryStatsFindCmd(testDB);
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

// The total size of documents in the collection should ensure that the queries in this test can
// be executed without requiring multiple batches, but we verify that by looking at the
// timestamps.
assert.eq(queryStatsEntry.metrics.firstResponseExecMicros, queryStatsEntry.metrics.totalExecMicros);
verifyMetrics(queryStatsResults);

MongoRunner.stopMongod(conn);
