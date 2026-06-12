/**
 * Confirms that 'planCacheShapeHash' and 'planCacheKey' are correctly reported across a variety
 * of commands and outputs.
 *
 * @tags: [
 *   requires_profiling,
 *   featureFlagSbeFull
 * ]
 */

import {findMatchingLogLine} from "jstests/libs/log.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {getPlanCacheKeyFromShape} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB("plan_cache_key_reporting");
const coll = db.coll;
assert.commandWorked(db.createCollection(coll.getName()));

function setupCollection() {
    assert(coll.drop());
    assert.commandWorked(coll.createIndexes([{a: 1}, {a: -1}]));
}

function assertHasPlanCacheKeys(obj) {
    assert.eq(typeof obj.planCacheShapeHash, "string", obj);
    assert.gt(obj.planCacheShapeHash.length, 0, obj);
    assert.eq(typeof obj.planCacheKey, "string", obj);
    assert.gt(obj.planCacheKey.length, 0, obj);
}

// Validate that 'planCacheShapeHash' and 'planCacheKey' are correctly reported in the
// system.profile collection.
(function validateProfilerOutput() {
    setupCollection();
    db.setProfilingLevel(2);

    const comment = "validateProfilerOutput";
    // Run twice so the second execution recovers the plan from cache and reports 'planCacheKey'.
    [...Array(2)].forEach(() =>
        assert.eq(0, coll.aggregate([{$match: {a: 1}}], {comment}).itcount()),
    );

    db.setProfilingLevel(0);
    const entry = getLatestProfilerEntry(db, {"command.comment": comment});
    assert.neq(entry, null);
    assertHasPlanCacheKeys(entry);
})();

// Validate that 'planCacheShapeHash' and 'planCacheKey' are correctly reported in explain output.
(function validateExplainOutput() {
    setupCollection();
    const explain = coll.explain().aggregate([{$match: {a: 1}}]);
    assert.neq(explain, null);
    assertHasPlanCacheKeys(explain.queryPlanner);
})();

// Validate that 'planCacheShapeHash' and 'planCacheKey' are correctly reported in
// $planCacheStats output.
(function validatePlanCacheStatsOutput() {
    setupCollection();
    const query = {a: 1};
    // Run the test command once. We're not interested whether the cached entry is active or
    // not, but rather if it's present in the cache.
    assert.eq(0, coll.aggregate([{$match: query}]).itcount());

    const planCacheKey = getPlanCacheKeyFromShape({query, collection: coll, db});
    const allPlanCacheEntries = coll
        .aggregate([{$planCacheStats: {}}, {$match: {planCacheKey}}])
        .toArray();
    assert.eq(allPlanCacheEntries.length, 1, allPlanCacheEntries);
    const entry = allPlanCacheEntries[0];
    assert.eq(entry.version, "1", entry);
    assertHasPlanCacheKeys(entry);
})();

// Validate that 'planCacheShapeHash' and 'planCacheKey' are correctly reported in log output for
// slow queries.
(function validateSlowQueryLogOutput() {
    setupCollection();
    // Set all logging parameters: disable profiling and log all operations at the default
    // logLevel.
    assert.commandWorked(db.setProfilingLevel(0, {slowms: -1}));
    assert.commandWorked(db.setLogLevel(0, "command"));

    // Clear the log before running the test, to guarantee that we do not match against any
    // similar tests which may have run previously.
    assert.commandWorked(db.adminCommand({clearLog: "global"}));

    // Run an aggregate command in order to generate a log line.
    const pipeline = [{$match: {a: 10}}];
    const comment = "validateSlowQueryLogOutput";
    assert.eq(0, coll.aggregate(pipeline, {comment}).itcount());

    // Confirm whether the operation was logged or not.
    const logFields = {command: "aggregate", aggregate: coll.getName(), pipeline, comment};
    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    const logEntry = JSON.parse(findMatchingLogLine(globalLog.log, logFields));
    assert.neq(logEntry, null);
    assertHasPlanCacheKeys(logEntry.attr);
})();

// Validate that a query with a $lookup stage correctly reports plan cache keys.
(function validateLookupPlanCacheShapeHashMap() {
    const lookupColl = db.lookupColl;
    lookupColl.drop();
    assert.commandWorked(lookupColl.createIndex({b: 1}));
    setupCollection();

    const pipeline = [
        {
            $lookup: {
                from: lookupColl.getName(),
                localField: "a",
                foreignField: "b",
                as: "whatever", // B)
            },
        },
    ];
    const explain = coll.explain().aggregate(pipeline);
    assert.neq(explain, null);
    assertHasPlanCacheKeys(explain.queryPlanner);
})();

// Validate that a query with a $group stage correctly reports plan cache keys.
(function validateGroupPlanCacheShapeHashMap() {
    const groupColl = db.groupColl;
    groupColl.drop();
    assert.commandWorked(groupColl.insertOne({b: 1}));

    const pipeline = [
        {
            $group: {
                _id: "$b",
            },
        },
    ];
    const explain = groupColl.explain().aggregate(pipeline);
    assert.neq(explain, null);
    assertHasPlanCacheKeys(explain.queryPlanner);
})();

MongoRunner.stopMongod(conn);
