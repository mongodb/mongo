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
const sleepMillisInSendResponse = 200;

const expectedFields = [
    "totalMillis",
    "activeMillis",
    "receiveWorkMillis",
    "processWorkMillis",
    "sendResponseMillis",
    "finalizeMillis",
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
    const prevSlowSessionWorkflowCount = getSlowLogCount(conn);

    // Wait, then do a query beyond the 100ms threshold. Make sure the slow loop log line exists.
    const fp = configureFailPoint(
        conn, "sessionWorkflowDelaySendMessage", {millis: sleepMillisInSendResponse});
    coll.find().toArray();
    fp.off();
    let logAndCount = getSlowLogAndCount(conn);
    const slowSessionWorkflowCount = logAndCount.count;
    assert.gt(slowSessionWorkflowCount,
              prevSlowSessionWorkflowCount,
              "Expected to find at least one slow SessionWorkflow log.");

    // Do some sanity checks over the actual contents of the log.
    const slowLoopObj = JSON.parse(logAndCount.log);
    jsTest.log(slowLoopObj);
    let elapsedObj = slowLoopObj.attr.elapsed;
    expectedFields.forEach((expectedField) => {
        assert(expectedField in elapsedObj,
               "Expected to find field but couldn't: " + expectedField);
    });
    const sendResponseElapsed = elapsedObj.sendResponseMillis;

    assert.gte(sendResponseElapsed,
               sleepMillisInSendResponse,
               "The time reported sending a response didn't include the sleep in the failpoint.");
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
