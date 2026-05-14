/**
 * Empirical companion to src/mongo/tla_plus/Replication/OplogTruncationLiveness.
 *
 * Reproduces the symptom shape from the production incident in which six
 * fleet clusters stopped truncating oplog after time-based retention was
 * enabled: the oplog cap maintainer thread is stalled, oplog accumulates
 * beyond the retention cap, and (in the bug version) no progress is made;
 * in the fixed version the maintainer eventually shrinks the oplog back
 * below the cap.
 *
 * The hangOplogCapMaintainerThread failpoint is the documented runtime
 * knob that reproduces the prod-scenario stall: while it is alwaysOn the
 * maintainer is parked on a sync point and cannot process pending truncate
 * markers, exactly matching the observed behavior in which the
 * ReplicatedOplogTruncationThread was logging increasing attempts while
 * the oplog continued to grow.
 *
 * Test outline:
 *   1.  Start a 1-node replica set with a 1MB oplog and a 10-second
 *       oplogMinRetentionHours (modeled as 0.002777 hours, matching the
 *       pattern from jstests/noPassthrough/oplog/oplog_retention_hours.js).
 *       The hangOplogCapMaintainerThread failpoint is alwaysOn at start.
 *   2.  Write enough data to push the oplog well past both its byte cap
 *       and its retention window, simulating the prod cluster state in
 *       which "Replication Oplog Window was about a week when truncation
 *       was enabled" (Jira description).
 *   3.  Assert the oplog has NOT been truncated while the failpoint is
 *       held (the symptom of the prod stall).
 *   4.  Release the failpoint and assert the oplog shrinks back below
 *       1.1x its configured cap within a bounded time (the liveness
 *       obligation the maintainer is supposed to satisfy).
 *   5.  Verify the serverStatus.oplogTruncation counters reflect that at
 *       least one truncation actually completed.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const oplogSizeMB = 1;
const oplogSizeBytes = oplogSizeMB * 1024 * 1024;
const oplogSoftCapBytes = 1.1 * oplogSizeBytes; // OplogTruncateMarkers permits 10% overshoot.

// Ten seconds, expressed in hours, mirroring the value used by
// jstests/noPassthrough/oplog/oplog_retention_hours.js. The retention
// window is intentionally short so the test does not wait minutes for
// the maintainer to act on it.
const oplogMinRetentionHours = 0.002777;
const retentionSecondsApprox = oplogMinRetentionHours * 60 * 60;

const tenKB = "a".repeat(10 * 1024);

const replSet = new ReplSetTest({
    name: "oplog_cap_maintainer_truncation_liveness",
    oplogSize: oplogSizeMB,
    nodes: 1,
    nodeOptions: {
        // syncdelay 1s so the checkpointer keeps up and we don't need to
        // wait minutes for stable-timestamp to advance.
        syncdelay: 1,
        setParameter: {
            logComponentVerbosity: tojson({storage: 1}),
            // Park the cap maintainer at the failpoint. This simulates
            // the prod stall: the thread is alive but not making
            // progress on the truncate-marker queue.
            "failpoint.hangOplogCapMaintainerThread": tojson({mode: "alwaysOn"}),
        },
    },
});

// oplogMinRetentionHours is a startup option, passed through to mongod's
// CLI by startSet(); see jstests/noPassthrough/oplog/oplog_retention_hours.js
// for the canonical pattern.
replSet.startSet({oplogMinRetentionHours: oplogMinRetentionHours});
replSet.initiate();

const primary = replSet.getPrimary();
const oplog = primary.getDB("local").oplog.rs;
const coll = primary.getDB("oplog_cap_maintainer_truncation_liveness").coll;

jsTestLog("Step 1: wait for the cap maintainer thread to park at hangOplogCapMaintainerThread.");
assert.commandWorked(
    primary.adminCommand({
        waitForFailPoint: "hangOplogCapMaintainerThread",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

jsTestLog("Step 2: write enough data to push the oplog past its retention window AND its byte cap.");
// Aim well above the soft cap so the prod-style "way past retention" condition
// is unambiguous. 20 inserts of 10KB = ~200KB of user data + oplog overhead;
// loop until we are at least 2x oplogSize.
const initialOplogSize = oplog.dataSize();
let writeCount = 0;
while (oplog.dataSize() < 2 * oplogSizeBytes) {
    assert.commandWorked(coll.insert({tenKB: tenKB}));
    writeCount++;
    // Belt-and-braces guard so a misconfigured run can't loop forever.
    assert.lt(writeCount, 5000, `Wrote ${writeCount} docs without crossing 2x oplog cap`);
}
jsTestLog(`Wrote ${writeCount} docs; oplog dataSize now ${oplog.dataSize()} (cap ${oplogSizeBytes}).`);

// Hold past the retention window so the would-be-truncated entries are
// definitely older than oplogMinRetentionHours. The maintainer is still
// parked; this is the prod-shaped "stalled while retention violated" state.
jsTestLog(`Step 3a: sleep ${retentionSecondsApprox * 2}s (2x retention window) with the maintainer parked.`);
sleep((retentionSecondsApprox * 2) * 1000);

jsTestLog("Step 3b: confirm the oplog has NOT shrunk while the failpoint holds the maintainer.");
const stalledSize = oplog.dataSize();
assert.gt(
    stalledSize,
    oplogSoftCapBytes,
    `Oplog dataSize ${stalledSize} should still exceed soft cap ${oplogSoftCapBytes} ` +
        `while hangOplogCapMaintainerThread is alwaysOn. If this fires, the failpoint may have been ` +
        `relocated; see SERVER-121352.`,
);

// The bug version of the spec (BackoffOnConflict = FALSE in
// MC_bug.cfg) corresponds to this state persisting forever. The fix
// version corresponds to step 4 succeeding inside the assert.soon
// budget.
jsTestLog("Step 4: release the maintainer and assert it shrinks the oplog back below the soft cap.");
assert.commandWorked(primary.adminCommand({configureFailPoint: "hangOplogCapMaintainerThread", mode: "off"}));

// Encourage the maintainer to actually wake by writing a few more
// entries (post-fix, the truncate-marker pipeline needs at least one
// new marker creation to fire). The replicated_truncate path is
// triggered on the next insertion after the marker queue advances.
for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({tenKB: tenKB, postRelease: i}));
}

assert.soon(
    () => {
        const sizeNow = oplog.dataSize();
        const truncationStatus = primary.getDB("admin").runCommand({serverStatus: 1}).oplogTruncation;
        jsTestLog(
            `  oplog dataSize=${sizeNow} cap=${oplogSizeBytes} soft=${oplogSoftCapBytes} ` +
                `truncateCount=${truncationStatus.truncateCount} ` +
                `totalTimeTruncatingMicros=${truncationStatus.totalTimeTruncatingMicros}`,
        );
        return sizeNow < oplogSoftCapBytes;
    },
    "Timed out waiting for the oplog cap maintainer thread to shrink the oplog below the soft cap. " +
        "This is the symptom SERVER-121352 reported in production: maintainer is unblocked but " +
        "still not making progress on the truncate-marker queue.",
    ReplSetTest.kDefaultTimeoutMS,
    1000 /* interval ms */,
);

jsTestLog("Step 5: verify serverStatus.oplogTruncation reflects at least one completed truncation.");
const finalStatus = primary.getDB("admin").runCommand({serverStatus: 1});
assert.commandWorked(finalStatus);
const truncation = finalStatus.oplogTruncation;
assert.gte(
    truncation.truncateCount,
    1,
    `Expected at least one truncation to have completed; oplogTruncation=${tojson(truncation)}`,
);
assert.gt(
    truncation.totalTimeTruncatingMicros,
    0,
    `Expected non-zero totalTimeTruncatingMicros; oplogTruncation=${tojson(truncation)}`,
);

jsTestLog("Done. The maintainer met its liveness obligation after the failpoint was released.");
replSet.stopSet();
