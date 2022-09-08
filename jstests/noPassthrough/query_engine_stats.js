/**
 * Tests that the query engine used is recorded correctly in the logs, system.profile, and
 * serverStatus.
 */

(function() {
"use strict";

load("jstests/libs/profiler.js");  // For 'getLatestProfilerEntry()'.
load("jstests/libs/sbe_util.js");  // For 'checkSBEEnabled()'.

let conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod was unable to start up");

let db = conn.getDB(jsTestName());

// This test assumes that SBE is being used for most queries.
if (!checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    jsTestLog("Skipping test because SBE is not fully enabled");
    MongoRunner.stopMongod(conn);
    return;
}

assert.commandWorked(db.dropDatabase());

let coll = db.collection;

// Set logLevel to 1 so that all queries will be logged.
assert.commandWorked(db.setLogLevel(1));

// Set profiling level to profile all queries.
assert.commandWorked(db.setProfilingLevel(2));

// Set up the collection.
assert.commandWorked(coll.insertMany([
    {_id: 0, a: 1, b: 1, c: 1},
    {_id: 1, a: 2, b: 1, c: 2},
    {_id: 2, a: 3, b: 1, c: 3},
    {_id: 3, a: 4, b: 2, c: 4}
]));

const framework = {
    find: {sbe: "sbe", classic: "classic", cqf: "findCQF"},
    aggregate: {
        sbeHybrid: "sbeHybrid",
        classicHybrid: "classicHybrid",
        sbeOnly: "sbeOnly",
        classicOnly: "classicOnly",
        cqf: "aggCQF"
    }
};

// Ensure the slow query log contains the correct information about the queryFramework used.
function verifySlowQueryLog(db, expectedComment, queryFramework) {
    const logId = 51803;  // ID for 'Slow Query' commands
    const expectedLog = {command: {comment: expectedComment}};
    if (queryFramework) {
        expectedLog.queryFramework = queryFramework;
    }
    assert(checkLog.checkContainsWithCountJson(db, logId, expectedLog, 1, null, true),
           "failed to find [" + tojson(expectedLog) + "] in the slow query log");
}

// Ensure the profile filter contains the correct information about the queryFramework used.
function verifyProfiler(expectedComment, queryFramework) {
    const profileEntryFilter = {ns: "query_engine_stats.collection"};
    const profileObj = getLatestProfilerEntry(db, profileEntryFilter);
    try {
        assert.eq(profileObj.command.comment, expectedComment);
        if (queryFramework) {
            assert.eq(profileObj.queryFramework, queryFramework);
        }
    } catch (e) {
        print('failed to find [{ "queryFramework" : "' + queryFramework + '", { "comment" : "' +
              expectedComment + '"} }] in the latest profiler entry.');
        throw (e);
    }
}

// Create an object with the correct queryFramework counter values after the specified type of
// query.
function generateExpectedCounters(queryFramework) {
    let counters = db.serverStatus().metrics.query.queryFramework;
    assert(counters, "counters did not exist");
    let expected = Object.assign(counters);
    switch (queryFramework) {
        case framework.find.sbe:
            expected.find.sbe = NumberLong(expected.find.sbe + 1);
            break;
        case framework.find.classic:
            expected.find.classic = NumberLong(expected.find.classic + 1);
            break;
        case framework.find.cqf:
            expected.find.cqf = NumberLong(expected.find.cqf + 1);
            break;
        case framework.aggregate.sbeOnly:
            expected.aggregate.sbeOnly = NumberLong(expected.aggregate.sbeOnly + 1);
            break;
        case framework.aggregate.classicOnly:
            expected.aggregate.classicOnly = NumberLong(expected.aggregate.classicOnly + 1);
            break;
        case framework.aggregate.sbeHybrid:
            expected.aggregate.sbeHybrid = NumberLong(expected.aggregate.sbeHybrid + 1);
            break;
        case framework.aggregate.classicHybrid:
            expected.aggregate.classicHybrid = NumberLong(expected.aggregate.classicHybrid + 1);
            break;
        case framework.aggregate.cqf:
            expected.aggregate.cqf = NumberLong(expected.aggregate.cqf + 1);
            break;
    }
    return expected;
}

// Compare the values of the queryFramework counters to an object that represents the expected
// values.
function compareQueryEngineCounters(expectedCounters) {
    let counters = db.serverStatus().metrics.query.queryFramework;
    assert.docEq(counters, expectedCounters);
}

// Start with SBE off.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));

// Run a find command.
let expectedCounters = generateExpectedCounters(framework.find.classic);
let queryComment = "findSbeOff";
assert.eq(coll.find({a: 3}).comment(queryComment).itcount(), 1);
verifySlowQueryLog(db, queryComment, framework.find.classic);
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, framework.find.classic);

// Run an aggregation that doesn't use DocumentSource.
expectedCounters = generateExpectedCounters(framework.aggregate.classicOnly);
queryComment = "aggSbeOff";
assert.eq(coll.aggregate([{$match: {b: 1, c: 3}}], {comment: queryComment}).itcount(), 1);
verifySlowQueryLog(db, queryComment, framework.find.classic);
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, framework.find.classic);

