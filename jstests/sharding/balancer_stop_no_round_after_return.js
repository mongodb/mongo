/**
 * Reproduces SERVER-100155 (CAR-impact 5).
 *
 * Once `_configsvrBalancerStop` returns OK to the operator, no migration is permitted to commit.
 * The bug lets a stepdown-interrupted balancer round mark itself complete (via `_endRound`) while
 * a migration is still `commitInFlight` on the shard side, so `joinCurrentRound` returns OK but a
 * later commit lands on disk -- breaking every operator workflow that runs `mongodump` or
 * `mongosync` after stopping the balancer.
 *
 * The test:
 *   1. Boots a 3-node CSRS with 2 shards.
 *   2. Shards a collection and pre-splits it so the balancer has work.
 *   3. Pins the donor at `hangBeforeSendingCommitDecision` so the migration is `commitInFlight`
 *      from the recipient's point of view but cannot finalize.
 *   4. Steps down the CSRS primary while the migration is pinned (mirrors the
 *      `InterruptedDueToReplStateChange' path inside `Balancer::_mainThread`).
 *   5. After the new primary is up, fires `balancerStop` and records the moment it returns OK.
 *   6. Releases the pinned commit decision and asserts that no migration finalised after the
 *      recorded OK timestamp. The buggy implementation lets the commit slip through; the fix
 *      keeps the migration pending until the round actually drains.
 *
 * @tags: [
 *   requires_fcv_80,
 *   requires_sharding,
 *   requires_replication,
 *   does_not_support_stepdowns,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const st = new ShardingTest({
    shards: 2,
    config: 3,
    other: {
        enableBalancer: false,
        configOptions: {
            setParameter: {
                // Speed up balancer rounds so the test is not blocked on default throttling.
                balancerMigrationsThrottlingMs: 0,
            },
        },
    },
});

const dbName = "test_server100155";
const collName = "moveable";
const ns = `${dbName}.${collName}`;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

// Pre-split so the balancer has imbalance to act on, with both halves currently on shard0.
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));

const coll = st.s.getDB(dbName)[collName];
const bulk = coll.initializeUnorderedBulkOp();
for (let i = -50; i < 50; i++) {
    bulk.insert({x: i, payload: "x".repeat(1024)});
}
assert.commandWorked(bulk.execute());

// -------------------------------------------------------------------------------------------------
// Pin the donor mid-commit. The recipient has already accepted the clone, so from its perspective
// the migration is `commitInFlight'. The donor will sit at `hangBeforeSendingCommitDecision` until
// we release it after balancerStop has returned.
// -------------------------------------------------------------------------------------------------
const donor = st.shard0;
const hangCommit = configureFailPoint(donor, "hangBeforeSendingCommitDecision");

// Issue the migration in a parallel shell so we can drive the rest of the scenario while it hangs.
const moveRangeShell = startParallelShell(
    funWithArgs(function (nss, toShard) {
        // The command may fail with InterruptedDueToReplStateChange if the CSRS steps down while
        // the commit is in flight. That outcome is fine; what matters for this test is the shard-
        // side commit decision the donor has already prepared, not the success of this RPC.
        const ret = db.getSiblingDB("admin").runCommand({
            moveRange: nss,
            min: {x: 0},
            toShard: toShard,
            secondaryThrottle: false,
            waitForDelete: false,
        });
        jsTest.log("moveRange parallel shell returned: " + tojson(ret));
    }, ns, st.shard1.shardName),
    st.s.port,
);

hangCommit.wait();
jsTest.log("Donor pinned at hangBeforeSendingCommitDecision; CSRS stepdown next.");

// -------------------------------------------------------------------------------------------------
// Step down the CSRS. `Balancer::onStepDown' fires `requestTermination', which marks the
// balancer's `_threadOperationContext' killed -- mirroring the `InterruptedDueToReplStateChange'
// path inside `_mainThread`. After step-up on a new primary, the balancer is `mode=full' again,
// but the previous round's in-flight commit is still pinned on the donor.
// -------------------------------------------------------------------------------------------------
const oldPrimary = st.configRS.getPrimary();
const newPrimary = st.configRS.getSecondaries()[0];
jsTest.log(`Stepping down ${oldPrimary.host}; new primary will be ${newPrimary.host}.`);
st.configRS.stepUp(newPrimary);
st.configRS.awaitReplication();
assert.eq(st.configRS.getPrimary().host, newPrimary.host, "stepUp did not promote the chosen node");

// -------------------------------------------------------------------------------------------------
// Issue balancerStop. With the fix in place, joinCurrentRound must NOT return OK until the
// pinned commit is drained (committed or aborted). With the bug present, balancerStop will return
// OK while the commit is still pinned on the donor; the failpoint release below then commits the
// migration AFTER OK -- the violation we are pinning.
// -------------------------------------------------------------------------------------------------
let okTimestamp = null;
const balancerStopShell = startParallelShell(
    funWithArgs(function () {
        const ret = assert.commandWorked(db.getSiblingDB("admin").runCommand({balancerStop: 1}));
        // Stamp the moment OK was observed -- the operator's "the cluster is quiesced" point.
        // The shell writes back via a sentinel collection that the parent reads after join.
        assert.commandWorked(
            db.getSiblingDB("test_server100155").server100155_sentinel.insert({
                _id: "stop_ok_at",
                clusterTime: new Date(),
                response: ret,
            }),
        );
    }),
    st.s.port,
);

// Give balancerStop a beat to enter joinCurrentRound. If the bug is present, OK lands almost
// immediately because `_inBalancerRound' was already cleared on the interrupt path. If the fix is
// present, joinCurrentRound spins waiting for the round to drain -- the failpoint release below
// is the only way out.
sleep(2000);

// -------------------------------------------------------------------------------------------------
// Release the pinned commit. Any migration that finalises from this point onward must NOT
// observe `stopReturnedOk = TRUE' (in spec terms). If it does, we have reproduced SERVER-100155.
// -------------------------------------------------------------------------------------------------
jsTest.log("Releasing hangBeforeSendingCommitDecision; recipient is now free to finalise commit.");
hangCommit.off();

moveRangeShell();
balancerStopShell();

const sentinel = st.s.getDB("test_server100155").server100155_sentinel.findOne({_id: "stop_ok_at"});
assert.neq(sentinel, null, "balancerStop parallel shell never recorded its OK timestamp");
const stopOkAt = sentinel.clusterTime;
jsTest.log(`balancerStop returned OK at ${stopOkAt.toISOString()}.`);

// -------------------------------------------------------------------------------------------------
// THE LOAD-BEARING ASSERTION.
//
// Walk every chunk for the namespace. None of them may have been written to disk AFTER the
// moment balancerStop returned OK. Chunks are stamped with `lastmod' / `lastmodEpoch'; the
// `history' array on each chunk records every ownership transition with a `validAfter' timestamp.
// We check both surfaces: any `validAfter' newer than `stopOkAt' is a SERVER-100155 violation.
// -------------------------------------------------------------------------------------------------
const configDB = st.s.getDB("config");
const chunks = findChunksUtil.findChunksByNs(configDB, ns).toArray();
assert.gt(chunks.length, 0, "expected at least one chunk for the test namespace");

let violations = [];
for (const chunk of chunks) {
    if (!Array.isArray(chunk.history)) {
        continue;
    }
    for (const entry of chunk.history) {
        // `validAfter' is a Timestamp (clusterTime); compare against the JS Date we stamped.
        const validAfterMs = entry.validAfter.getTime() * 1000;
        if (validAfterMs > stopOkAt.getTime()) {
            violations.push({
                chunkId: chunk._id,
                shard: entry.shard,
                validAfter: entry.validAfter,
                stopOkAt: stopOkAt,
            });
        }
    }
}

assert.eq(
    violations.length,
    0,
    "SERVER-100155: migration(s) finalised AFTER balancerStop returned OK: " + tojson(violations),
);

// Verify balancer is actually stopped now (post-condition).
const status = assert.commandWorked(st.s.adminCommand({balancerStatus: 1}));
assert.eq(status.mode, "off", "balancer mode should be off after balancerStop returned OK");
assert.eq(status.inBalancerRound, false, "no balancer round should be in flight after balancerStop");

jsTest.log("SERVER-100155 regression check passed: no migration committed after balancerStop OK.");

st.stop();
