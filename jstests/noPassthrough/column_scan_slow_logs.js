/**
 * Tests to ensure that COLUMN_SCAN plan and scanned columns appear in slow query log lines when
 * the columstore index is the winning plan.
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For 'checkSBEEnabled()'.
load("jstests/libs/log.js");       // For 'verifySlowQueryLog()'.

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB(jsTestName());

const columnstoreEnabled =
    checkSBEEnabled(db, ["featureFlagColumnstoreIndexes", "featureFlagSbeFull"]);
if (!columnstoreEnabled) {
    jsTestLog("Skipping columnstore index validation test since the feature flag is not enabled.");
    MongoRunner.stopMongod(conn);
    return;
}

assert.commandWorked(db.dropDatabase());

const coll = db.collection;

// Set logLevel to 1 so that all queries will be logged.
assert.commandWorked(db.setLogLevel(1));

// Set profiling level to profile all queries.
// Additionally, set slow threshold to -1 to ensure that all operations are logged as SLOW.
assert.commandWorked(db.setProfilingLevel(2, {slowms: -1}));

assert.commandWorked(coll.createIndex({"$**": "columnstore"}));
const docs = [
    {_id: 0, x: 1, y: [{a: 2}, {a: 3}, {a: 4}]},
    {_id: 1, x: 1},
    {_id: 2, x: 2, y: [{b: 5}, {b: 6}, {b: 7}]},
    {_id: 3, x: 1, y: [{b: 5}, {b: [1, 2, {c: 5}]}, {c: 7}]},
    {_id: 4, x: 5, y: [{b: {c: 1}}]}
];
assert.commandWorked(coll.insertMany(docs));

const queryComment = 'findColumnScan';
coll.find({x: {$gt: 1}}, {_id: 1, x: 1}).comment(queryComment).toArray();

const relevantLog = JSON.parse(checkLog.getLogMessage(db, /\"planSummary\":\"COLUMN_SCAN/));
assert(relevantLog !== null);
assert.eq(relevantLog.id, 51803, 'Slow query log (id: 51803) not found.');
assert.eq(relevantLog.attr.command.comment,
          'findColumnScan',
          `Relevant query with comment '${queryComment}' not found.`);

// We expect a log line with a planSummary format like "COLUMN_SCAN
// {match:['x'],output:['_id','x']}". We assert only on the pieces (and frequency) here to avoid
// depending on the exact order.
const planSummary = relevantLog.attr.planSummary;
assert(planSummary.match(/COLUMN_SCAN/),
       `'COLUMN_SCAN' plan not found. Instead, got: ${planSummary}`);
assert(planSummary.match(/'match'/), `'match' not found. Instead, got: ${planSummary}`);
assert(planSummary.match(/'output'/), `'output' not found. Instead, got: ${planSummary}`);
assert.eq(planSummary.match(/'x'/g).length,
          2,
          `'x' should appear twice in planSummary. Instead, got: ${planSummary}`);
assert.eq(planSummary.match(/'_id'/g).length,
          1,
          `'_id' should appear once in planSummary. Instead, got: ${planSummary}`);

MongoRunner.stopMongod(conn);
}());
