/**
 * Tests that the 'planCacheShapeHash' and 'planCacheKey' are logged correctly.
 */
import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("jstests_logs_plan_cache_shape_hash");

// Set slowms to 0 so that all queries will be logged.
assert.commandWorked(db.runCommand({profile: 0, slowms: 0}));

// Set up collections foo and bar.
db.foo.drop();
assert.commandWorked(db.foo.insert({a: 1, b: 1, c: 1}));
assert.commandWorked(db.foo.insert({d: 1}));
assert.commandWorked(db.foo.insert({d: 2}));
assert.commandWorked(db.foo.createIndexes([{a: 1, b: 1}, {b: 1, a: 1}]));

db.bar.drop();
assert.commandWorked(db.bar.insert({a: 1, b: 1, c: 1}));
assert.commandWorked(db.bar.createIndexes([{a: 1, b: 1, c: 1}, {b: 1, a: 1, c: 1}]));

function buildAggCommand(pipeline, comment, hint) {
    const command = {
        aggregate: "foo",
        pipeline,
        cursor: {},
        comment,
    };
    if (hint) {
        command.hint = hint;
    }
    return command;
}

function buildUpdateCommand(query, update, comment) {
    return {
        update: "foo",
        updates: [{
            q: query,
            u: update,
        }],
        comment,
    };
}

function buildDeleteCommand(query, comment) {
    return {
        delete: "foo",
        deletes: [{
            q: query,
            limit: 1,
        }],
        comment,
    };
}

function buildFindAndModifyCommand(query, updateOrRemove, comment) {
    return {
        findAndModify: "foo",
        query,
        ...updateOrRemove,
        comment,
    };
}

// Ensure the slow query log contains the same 'planCacheShapeHash' and 'planCacheKey' as the
// explain output.
function runAndVerifySlowQueryLog(command) {
    let isAgg = Object.keys(command)[0] == "aggregate";
    let result = db.runCommand(command);
    if (isAgg) {
        assert.eq(result.cursor.firstBatch.length, 1, result);
    }
    let explainCommand = isAgg ? {...command, explain: true} : {explain: command};
    let queryPlanner = getQueryPlanner(db.runCommand(explainCommand));
    const logId = 51803;
    let expectedLog = {};
    expectedLog.command = {};
    if (isAgg) {
        expectedLog.command.cursor = {};
    }
    expectedLog.command.comment = command.comment;
    expectedLog.planCacheShapeHash = queryPlanner.planCacheShapeHash;
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

// Aggregation $match-$lookup
runAndVerifySlowQueryLog(buildAggCommand(
    [{"$match": {$or: [{a: 1}, {b: 1}]}}, lookupStage] /* pipeline */, "pipeline1" /* comment */));
runAndVerifySlowQueryLog(
    buildAggCommand([{"$match": {a: {$in: [1, 2, 3, 4, 5]}}}, lookupStage] /* pipeline */,
                    "pipeline2" /* comment */));
runAndVerifySlowQueryLog(
    buildAggCommand([{"$match": {b: 1}}, lookupStage] /* pipeline */, "pipeline3" /* comment */));

// Aggregation $match-$lookup with hint
const hint = {
    "a": 1,
    "b": 1
};
runAndVerifySlowQueryLog(
    buildAggCommand([{"$match": {$or: [{a: 1}, {b: 1}]}}, lookupStage] /* pipeline */,
                    "pipelineWithHint1" /* comment */,
                    hint));
runAndVerifySlowQueryLog(
    buildAggCommand([{"$match": {a: {$in: [1, 2, 3, 4, 5]}}}, lookupStage] /* pipeline */,
                    "pipelineWithHint2" /* comment */,
                    hint));
runAndVerifySlowQueryLog(buildAggCommand(
    [{"$match": {b: 1}}, lookupStage] /* pipeline */, "pipelineWithHint3" /* comment */, hint));

// Update
runAndVerifySlowQueryLog(
    buildUpdateCommand({d: 1} /* query */, {$set: {d: 1}} /* update */, "update" /* comment */));

// Delete
runAndVerifySlowQueryLog(buildDeleteCommand({d: 1} /* query */, "delete" /* comment */));

// FindAndModify
runAndVerifySlowQueryLog(buildFindAndModifyCommand({d: 2} /* query */,
                                                   {update: {$set: {d: 2}}} /* updateOrRemove */,
                                                   "findAndModifyUpdate" /* comment */));
runAndVerifySlowQueryLog(buildFindAndModifyCommand(
    {d: 2} /* query */, {remove: true} /* updateOrRemove */, "findAndModifyRemove" /* comment */));

MongoRunner.stopMongod(conn);
