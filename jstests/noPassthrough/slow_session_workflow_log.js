/**
 * Verifies that the SessionWorkflow provides a slow loop log when appropriate.
 * @tags: [
 *   requires_sharding,
 *   multiversion_incompatible
 * ]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/log.js");  // For findMatchingLogLine

const expectedLogId = 6983000;
const sleepMillisInQueryFilter = 200;
const sleepMillisBetweenQueries = 100;

/*
TODO(SERVER-69831): enable when we have slow loop crieteria
const expectedFields = [
    "totalElapsedMillis",
    "activeElapsedMillis",
    "sourceWorkElapsedMillis",
    "processMessageElapsedMillis",
    "sendMessageElapsedMillis",
    "finalizeElapsedMillis",
];
*/

function runTest(conn) {
    // Build a new connection based on the replica set URL
    let coll = conn.getCollection("test.foo");
    coll.drop();

    // TODO(SERVER-69831): remove, and actually test under which conditions the log appears.
    configureFailPoint(conn, "alwaysLogSlowSessionWorkflow");

    assert.commandWorked(assert.commandWorked(coll.insert({_id: 1})));

    // Do a query that we would expect to be fast.
    let count = coll.find({}).toArray();
    assert.eq(count.length, 1, "expected 1 document");

    // TODO(SERVER-69831): Expect no slow loop logs.

    // This sleep should show up as part of sourceWorkElapsedMillis.
    sleep(sleepMillisBetweenQueries);

    // Do a slow query beyond the 100ms threshold. Make sure the slow loop log line exists.
    count =
        coll.find({$where: 'function() { sleep(' + sleepMillisInQueryFilter + '); return true; }'})
            .toArray();
    assert.eq(count.length, 1, "expected 1 document");

    let allLogLines = checkLog.getGlobalLog(conn);
    var slowLoopLogLine;
    assert.soon(() => {
        slowLoopLogLine = findMatchingLogLine(allLogLines, {id: expectedLogId});
        return slowLoopLogLine !== null;
    }, "Couldn't find slow loop log line");

    /*
    const slowLoopObj = JSON.parse(slowLoopLogLine);
    TODO(SERVER-69831): enable when we have a single slow loop log.
    expectedFields.forEach((expectedField) => {
        assert(slowLoopObj.attr[expectedField]);
    });
    */

    // TODO(SERVER-69831): Expect that sourceWorkElapsedMillis and processMessageElapsedMillis are
    // each greater than their respective sleeps, and totalElapsedMillis >= their sum.
}

// Test standalone.
const m = MongoRunner.runMongod();
runTest(m);
MongoRunner.stopMongod(m);

// Test sharded.
const st = new ShardingTest({shards: 1, mongos: 1});
runTest(st.s0);
st.stop();
})();
