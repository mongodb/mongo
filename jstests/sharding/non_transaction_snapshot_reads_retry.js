/**
 * Test that mongos retries with a new read timestamp if a non-transaction snapshot read fails
 * with a SnapshotError.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 *
 * - Create a sharded collection and insert a document.
 * - Start a snapshot read with no atClusterTime, tne read selects some atClusterTime T.
 * - Block the read with a failpoint.
 * - Update the document at timestamp updateTS > T.
 * - Sleep until updateTS is older than historyWindowSecs.
 * - Insert a document with w: majority to trigger history cleanup.
 * - Unblock the read.
 * - The read will fail with SnapshotTooOld, mongos should retry and succeed.
 * - Assert the read succeeded and returned the updated (post-updateTS) document.
 */
import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const historyWindowSecs = 10;
const testMarginSecs = 1;
const st = new ShardingTest({
    shards: {rs0: {nodes: 1}},
    other: {rsOptions: {setParameter: {minSnapshotHistoryWindowInSeconds: historyWindowSecs}}}
});

const primary = st.rs0.getPrimary();
const primaryAdmin = primary.getDB("admin");
assert.eq(assert
              .commandWorked(
                  primaryAdmin.runCommand({getParameter: 1, minSnapshotHistoryWindowInSeconds: 1}))
              .minSnapshotHistoryWindowInSeconds,
          historyWindowSecs);

const mongosDB = st.s.getDB("test");
const mongosColl = mongosDB.test;

assert.commandWorked(
    mongosDB.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.rs0.getURL()}));
st.shardColl(mongosColl, {_id: 1}, false);
let result =
    mongosDB.runCommand({insert: "test", documents: [{_id: 0}], writeConcern: {w: "majority"}});
const insertTS = assert.commandWorked(result).operationTime;
jsTestLog(`Inserted document at ${tojson(insertTS)}`);

const fp = configureFailPoint(primary, "waitInFindBeforeMakingBatch", {nss: "test.test"});

function read(insertTS, enableCausal, updatedValue, retries) {
    assert.lte(retries, 60);
    const readConcern = {level: "snapshot"};
    if (enableCausal) {
        readConcern.afterClusterTime = insertTS;
    }

    const result = assert.commandWorked(db.runCommand({
        find: "test",
        filter: {_id: 0},
        projection: {
            _id: 1,
            x: 1,
            "foo.bar": 1
        },  // use non-simple projection to avoid Express fast-path; it doesn't yield
        singleBatch: true,
        readConcern: readConcern
    }));

    jsTestLog(`find result for enableCausal=${enableCausal}: ${tojson(result)}`);
    const act = result.cursor.atClusterTime;
    assert.gte(act, insertTS);
    const firstResult = result.cursor.firstBatch[0];
    assert.eq(firstResult["_id"], 0);
    // The fundamental problem with this test is that we don't know if we have reached updateTS or
    // not because we cannot pass updateTS to the shells after they have started. The only guarantee
    // (modulo a bug) is that we have reached insertTS. Therefore, we retry the read until we see
    // the update.
    if (!firstResult.hasOwnProperty("x")) {
        const sleepMS = 1000;
        jsTestLog(`Wait ${sleepMS}ms for the clusterTime to catch up (retries: ${retries})`);
        sleep(sleepMS);
        return read(insertTS, enableCausal, updatedValue, retries + 1);
    }
    assert.eq(firstResult["x"], updatedValue);
}

const updatedValue = "updatedValue";
const waitForShell =
    startParallelShell(funWithArgs(read, insertTS, false, updatedValue, 0), st.s.port);
const waitForShellCausal =
    startParallelShell(funWithArgs(read, insertTS, true, updatedValue, 0), st.s.port);

jsTestLog("Wait for shells to hit waitInFindBeforeMakingBatch failpoint");
fp.wait(kDefaultWaitForFailPointTimeout, 2);

jsTestLog("Update document");
result = mongosDB.runCommand({
    update: "test",
    updates: [{q: {_id: 0}, u: {$set: {x: updatedValue}}}],
    writeConcern: {w: "majority"}
});

const updateTS = assert.commandWorked(result).operationTime;
jsTestLog(`Updated document at updateTS ${tojson(updateTS)}`);
assert.gt(updateTS, insertTS);

jsTestLog("Sleep until updateTS is older than historyWindowSecs");
sleep((historyWindowSecs + testMarginSecs) * 1000);

jsTestLog("Trigger history cleanup with a w-majority insert");
result = assert.commandWorked(
    mongosDB.runCommand({insert: "test", documents: [{_id: 1}], writeConcern: {w: "majority"}}));
const historyCleanupTS = result.operationTime;
jsTestLog(`History cleanup at ${tojson(historyCleanupTS)}`);

jsTestLog("Disable failpoint");
fp.off();

jsTestLog("Wait for shells to finish");
waitForShell();
waitForShellCausal();

st.stop();
