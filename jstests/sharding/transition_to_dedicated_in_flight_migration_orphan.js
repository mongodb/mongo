/**
 * SERVER-125663: orphans accumulate on the former config-server-shard when
 * `transitionToDedicatedConfigServer` races with a migration commit and drops
 * `config.rangeDeletions` between the donor registering the in-memory range-deletion task and
 * `markAsReadyRangeDeletionTaskLocally` running.
 *
 * Exercises the interleaving by hanging the donor inside `markAsReadyRangeDeletionTaskLocally` via
 * the `hangInReadyRangeDeletionLocallyInterruptible` failpoint, then runs
 * `transitionToDedicatedConfigServer` to completion (which drops `config.rangeDeletions` on the
 * config-server-shard), then releases the failpoint and asserts that:
 *
 *   1. No in-memory range-deletion task remains wedged in `pending = true` state.
 *   2. No orphan documents remain on the former config-server-shard.
 *   3. The `moveChunk` invocation that triggered the race did not hang (it was issued without
 *      `waitForDelete`, so a hang would surface as a client-side timeout).
 *
 * Companion to TLA+ spec `src/mongo/tla_plus/Sharding/ConfigServerTransitionOrphan`.
 *
 * @tags: [
 *   requires_fcv_83,
 *   does_not_support_stepdowns,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kDbName = "transition_orphan_db";
const kCollName = "orphans";
const kNs = kDbName + "." + kCollName;

// Bring up a 2-shard cluster with an embedded config server. The config server is also `shard0`
// (the config-server-shard); `shard1` is a dedicated data shard.
const st = new ShardingTest({
    name: "transitionOrphan",
    shards: 2,
    other: {
        configShard: true,
        rsOptions: {
            setParameter: {
                orphanCleanupDelaySecs: 0,
                // Block the range deleter actually running so the test deterministically observes
                // the in-memory task state across the race. Released at end of test.
                "failpoint.suspendRangeDeletion": tojson({mode: "alwaysOn"}),
            },
        },
    },
});

const mongos = st.s;
const configShardName = "config";
const recipientShardName = st.shard1.shardName;

jsTest.log.info("Seed sharded collection on the config-server-shard");
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName, primaryShard: configShardName}));
assert.commandWorked(mongos.adminCommand({shardCollection: kNs, key: {x: 1}}));
const seeded = mongos.getDB(kDbName)[kCollName];
const kDocs = 50;
let bulk = seeded.initializeUnorderedBulkOp();
for (let i = 0; i < kDocs; i++) {
    bulk.insert({x: i, payload: "p".repeat(64)});
}
assert.commandWorked(bulk.execute());

jsTest.log.info("Hang the donor inside markAsReadyRangeDeletionTaskLocally on the config shard");
const configPrimary = st.configRS.getPrimary();
const hangMarkAsReady =
    configureFailPoint(configPrimary, "hangInReadyRangeDeletionLocallyInterruptible");

jsTest.log.info("Start a moveChunk in a parallel shell (donor = config, recipient = shard1)");
const moveChunkAwait = startParallelShell(
    funWithArgs(function (ns, recipient) {
        // No waitForDelete: SERVER-125663 also affects the in-flight migration through the same
        // wedged future, but we keep this test scoped to the orphan accumulation symptom.
        assert.commandWorked(db.getSiblingDB("admin").runCommand({
            moveChunk: ns,
            find: {x: 0},
            to: recipient,
        }));
    }, kNs, recipientShardName),
    mongos.port,
);

jsTest.log.info("Wait for the donor to reach the failpoint");
hangMarkAsReady.wait();

jsTest.log.info("Run transitionToDedicatedConfigServer concurrently (drops config.rangeDeletions)");
assert.commandWorked(mongos.adminCommand({startTransitionToDedicatedConfigServer: 1}));

// movePrimary of any unsharded data hosted on the config shard so the transition can complete.
// In this test no other database has the config-server-shard as primary, so this is a no-op safety
// net.

assert.soon(
    () => {
        const status = mongos.adminCommand({getTransitionToDedicatedConfigServerStatus: 1});
        if (!status.ok) {
            jsTest.log.info({msg: "transition status not ok yet", status});
            return false;
        }
        return status.state === "drainingComplete";
    },
    "transition draining never reached drainingComplete",
    5 * 60 * 1000,
);

jsTest.log.info("Commit transition: this drops config.rangeDeletions on the config-server-shard");
assert.commandWorked(mongos.adminCommand({commitTransitionToDedicatedConfigServer: 1}));

jsTest.log.info("Release the failpoint -- donor now runs markAsReadyRangeDeletionTaskLocally");
hangMarkAsReady.off();

moveChunkAwait();

jsTest.log.info("Allow the range deleter to actually drain orphans");
assert.commandWorked(configPrimary.adminCommand({
    configureFailPoint: "suspendRangeDeletion",
    mode: "off",
}));

jsTest.log.info("Assert no in-memory range-deletion task remains wedged in pending state");
// After SERVER-125663 is fixed, the donor must either: (a) keep `config.rangeDeletions` writable
// until in-flight migrations drain, or (b) be told `clearPending` synchronously even when the
// $unset matches no document. Either way, the in-memory pending-task count must reach 0.
const cfgPendingPath = "shardingStatistics.rangeDeleterTasks";
assert.soon(
    () => {
        const serverStatus = configPrimary.adminCommand({serverStatus: 1});
        assert.commandWorked(serverStatus);
        const rdStats = serverStatus.shardingStatistics &&
            serverStatus.shardingStatistics.rangeDeleterTasks;
        // The exact field name has varied across versions; we accept either `pending` or
        // `numPending` keys.
        const pending = rdStats &&
            (rdStats.pending !== undefined ? rdStats.pending :
                                             rdStats.numPending !== undefined ? rdStats.numPending : 0);
        return pending === 0;
    },
    "in-memory range-deletion task remained pending after transition (SERVER-125663 regression)",
    60 * 1000,
);

jsTest.log.info("Assert no orphan documents remain on the former config-server-shard");
const formerConfigShardConn = st.rs0.getPrimary();
assert.soon(
    () => {
        // The collection itself still exists on the former config-server-shard until it is
        // dropped by `cleanupOrphaned` / range deleter. Use the rangeDeleterStats orphan count.
        const stats = formerConfigShardConn.getDB(kDbName).runCommand({collStats: kCollName});
        if (!stats.ok) {
            // Collection has already been removed entirely -- 0 orphans.
            return true;
        }
        const numOrphans = stats.numOrphanDocs !== undefined ? stats.numOrphanDocs : stats.count;
        jsTest.log.info({msg: "orphan-poll", numOrphans});
        return numOrphans === 0;
    },
    "orphan documents remained on former config-server-shard (SERVER-125663 regression)",
    2 * 60 * 1000,
);

st.stop();
