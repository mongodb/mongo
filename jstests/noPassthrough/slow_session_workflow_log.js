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

// This specific test doesn't care about precision in the times reported by the server. Since the
// server may be doing some extra work before listening for another message, and since the server
// and this test may vary in the precision by which they convert ticks to milliseconds, we allow
// some error when comparing local times to server times.
const errorAllowance = 0.9;

const expectedFields = [
    "totalElapsedMillis",
    "activeElapsedMillis",
    "sourceWorkElapsedMillis",
    "processWorkElapsedMillis",
    "sendResponseElapsedMillis",
    "finalizeElapsedMillis",
];

function getSlowLogAndCount(conn) {
    let allLogLines = checkLog.getGlobalLog(conn);
    let slowSessionWorkflowLines = findMatchingLogLines(allLogLines, {id: expectedLogId});
    let slowLinesArr = [];
    if (slowSessionWorkflowLines !== null) {
        slowLinesArr = Array.from(slowSessionWorkflowLines);
    }

    return {log: slowLinesArr.at(-1), count: slowLinesArr.length};
}

function getSlowLogCount(conn) {
    return getSlowLogAndCount(conn).count;
}

function runTest(conn) {
    let coll = conn.getCollection("test.foo");
    coll.drop();
    assert.commandWorked(assert.commandWorked(coll.insert({_id: 1})));

    // In order to find the new log lines, a baseline needs to be established.
    let slowSessionWorkflowCount = getSlowLogCount(conn);

    // Do a query that we would expect to be fast. Expect no new slow SessionWorkflows are logged.
    let count = coll.find({}).toArray();
    assert.eq(count.length, 1, "expected 1 document");
    let prevSlowSessionWorkflowCount = slowSessionWorkflowCount;
    slowSessionWorkflowCount = getSlowLogCount(conn);
    assert.eq(slowSessionWorkflowCount,
              prevSlowSessionWorkflowCount,
              "There should not be a slow SessionWorkflow log at this point but one was found.");

    // Wait, then do a query beyond the 100ms threshold. Make sure the slow loop log line exists.
    sleep(sleepMillisBetweenQueries);
    coll.find({$where: 'function() { sleep(' + sleepMillisInQueryFilter + '); return true; }'})
        .toArray();
    let logAndCount = getSlowLogAndCount(conn);
    prevSlowSessionWorkflowCount = slowSessionWorkflowCount;
    slowSessionWorkflowCount = logAndCount.count;
    assert.eq(slowSessionWorkflowCount,
              prevSlowSessionWorkflowCount + 1,
              "Expected to find a slow SessionWorkflow log.");

    // Do some sanity checks over the actual contents of the log.
    const slowLoopObj = JSON.parse(logAndCount.log);
    expectedFields.forEach((expectedField) => {
        assert(slowLoopObj.attr[expectedField] !== null,
               "Expected to find field but couldn't: " + expectedField);
    });
    let totalElapsed = slowLoopObj.attr.totalElapsedMillis;
    let activeElapsed = slowLoopObj.attr.activeElapsedMillis;
    let sourceWorkElapsed = slowLoopObj.attr.sourceWorkElapsedMillis;
    let processWorkElapsed = slowLoopObj.attr.processWorkElapsedMillis;
    assert.gte(
        sourceWorkElapsed,
        sleepMillisBetweenQueries * errorAllowance,
        "The time reported sourcing a message didn't include the time sleeping between queries.");
    assert.gte(processWorkElapsed,
               sleepMillisInQueryFilter,
               "The time reported processing work didn't include the sleep in the find filter.");

    // When comparing server time to another server time, there is no reason to expect error.
    assert.gte(activeElapsed,
               processWorkElapsed,
               "The time reported as active time didn't include the time processing work.");
    assert.gte(
        totalElapsed,
        sourceWorkElapsed + activeElapsed,
        "The total time reported didn't include the sum of active time and message sourcing time.");
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
