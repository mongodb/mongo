/**
 * Confirms that the SBE and classic engines use the correct format to report 'planCacheShapeHash'
 * and 'planCacheKey' values across a variety of commands and outputs.
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

// Drop and recreates a test collection along with two indexes: {a: 1} and {a: 1, b: 1}.
function setupCollection() {
    assert(coll.drop());
    assert.commandWorked(coll.createIndexes([{a: 1}, {a: -1}]));
}

// Runs the given 'testToRun' twice using the SBE and classic engine and returns the result of each
// run as an array with two elements: first for SBE, second for classic.
//
// Calls 'setupCollection' before each run.
function runTestAgainstSbeAndClassicEngines(testToRun) {
    return ["trySbeEngine", "forceClassicEngine"].map((engine) => {
        setupCollection();
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: engine}));
        return testToRun(engine);
    });
}

// Verifies and asserts that the two objects 'sbe' and 'classic', which represent the result of
// execution of some command agains the SBE and classic engine respectively, contain the
// 'planCacheShapeHash' and 'planCacheKey' attributes, and the values of those attribuets are
// different.
//
// The purpose of this assertion is to ensure that the SBE and classic engines use the correct
// 'planCacheShapeHash' and 'planCacheKey' formats, which are different between the two engines.
function assertPlanCacheShapeHashAndPlanCacheKey(sbe, classic) {
    assert.eq(typeof sbe.planCacheShapeHash, "string", sbe);
    assert.gt(sbe.planCacheShapeHash.length, 0, sbe);
    assert.eq(typeof sbe.planCacheKey, "string", sbe);
    assert.gt(sbe.planCacheKey.length, 0, sbe);

    assert.eq(typeof classic.planCacheShapeHash, "string", classic);
    assert.gt(classic.planCacheShapeHash.length, 0, classic);
    assert.eq(typeof classic.planCacheKey, "string", classic);
    assert.gt(classic.planCacheKey.length, 0, classic);

    assert.neq(sbe.planCacheShapeHash, classic.planCacheShapeHash, `sbe=${tojson(sbe)}, classic=${tojson(classic)}`);
    assert.neq(sbe.planCacheKey, classic.planCacheKey, `sbe=${tojson(sbe)}, classic=${tojson(classic)}`);
}

// Validate the the 'planCacheShapeHash' and 'planCacheKey' are correctly reported in the
// system.profile collection.
(function validateProfilerOutput() {
    const [sbe, classic] = runTestAgainstSbeAndClassicEngines(function (engine) {
        // Set db profiling level to collect data for all operations.
        db.setProfilingLevel(2);

        const comment = "validateProfilerOutput-${engine}";
        // We need to run the query twice in order to recover the plan from the cache and report
        // the 'planCacheKey'.
        [...Array(2)].forEach(() => assert.eq(0, coll.aggregate([{$match: {a: 1}}], {comment}).itcount()));

        // Disable profiling and find the matching profiler entry for the command above.
        db.setProfilingLevel(0);
        return getLatestProfilerEntry(db, {"command.comment": comment});
    });

    assert.neq(sbe, null);
    assert.neq(classic, null);
    assertPlanCacheShapeHashAndPlanCacheKey(sbe, classic);
})();

// Validate the the 'planCacheShapeHash' and 'planCacheKey' are correctly reported in explain
// output.
(function validateExplainOutput() {
    const [sbe, classic] = runTestAgainstSbeAndClassicEngines(function (engine) {
        return coll.explain().aggregate([{$match: {a: 1}}]);
    });

    assert.neq(sbe, null);
    assert.neq(classic, null);
    assert.eq(sbe.explainVersion, "2", sbe);
    assert.eq(classic.explainVersion, "1", classic);

    assertPlanCacheShapeHashAndPlanCacheKey(sbe.queryPlanner, classic.queryPlanner);
})();

// Validate the the 'planCacheShapeHash' and 'planCacheKey' are correctly reported in
// $planCacheStats output.
(function validatePlanCacheStatsOutput() {
    const [sbe, classic] = runTestAgainstSbeAndClassicEngines(function (engine) {
        const query = {a: 1};
        // Run the test command once. We're not interested whether the cached entry is active or
        // not, but rather if it's present in the cache.
        assert.eq(0, coll.aggregate([{$match: query}]).itcount());

        const planCacheKey = getPlanCacheKeyFromShape({query, collection: coll, db});
        const allPlanCacheEntries = coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey}}]).toArray();
        assert.eq(allPlanCacheEntries.length, 1, allPlanCacheEntries);
        return allPlanCacheEntries[0];
    });

    assert.neq(sbe, null);
    assert.neq(classic, null);
    assert.eq(sbe.version, "2", sbe);
    assert.eq(classic.version, "1", classic);

    assertPlanCacheShapeHashAndPlanCacheKey(sbe, classic);
})();

// Validate the the 'planCacheShapeHash' and 'planCacheKey' are correctly reported in log output for
// slow queries.
(function validateSlowQueryLogOutput() {
    const [sbe, classic] = runTestAgainstSbeAndClassicEngines(function (engine) {
        // Set all logging parameters: disable profiling and log all operations at the default
        // logLevel.
        assert.commandWorked(db.setProfilingLevel(0, {slowms: -1}));
        assert.commandWorked(db.setLogLevel(0, "command"));

        // Clear the log before running the test, to guarantee that we do not match against any
        // similar tests which may have run previously.
        assert.commandWorked(db.adminCommand({clearLog: "global"}));

        // Run an aggregate command in order to generate a log line.
        const pipeline = [{$match: {a: 10}}];
        const comment = `validateSlowQueryLogOutput-${engine}`;
        assert.eq(0, coll.aggregate(pipeline, {comment}).itcount());

        // Confirm whether the operation was logged or not.
        const logFields = {command: "aggregate", aggregate: coll.getName(), pipeline, comment};
        const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
        return JSON.parse(findMatchingLogLine(globalLog.log, logFields));
    });

    assert.neq(sbe, null);
    assert.neq(classic, null);
    assertPlanCacheShapeHashAndPlanCacheKey(sbe.attr, classic.attr);
})();

// Validate that a query with pushed down $lookup stage uses classic plan cache key encoding.
(function validateLookupPlanCacheShapeHashMap() {
    const lookupColl = db.lookupColl;
    lookupColl.drop();
    assert.commandWorked(lookupColl.createIndex({b: 1}));
    const [sbe, classic] = runTestAgainstSbeAndClassicEngines(function (engine) {
        const pipeline = [
            {
                $lookup: {
                    from: lookupColl.getName(),
                    localField: "a",
                    foreignField: "b",
                    as: "whatever",
                },
            },
        ];
        return coll.explain().aggregate(pipeline);
    });

    assert.neq(sbe, null);
    assert.neq(classic, null);
    assert.eq(sbe.explainVersion, "2", sbe);
    assert.eq(classic.explainVersion, "1", classic);

    // The query hashes and the plan cache keys ('the keys') are different now because
    // 'internalQueryFrameworkControl' flag is encoded into query shape, once this
    // flag is removed from the query shape encoding the keys will be different.
    assertPlanCacheShapeHashAndPlanCacheKey(sbe.queryPlanner, classic.stages[0]["$cursor"].queryPlanner);
})();

// Validate that a query with pushed down $group stage uses classic plan cache key encoding.
(function validateGroupPlanCacheShapeHashMap() {
    const groupColl = db.groupColl;
    groupColl.drop();
    assert.commandWorked(groupColl.insertOne({b: 1}));
    const [sbe, classic] = runTestAgainstSbeAndClassicEngines(function (engine) {
        const pipeline = [
            {
                $group: {
                    _id: "$b",
                },
            },
        ];
        return groupColl.explain().aggregate(pipeline);
    });

    assert.neq(sbe, null);
    assert.neq(classic, null);
    assert.eq(sbe.explainVersion, "2", sbe);
    assert.eq(classic.explainVersion, "1", classic);

    // The query hashes and the plan cache keys ('the keys') are different now because
    // 'internalQueryFrameworkControl' flag is encoded into query shape, once this
    // flag is removed from the query shape encoding the keys will be different.
    assertPlanCacheShapeHashAndPlanCacheKey(sbe.queryPlanner, classic.stages[0]["$cursor"].queryPlanner);
})();

MongoRunner.stopMongod(conn);
