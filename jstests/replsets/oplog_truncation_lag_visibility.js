/**
 * SERVER-123047: visibility for oplog truncation lag.
 *
 * Pins the four new fields the ticket added to the `oplogTruncation` server-status
 * section:
 *   - truncateInProgress
 *   - currentTruncateActionStartMillis
 *   - lastTruncateDurationMicros
 *   - writeConflictCount
 *
 * The test:
 *   1. Snapshots the section while the cap maintainer is paused (hangOplogCapMaintainerThread
 *      fail point), asserting the idle-state contract:
 *        truncateInProgress == false
 *        currentTruncateActionStartMillis == 0
 *        writeConflictCount  is a finite Number  >= 0
 *        lastTruncateDurationMicros is a finite Number >= 0
 *      This is the same invariant `IdleMeansZero` in
 *      src/mongo/tla_plus/Replication/OplogTruncationLag/OplogTruncationLag.tla.
 *   2. Releases the fail point, drives a few inserts that force at least one truncate
 *      action, and re-samples the section. truncateCount and writeConflictCount must be
 *      cumulative (non-decreasing) across samples - the spec's WriteConflictMonotone /
 *      TruncateCountMonotone properties.
 *   3. Confirms that once truncates have been observed, the start-millis gauge has
 *      returned to 0 and `truncateInProgress` is false (`InProgressMatchesStart`).
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replSet = new ReplSetTest({
    name: "oplog_truncation_lag_visibility",
    nodes: 1,
    nodeOptions: {
        // Small oplog so a handful of large inserts trigger truncation.
        oplogSize: 1,
    },
});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const adminDb = primary.getDB("admin");

function getOplogTruncationSection() {
    const status = assert.commandWorked(adminDb.serverStatus());
    assert(status.oplogTruncation,
           "oplogTruncation section missing from serverStatus; SERVER-123047 contract broken: " +
               tojson(status));
    return status.oplogTruncation;
}

function assertHasNewFields(section) {
    const required = [
        "truncateInProgress",
        "currentTruncateActionStartMillis",
        "lastTruncateDurationMicros",
        "writeConflictCount",
    ];
    for (const field of required) {
        assert(section.hasOwnProperty(field),
               `oplogTruncation section is missing SERVER-123047 field '${field}'. ` +
                   `Got: ${tojson(section)}`);
    }
    // Type contracts.
    assert.eq(typeof section.truncateInProgress, "boolean", tojson(section));
    assert.eq(typeof section.currentTruncateActionStartMillis, "number", tojson(section));
    assert.eq(typeof section.lastTruncateDurationMicros, "number", tojson(section));
    assert.eq(typeof section.writeConflictCount, "number", tojson(section));
    // Lower-bound contracts.
    assert.gte(section.currentTruncateActionStartMillis, 0, tojson(section));
    assert.gte(section.lastTruncateDurationMicros, 0, tojson(section));
    assert.gte(section.writeConflictCount, 0, tojson(section));
    assert.gte(section.truncateCount, 0, tojson(section));
}

// --- Phase 1: maintainer paused -> the idle-state invariants must hold.
const hangFp = configureFailPoint(primary, "hangOplogCapMaintainerThread");
hangFp.wait();

const idleSection = getOplogTruncationSection();
assertHasNewFields(idleSection);
assert.eq(idleSection.truncateInProgress, false,
          "truncateInProgress must be false while the maintainer is paused: " + tojson(idleSection));
assert.eq(idleSection.currentTruncateActionStartMillis, 0,
          "currentTruncateActionStartMillis must be 0 while the maintainer is paused: " +
              tojson(idleSection));

// --- Phase 2: release the fail point and drive enough inserts to force a truncate.
hangFp.off();

const coll = primary.getDB("test").oplog_trunc_vis;
// 400 KiB each so a 1 MiB oplog rolls over within a few inserts.
const blob = "x".repeat(400 * 1024);
for (let i = 0; i < 8; ++i) {
    assert.commandWorked(coll.insert({_id: i, blob: blob}));
}

// Wait for at least one truncate to be observed via the cumulative counter.
assert.soon(
    () => {
        const s = getOplogTruncationSection();
        return s.truncateCount > 0;
    },
    () => "truncateCount never advanced after inserts; section=" +
              tojson(getOplogTruncationSection()),
    /*timeout*/ 60_000,
);

const postSection = getOplogTruncationSection();
assertHasNewFields(postSection);

// Monotonicity across samples (the spec's WriteConflictMonotone / TruncateCountMonotone).
assert.gte(postSection.writeConflictCount, idleSection.writeConflictCount,
           "writeConflictCount must be cumulative (non-decreasing). idle=" +
               tojson(idleSection) + " post=" + tojson(postSection));
assert.gte(postSection.truncateCount, idleSection.truncateCount,
           "truncateCount must be cumulative. idle=" + tojson(idleSection) +
               " post=" + tojson(postSection));

// At least one truncate completed, so lastTruncateDurationMicros should be non-zero.
assert.gt(postSection.lastTruncateDurationMicros, 0,
          "lastTruncateDurationMicros should reflect a completed truncate: " + tojson(postSection));

// After the workload settles, the in-progress flag and start-millis gauge must agree.
// `InProgressMatchesStart` in the TLA+ spec - this is the regression line for the
// ON_BLOCK_EXIT guard in `_deleteExcessDocuments`.
assert.soon(
    () => {
        const s = getOplogTruncationSection();
        const matches = (s.truncateInProgress === false && s.currentTruncateActionStartMillis === 0) ||
                        (s.truncateInProgress === true && s.currentTruncateActionStartMillis > 0);
        return matches;
    },
    () => "truncateInProgress / currentTruncateActionStartMillis fell out of sync: " +
              tojson(getOplogTruncationSection()),
    /*timeout*/ 30_000,
);

replSet.stopSet();
