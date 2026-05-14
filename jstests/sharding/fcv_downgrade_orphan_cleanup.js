/**
 * Regression test for SERVER-121914.
 *
 * Downgrading the FCV aborts in-flight chunk migrations. If a migration is aborted after it has
 * committed but before its range-deletion task has been scheduled, the range-deletion document is
 * left in `pending: true` state and the corresponding orphans are not cleaned up until the
 * migration is lazily recovered (next query on the namespace, or next step-up).
 *
 * This test induces that window using the `hangBeforeWritingDecisionDocument` failpoint on the
 * donor, runs setFeatureCompatibilityVersion: lastLTSFCV in parallel, and asserts that within 60s
 * of the downgrade completing either:
 *   (a) the orphan count on the donor drops to zero (synchronous drain happened), or
 *   (b) a structured warning was logged identifying the leaked pending range-deletion.
 *
 * @tags: [
 *   requires_fcv_81,
 *   multiversion_incompatible,
 *   does_not_support_stepdowns,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "fcvDowngradeOrphanCleanup";
const collName = "coll";
const ns = dbName + "." + collName;

const st = new ShardingTest({
    shards: {rs0: {nodes: 1}, rs1: {nodes: 1}},
    mongos: 1,
    config: 1,
});

const mongosAdmin = st.s.getDB("admin");
const donor = st.rs0.getPrimary();
const recipient = st.rs1.getPrimary();

// Make sure we begin at latest FCV.
assert.commandWorked(mongosAdmin.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
checkFCV(donor.getDB("admin"), latestFCV);
checkFCV(recipient.getDB("admin"), latestFCV);

// Create a sharded collection living on shard0 initially.
assert.commandWorked(mongosAdmin.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(mongosAdmin.runCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(mongosAdmin.runCommand({split: ns, middle: {_id: 0}}));

const numDocs = 200;
const testColl = st.s.getDB(dbName)[collName];
let bulk = testColl.initializeUnorderedBulkOp();
for (let i = -numDocs / 2; i < numDocs / 2; i++) {
    bulk.insert({_id: i, payload: "x".repeat(64)});
}
assert.commandWorked(bulk.execute());

// Pause range deletion on the donor so orphans accumulate even on the happy-path.
const suspendRangeDeletion = configureFailPoint(donor, "suspendRangeDeletion");

// Hang the migration on the donor after commit but before the decision document is persisted.
// This is the window in which a concurrent setFCV abort leaks a `pending: true` range deletion.
const hangAfterCommit = configureFailPoint(donor, "hangBeforeWritingDecisionDocument");

// Kick off the moveChunk in a parallel shell so we can drive setFCV against the same cluster.
const moveChunkShell = startParallelShell(function () {
    // Expected to fail with MigrationAborted once setFCV aborts the migration.
    const res = db
        .getSiblingDB("admin")
        .runCommand({moveChunk: "fcvDowngradeOrphanCleanup.coll", find: {_id: 50}, to: "shard1-rs"});
    jsTest.log.info("moveChunk shell completed: " + tojson(res));
}, st.s.port);

hangAfterCommit.wait();
jsTest.log.info("Donor reached post-commit hang; firing setFCV downgrade.");

// Trigger downgrade. The lastLTSFCV global is provided by mongo_shell_session.js.
assert.commandWorked(
    mongosAdmin.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
);

// Release the failpoint so the migration can observe the abort.
hangAfterCommit.off();
moveChunkShell();

// Re-enable the range deleter — if the downgrade path drained orphans synchronously, the donor
// should already be empty. Otherwise, the recovery hook should kick in shortly.
suspendRangeDeletion.off();

// Count orphans on the donor for the migrated range. With the bug present, pending range deletions
// linger and the orphan-doc count stays > 0 indefinitely until a lazy recovery is triggered.
const donorColl = donor.getCollection(ns);
const rangeDeletionsColl = donor.getDB("config").getCollection("rangeDeletions");

function orphanSnapshot() {
    // Anything with _id >= 0 was supposed to leave the donor in the [0, +inf) chunk move.
    const orphanCount = donorColl.find({_id: {$gte: 0}}).itcount();
    const pendingDeletions = rangeDeletionsColl.find({nss: ns, pending: true}).toArray();
    return {orphanCount, pendingDeletions};
}

let lastSnapshot;
const drained = assert.soon(
    () => {
        lastSnapshot = orphanSnapshot();
        return lastSnapshot.orphanCount === 0;
    },
    () => "Orphans never drained after FCV downgrade. Last snapshot: " + tojson(lastSnapshot),
    60 * 1000,
    1000,
    {runHangAnalyzer: false},
);

// Acceptance path B: if the orphan count did not drain, we require a structured warning so an
// operator at least has telemetry pointing at the leaked range-deletion. Today no such log exists,
// which is exactly the gap SERVER-121914 captures. Once the fix lands this branch becomes the
// fallback for the drain-on-recovery variant.
if (lastSnapshot.orphanCount !== 0) {
    const log = checkLog.checkContainsOnceJson(donor, 10083100 /* SERVER-121914 reserved id */, {
        nss: ns,
    });
    assert(
        log,
        "FCV downgrade left orphans without emitting the SERVER-121914 telemetry log. " +
            "Snapshot: " + tojson(lastSnapshot),
    );
}

assert(drained || lastSnapshot.orphanCount === 0,
       "Either the synchronous drain or the structured warning must succeed.");

st.stop();
