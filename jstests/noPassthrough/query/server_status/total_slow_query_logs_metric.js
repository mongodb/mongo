/**
 * Tests that the query.totalSlowQueryLogs serverStatus metric is incremented when a slow query
 * is logged.
 */

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB(jsTestName());
const coll = db.getCollection("test");

// Insert a document to query.
assert.commandWorked(coll.insert({_id: 1, a: 1}));

// Set slowms to -1 so all operations are logged as slow queries.
assert.commandWorked(db.setProfilingLevel(0, {slowms: -1}));

// Get the metric before running a query.
const metricBefore = db.serverStatus().metrics.query.totalSlowQueryLogs;

// Run a find query.
assert.eq(1, coll.find({a: 1}).itcount());

// Verify the metric was incremented.
const metricAfter = db.serverStatus().metrics.query.totalSlowQueryLogs;
assert.gt(
    metricAfter,
    metricBefore,
    "Expected metrics.query.totalSlowQueryLogs to increment after running a query with slowms=-1",
);

// Run additional queries to verify consistent incrementing.
const metricBeforeMultiple = db.serverStatus().metrics.query.totalSlowQueryLogs;
const numQueries = 5;
for (let i = 0; i < numQueries; i++) {
    assert.eq(1, coll.find({a: 1}).itcount());
}
const metricAfterMultiple = db.serverStatus().metrics.query.totalSlowQueryLogs;
assert.gte(
    metricAfterMultiple - metricBeforeMultiple,
    numQueries,
    `Expected metrics.query.totalSlowQueryLogs to increment by at least ${numQueries} after running ${numQueries} queries`,
);

MongoRunner.stopMongod(conn);
