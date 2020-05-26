/**
 * Test that mongos retries with a new read timestamp if a non-transaction snapshot read fails
 * with a SnapshotError.
 *
 * @tags: [
 *   requires_fcv_46,
 *   requires_majority_read_concern,
 * ]
 *
 * - Create a sharded collection.
 * - Insert a document at timestamp insertTS.
 * - Start a snapshot read with no atClusterTime, block it with a failpoint.
 * - Update the document.
 * - After now > insertTS + window + margin, insertTS is expired.
 * - Unblock the read.
 * - The read will fail with SnapshotTooOld, mongos should retry and succeed.
 * - Assert the read succeeded and returned the updated (post-insertTS) document.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");

const historyWindowSecs = 10;
const st = new ShardingTest({
    shards: {rs0: {nodes: 1}},
    other: {rsOptions: {setParameter: {minSnapshotHistoryWindowInSeconds: historyWindowSecs}}}
});

const primaryAdmin = st.rs0.getPrimary().getDB("admin");
assert.eq(assert
              .commandWorked(
                  primaryAdmin.runCommand({getParameter: 1, minSnapshotHistoryWindowInSeconds: 1}))
              .minSnapshotHistoryWindowInSeconds,
          historyWindowSecs);

const mongosDB = st.s.getDB("test");
const mongosColl = mongosDB.test;

assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());
st.shardColl(mongosColl, {_id: 1}, false);
let result = mongosDB.runCommand({insert: "test", documents: [{_id: 0}]});
const insertTS = assert.commandWorked(result).operationTime;
const postInsertTime = Date.now();
jsTestLog(`Inserted document at ${insertTS}`);

assert.commandWorked(primaryAdmin.runCommand({
    configureFailPoint: "waitInFindBeforeMakingBatch",
    mode: "alwaysOn",
    data: {nss: "test.test"}
}));

function read(insertTS, enableCausal) {
    const readConcern = {level: "snapshot"};
    if (enableCausal) {
        readConcern.afterClusterTime = insertTS;
    }

    let result = assert.commandWorked(
        db.runCommand({find: "test", singleBatch: true, readConcern: readConcern}));

    jsTestLog(`find result for enableCausal=${enableCausal}: ${tojson(result)}`);
    assert.gt(result.cursor.atClusterTime, insertTS);
    assert.eq(result.cursor.firstBatch[0], {_id: 0, x: "updatedValue"});
}
const waitForShell = startParallelShell(funWithArgs(read, insertTS, false), st.s.port);
const waitForShellCausal = startParallelShell(funWithArgs(read, insertTS, true), st.s.port);

jsTestLog("Wait for shells to hit waitInFindBeforeMakingBatch failpoint");
assert.commandWorked(primaryAdmin.runCommand({
    waitForFailPoint: "waitInFindBeforeMakingBatch",
    timesEntered: 2,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Sleep until insertTS is older than historyWindowSecs");
const testMarginMS = 1000;
const expirationTime = postInsertTime + historyWindowSecs * 1000;
sleep(expirationTime + testMarginMS - Date.now());

jsTestLog("Update document");
assert.commandWorked(mongosDB.test.updateOne(
    {_id: 0}, {$set: {x: "updatedValue"}}, {writeConcern: {w: "majority"}}));

jsTestLog("Disable failpoint");
assert.commandWorked(
    primaryAdmin.runCommand({configureFailPoint: "waitInFindBeforeMakingBatch", mode: "off"}));

jsTestLog("Wait for shells to finish");
waitForShell();
waitForShellCausal();

st.stop();
})();
