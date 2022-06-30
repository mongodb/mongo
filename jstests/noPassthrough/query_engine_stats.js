/**
 * Tests that the query engine used is recorded correctly in the logs, system.profile, and
 * serverStatus.
 */

(function() {
"use strict";

load("jstests/libs/profiler.js");  // For 'getLatestProfilerEntry()'.
load("jstests/libs/sbe_util.js");  // For 'checkSBEEnabled()'.
load("jstests/libs/log.js");       // For 'verifySlowQueryLog()'.

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB(jsTestName());

// This test assumes that SBE is being used for most queries.
if (!checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    jsTestLog("Skipping test because SBE is not fully enabled");
    MongoRunner.stopMongod(conn);
    return;
}

assert.commandWorked(db.dropDatabase());

const coll = db.collection;

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

const engine = {
    find: {sbe: "sbe", classic: "classic"},
    aggregate: {
        sbeHybrid: "sbeHybrid",
        classicHybrid: "classicHybrid",
        sbeOnly: "sbeOnly",
        classicOnly: "classicOnly"
    }
};

// Ensure the profile filter contains the correct information about the queryExecutionEngine used.
function verifyProfiler(expectedComment, execEngine) {
    const profileEntryFilter = {ns: "query_engine_stats.collection"};
    const profileObj = getLatestProfilerEntry(db, profileEntryFilter);
    try {
        assert.eq(profileObj.command.comment, expectedComment);
        if (execEngine) {
            assert.eq(profileObj.queryExecutionEngine, execEngine);
        }
    } catch (e) {
        print('failed to find [{ "queryExecutionEngine" : "' + execEngine + '", { "comment" : "' +
              expectedComment + '"} }] in the latest profiler entry.');
        throw (e);
    }
}

// Create an object with the correct queryExecutionEngine counter values after the specified type of
// query.
function generateExpectedCounters(execEngine) {
    let counters = db.serverStatus().metrics.query.queryExecutionEngine;
    assert(counters, "counters did not exist");
    let expected = Object.assign(counters);
    switch (execEngine) {
        case engine.find.sbe:
            expected.find.sbe = NumberLong(expected.find.sbe + 1);
            break;
        case engine.find.classic:
            expected.find.classic = NumberLong(expected.find.classic + 1);
            break;
        case engine.aggregate.sbeOnly:
            expected.aggregate.sbeOnly = NumberLong(expected.aggregate.sbeOnly + 1);
            break;
        case engine.aggregate.classicOnly:
            expected.aggregate.classicOnly = NumberLong(expected.aggregate.classicOnly + 1);
            break;
        case engine.aggregate.sbeHybrid:
            expected.aggregate.sbeHybrid = NumberLong(expected.aggregate.sbeHybrid + 1);
            break;
        case engine.aggregate.classicHybrid:
            expected.aggregate.classicHybrid = NumberLong(expected.aggregate.classicHybrid + 1);
            break;
    }
    return expected;
}

// Compare the values of the queryExecutionEngine counters to an object that represents the expected
// values.
function compareQueryEngineCounters(expectedCounters) {
    let counters = db.serverStatus().metrics.query.queryExecutionEngine;
    assert.docEq(counters, expectedCounters);
}

// Start with SBE off.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}));

// Run a find command.
let expectedCounters = generateExpectedCounters(engine.find.classic);
let queryComment = "findSbeOff";
assert.eq(coll.find({a: 3}).comment(queryComment).itcount(), 1);
verifySlowQueryLog(db, queryComment, engine.find.classic);
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, engine.find.classic);

// Run an aggregation that doesn't use DocumentSource.
expectedCounters = generateExpectedCounters(engine.aggregate.classicOnly);
queryComment = "aggSbeOff";
assert.eq(coll.aggregate([{$match: {b: 1, c: 3}}], {comment: queryComment}).itcount(), 1);
verifySlowQueryLog(db, queryComment, engine.find.classic);
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, engine.find.classic);

// Run an aggregation that uses DocumentSource.
expectedCounters = generateExpectedCounters(engine.aggregate.classicHybrid);
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
verifySlowQueryLog(db, queryComment, engine.find.classic);
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, engine.find.classic);

// Turn SBE on.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: false}));

// Run a find command.
expectedCounters = generateExpectedCounters(engine.find.sbe);
queryComment = "findSbeOn";
assert.eq(coll.find({a: 3}).comment(queryComment).itcount(), 1);
verifySlowQueryLog(db, queryComment, engine.find.sbe);
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, engine.find.sbe);

// Run an aggregation that doesn't use DocumentSource.
expectedCounters = generateExpectedCounters(engine.aggregate.sbeOnly);
queryComment = "aggSbeOn";
assert.eq(coll.aggregate([{$match: {b: 1, c: 3}}], {comment: queryComment}).itcount(), 1);
verifySlowQueryLog(db, queryComment, engine.find.sbe);
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, engine.find.sbe);

// Run an aggregation that uses DocumentSource.
expectedCounters = generateExpectedCounters(engine.aggregate.sbeHybrid);
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
verifySlowQueryLog(db, queryComment, engine.find.sbe);
compareQueryEngineCounters(expectedCounters);
verifyProfiler(queryComment, engine.find.sbe);

MongoRunner.stopMongod(conn);
})();