// Run an aggregation that uses DocumentSource.
expectedCounters = generateExpectedCounters(framework.aggregate.classicHybrid);
queryComment = "docSourceSbeOff";
assert.eq(coll.aggregate(
                  [
                      {$_internalInhibitOptimization: {}},
                      {$group: {_id: "$a", acc: {$sum: "$b"}}},
                      {$match: {acc: 42}}
                  ],
                  {comment: queryComment})
              .itcount(),
          0);
verifySlowQueryLog(db, queryComment, framework.find.classic);
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, framework.find.classic);

// Turn SBE on.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

// Run a find command.
expectedCounters = generateExpectedCounters(framework.find.sbe);
queryComment = "findSbeOn";
assert.eq(coll.find({a: 3}).comment(queryComment).itcount(), 1);
verifySlowQueryLog(db, queryComment, framework.find.sbe);
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, framework.find.sbe);

// Run an aggregation that doesn't use DocumentSource.
expectedCounters = generateExpectedCounters(framework.aggregate.sbeOnly);
queryComment = "aggSbeOn";
assert.eq(coll.aggregate([{$match: {b: 1, c: 3}}], {comment: queryComment}).itcount(), 1);
verifySlowQueryLog(db, queryComment, framework.find.sbe);
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, framework.find.sbe);

// Run an aggregation that uses DocumentSource.
expectedCounters = generateExpectedCounters(framework.aggregate.sbeHybrid);
queryComment = "docSourceSbeOn";
assert.eq(coll.aggregate(
                  [
                      {$_internalInhibitOptimization: {}},
                      {$group: {_id: "$a", acc: {$sum: "$b"}}},
                      {$match: {acc: 42}}
                  ],
                  {comment: queryComment})
              .itcount(),
          0);
verifySlowQueryLog(db, queryComment, framework.find.sbe);
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, framework.find.sbe);

MongoRunner.stopMongod(conn);

conn = MongoRunner.runMongod({restart: conn, setParameter: 'featureFlagCommonQueryFramework=1'});
assert.neq(null, conn, "mongod was unable to start up");

db = conn.getDB(jsTestName());

assert.commandWorked(db.setLogLevel(1));
assert.commandWorked(db.setProfilingLevel(2));

coll = db.collection;

// Run find using CQF
expectedCounters = generateExpectedCounters(framework.find.cqf);
queryComment = "cqfFind";
assert.eq(coll.find({a: 1}).comment(queryComment).itcount(), 1);
verifySlowQueryLog(db, queryComment, "cqf");
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, "cqf");

// Run aggregate using CQF
expectedCounters = generateExpectedCounters(framework.aggregate.cqf);
queryComment = "cqfAggregate";
assert.eq(
    coll.aggregate([{$match: {a: 1}}, {$project: {_id: 0}}], {comment: queryComment}).itcount(), 1);
verifySlowQueryLog(db, queryComment, "cqf");
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, "cqf");

// Run CQF query which falls back to SBE (unsupported match expression)
expectedCounters = generateExpectedCounters(framework.find.sbe);
queryComment = "cqfFallback";
assert.eq(coll.find({a: {$mod: [4, 0]}}).comment(queryComment).itcount(), 1);
verifySlowQueryLog(db, queryComment, framework.find.sbe);
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, framework.find.sbe);

MongoRunner.stopMongod(conn);
})();
