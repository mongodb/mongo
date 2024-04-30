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

const historyWindowSecs = 10;
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

function read(insertTS, enableCausal) {
    const readConcern = {level: "snapshot"};
    if (enableCausal) {
        readConcern.afterClusterTime = insertTS;
    }

    let result = assert.commandWorked(db.runCommand({
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
    assert.gt(result.cursor.atClusterTime, insertTS);
    assert.eq(result.cursor.firstBatch[0], {_id: 0, x: "updatedValue"});
}
const waitForShell = startParallelShell(funWithArgs(read, insertTS, false), st.s.port);
const waitForShellCausal = startParallelShell(funWithArgs(read, insertTS, true), st.s.port);

jsTestLog("Wait for shells to hit waitInFindBeforeMakingBatch failpoint");
fp.wait(kDefaultWaitForFailPointTimeout, 2);

jsTestLog("Update document");
result = mongosDB.runCommand({
    update: "test",
    updates: [{q: {_id: 0}, u: {$set: {x: "updatedValue"}}}],
    writeConcern: {w: "majority"}
});

const updateTS = assert.commandWorked(result).operationTime;
jsTestLog(`Updated document at updateTS ${tojson(updateTS)}`);

jsTestLog("Sleep until updateTS is older than historyWindowSecs");
const testMarginMS = 1000;
sleep(historyWindowSecs * 1000 + testMarginMS);

jsTestLog("Trigger history cleanup with a w-majority insert");
assert.commandWorked(
    mongosDB.runCommand({insert: "test", documents: [{_id: 1}], writeConcern: {w: "majority"}}));

jsTestLog("Disable failpoint");
fp.off();

jsTestLog("Wait for shells to finish");
waitForShell();
waitForShellCausal();

st.stop();
