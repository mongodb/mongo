/**
 * Tests that a migration recipient aborts promptly when stuck waiting for orphan cleanup.
 *
 * Test steps:
 *   1. Shard a collection on shard0 and suspend range deletions on shard0.
 *   2. Move the chunk to shard1, leaving shard0 with a pending orphan deletion.
 *   3. Move the chunk back to shard0. shard0 blocks waiting for the orphan deletion to
 *      complete.
 *   4. Send _recvChunkAbort to shard0, simulating the donor aborting the migration.
 *   5. Assert shard0 exits the migration well under the drain timeout.
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
const collName = "coll";
const ns = dbName + "." + collName;

const kAbortExitWindowMs = 3 * 1000;

const st = new ShardingTest({shards: 2, other: {enableBalancer: false}});
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

for (let i = -10; i < 10; i++) {
    assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insertOne({_id: i}));
}

// Suspend range deletions on shard0 so the migration recipient will block waiting for orphan cleanup.
const suspendFp = configureFailPoint(st.shard0, "suspendRangeDeletion");
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

const moveChunkShell = startParallelShell(
    funWithArgs(
        function (ns, shard0Name) {
            db.getSiblingDB("admin").runCommand({moveChunk: ns, find: {_id: 0}, to: shard0Name});
        },
        ns,
        st.shard0.shardName,
    ),
    st.s.port,
);

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
assert.commandWorked(st.shard0.adminCommand({_recvChunkAbort: ns, sessionId: sessionId}));

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

st.s.getDB(dbName).getCollection(collName).drop();
st.stop();
