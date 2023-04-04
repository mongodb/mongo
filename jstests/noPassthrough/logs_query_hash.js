/**
 * Tests that the queryHash and planCacheKey are logged correctly.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("jstests_logs_query_hash");

// Set logLevel to 1 so that all queries will be logged.
assert.commandWorked(db.setLogLevel(1));

// Set up collections foo and bar.
db.foo.drop();
assert.commandWorked(db.foo.insert({a: 1, b: 1, c: 1}));
assert.commandWorked(db.foo.createIndexes([{a: 1, b: 1}, {b: 1, a: 1}]));

db.bar.drop();
assert.commandWorked(db.bar.insert({a: 1, b: 1, c: 1}));
assert.commandWorked(db.bar.createIndexes([{a: 1, b: 1, c: 1}, {b: 1, a: 1, c: 1}]));

// Ensure the slow query log contains the same queryHash and planCacheKey as the explain output.
function runAndVerifySlowQueryLog(pipeline, commentObj, hint) {
    assert.eq(db.foo.aggregate(pipeline, commentObj, hint).itcount(), 1);
    let queryPlanner =
        db.foo.explain().aggregate(pipeline, commentObj, hint).stages[0].$cursor.queryPlanner;
    const logId = 51803;
    let expectedLog = {};
    expectedLog.command = {};
    expectedLog.command.cursor = {};
    expectedLog.command.comment = commentObj.comment;
    expectedLog.queryHash = queryPlanner.queryHash;
    expectedLog.planCacheKey = queryPlanner.planCacheKey;
    assert(checkLog.checkContainsWithCountJson(db, logId, expectedLog, 2, null, true),
           "failed to find [" + tojson(expectedLog) + "] in the slow query log");
}

const lookupStage = {
    "$lookup": {
        "from": "bar",
        "let": {"b": {"$ifNull": ["$b", null]}},
        "pipeline": [
            {"$match": {"$or": [{"a": {"$exists": false}}, {"a": 1}]}},
            {"$match": {"$expr": {"$eq": ["$b", "$$b"]}}}
        ],
        "as": "bar"
    }
};

runAndVerifySlowQueryLog([{"$match": {$or: [{a: 1}, {b: 1}]}}, lookupStage],
                         {"comment": "pipeline1"});
runAndVerifySlowQueryLog([{"$match": {a: {$in: [1, 2, 3, 4, 5]}}}, lookupStage],
                         {"comment": "pipeline2"});
runAndVerifySlowQueryLog([{"$match": {b: 1}}, lookupStage], {"comment": "pipeline3"});

const hint = {
    "hint": {"a": 1, "b": 1}
};

runAndVerifySlowQueryLog(
    [{"$match": {$or: [{a: 1}, {b: 1}]}}, lookupStage], {"comment": "pipelineWithHint1"}, hint);
runAndVerifySlowQueryLog(
    [{"$match": {a: {$in: [1, 2, 3, 4, 5]}}}, lookupStage], {"comment": "pipelineWithHint2"}, hint);
runAndVerifySlowQueryLog([{"$match": {b: 1}}, lookupStage], {"comment": "pipelineWithHint3"}, hint);

MongoRunner.stopMongod(conn);
})();
