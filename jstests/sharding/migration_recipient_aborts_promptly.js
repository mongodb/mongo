/**
 * Tests that a migration recipient aborts promptly in two scenarios:
 *
 * Test 1 - kClone state (waitForClean):
 *   1. Shard a collection on shard0 and suspend range deletions on shard0.
 *   2. Move the chunk to shard1, leaving shard0 with a pending orphan deletion.
 *   3. Move the chunk back to shard0. shard0 blocks waiting for the orphan deletion to complete.
 *   4. Send _recvChunkAbort to shard0, simulating the donor aborting the migration.
 *   5. Assert shard0 exits the migration well under the drain timeout.
 *
 * Test 2 - kCommitStart state (after recovery doc persisted):
 *   1. Move the chunk to shard1 and pause after the recovery document is persisted.
 *   2. Send _recvChunkAbort to shard1.
 *   3. Release the failpoint and verify the donor's moveChunk completes (i.e. no dangling
 *      recovery document causing _recvChunkReleaseCritSec to loop).
 *
 * @tags: [
 *   assumes_balancer_off,
 *   requires_replication,
 *   requires_fcv_90,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const kAbortExitWindowMs = 3 * 1000;

const st = new ShardingTest({shards: 2, other: {enableBalancer: false}});
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

function setupCollection(ns) {
    const [db, coll] = ns.split(".");
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    const collection = st.s.getDB(db).getCollection(coll);
    for (let i = -10; i < 10; i++) {
        assert.commandWorked(collection.insertOne({_id: i}));
    }
}

function startMoveChunk(ns, toShardName) {
    return startParallelShell(
        funWithArgs(
            function (ns, toShardName) {
                const res = db
                    .getSiblingDB("admin")
                    .runCommand({moveChunk: ns, find: {_id: 0}, to: toShardName});
                assert.commandFailed(res, () => "moveChunk result: " + tojson(res));
            },
            ns,
            toShardName,
        ),
        st.s.port,
    );
}

// Test 1: abort while waiting for orphan cleanup (kClone state).
{
    const ns = dbName + ".coll1";
    setupCollection(ns);

    // Suspend range deletions so the recipient blocks in waitForClean.
    const suspendFp = configureFailPoint(st.shard0, "suspendRangeDeletion");
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}),
    );

    const moveChunkShell = startMoveChunk(ns, st.shard0.shardName);

    let sessionId;
    assert.soon(
        function () {
            const status = assert.commandWorked(st.shard0.adminCommand({_recvChunkStatus: 1}));
            if (status.active) {
                sessionId = status.sessionId;
                return true;
            }
            return false;
        },
        "Recipient migration never became active on shard0",
        15 * 1000,
        100,
    );

    jsTest.log("Sending _recvChunkAbort to shard0", {sessionId});
    const abortStart = Date.now();
    assert.commandWorked(st.shard0.adminCommand({_recvChunkAbort: ns, sessionId}));

    assert.soon(
        function () {
            const status = assert.commandWorked(st.shard0.adminCommand({_recvChunkStatus: 1}));
            return !status.active;
        },
        "Recipient did not exit promptly after _recvChunkAbort",
        kAbortExitWindowMs,
        50,
    );

    jsTest.log("Recipient exited in " + (Date.now() - abortStart) + "ms");

    suspendFp.off();
    moveChunkShell();
}

// Test 2: abort in kCommitStart after the recovery document is persisted.
{
    const ns = dbName + ".coll2";
    setupCollection(ns);

    const hangFp = configureFailPoint(
        st.shard1,
        "hangMigrationRecipientAfterPersistingRecoveryDoc",
    );
    const moveChunkShell = startMoveChunk(ns, st.shard1.shardName);

    hangFp.wait();

    const sessionId = assert.commandWorked(st.shard1.adminCommand({_recvChunkStatus: 1})).sessionId;
    jsTest.log("Sending _recvChunkAbort to shard1", {sessionId});
    assert.commandWorked(st.shard1.adminCommand({_recvChunkAbort: ns, sessionId}));

    hangFp.off();
    moveChunkShell();
}

st.stop();
