/**
 * Tests that initial sync skips waiting for the sync source's lastStableRecoveryTimestamp to
 * advance when the sync source's earliest oplog entry is the "initiating set" entry and
 * lastStableRecoveryTimestamp is within the configured threshold of that entry.
 *
 * Setup: a single-node primary whose stable recovery timestamp is held at the initiating-set
 * entry's timestamp. Writes advance the primary's optime beyond that held value, so that without
 * the skip logic, initial sync would stall waiting for the stable timestamp to catch up.
 * With the skip logic, initial sync should complete successfully.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Use a 1 MB oplog so the "initiating set" entry remains the earliest entry throughout the test.
const rst = new ReplSetTest({nodes: 1, nodeOptions: {oplogSize: 1}});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

// Wait for the primary to take its first stable checkpoint so that lastStableRecoveryTimestamp
// is set to a value near the initiating set entry's timestamp.
assert.soon(() => {
    const status = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
    return bsonWoCompare(status.lastStableRecoveryTimestamp, Timestamp(0, 0)) > 0;
}, "Timed out waiting for primary to take its first stable checkpoint");

// Retrieve the initiating set entry's timestamp from the head of the oplog.
const initiatingSetTs = primary
    .getDB("local")
    .oplog.rs.find()
    .sort({$natural: 1})
    .limit(1)
    .next().ts;
jsTestLog("Initiating set oplog entry timestamp: " + tojson(initiatingSetTs));

// Hold the primary's stable recovery timestamp at the initiating set entry's timestamp.
// This prevents lastStableRecoveryTimestamp from advancing past initiatingSetTs, so that
// without the initiating-set skip logic, initial sync would stall indefinitely waiting for
// the stable timestamp to reach beginApplyingTimestamp.
const holdStableFp = configureFailPoint(primary, "holdStableTimestampAtSpecificTimestamp", {
    timestamp: initiatingSetTs,
});

// Insert documents to advance the primary's optime. beginApplyingTimestamp will be set to
// a timestamp above the held stable recovery timestamp.
const testDb = primary.getDB("test");
assert.commandWorked(testDb.coll.insertMany([{a: 1}, {a: 2}, {a: 3}]));

// Capture the primary's last oplog entry timestamp. Without the initiating-set reset logic,
// this is what beginApplyingTimestamp would be set to on the secondary.
const normalBeginApplyingTs = primary
    .getDB("local")
    .oplog.rs.find()
    .sort({$natural: -1})
    .limit(1)
    .next().ts;
assert(
    bsonWoCompare(normalBeginApplyingTs, initiatingSetTs) > 0,
    "Primary's last oplog entry should be newer than the initiating set entry. normalBeginApplyingTs=" +
        tojson(normalBeginApplyingTs) +
        " initiatingSetTs=" +
        tojson(initiatingSetTs),
);

jsTestLog("Adding secondary to trigger initial sync.");

// Add a new non-voting secondary to trigger initial sync. numInitialSyncAttempts=1 ensures
// the test fails fast if the attempt is rejected rather than retried indefinitely.
const secondary = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        numInitialSyncAttempts: 1,
        // The default threshold is 0, so the skip would only fire while the sync source's stable
        // recovery timestamp is still at the initiating-set entry. This test writes data first (so
        // the stable timestamp has advanced), then verifies the skip resets beginApplyingTimestamp;
        // set a large threshold to force the skip to fire regardless.
        initialSyncWaitForSyncSourceLastStableRecoveryTsInitiatingSetThresholdSecs: 3600,
    },
});

// Pause initial sync just before database cloning begins. At this point _checkIfInitiatingSet()
// has already run and overridden beginApplyingTimestamp with the initiating set entry timestamp.
const pauseBeforeCloningFp = configureFailPoint(secondary, "initialSyncHangBeforeCopyingDatabases");

rst.reInitiate();

// Wait for initial sync to reach the cloning phase.
pauseBeforeCloningFp.wait();

// Validate that beginApplyingTimestamp was reset to the initiating set entry's timestamp
// rather than remaining at the normal value (normalBeginApplyingTs).
const syncStatus = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
assert(
    syncStatus.initialSyncStatus,
    "Expected initialSyncStatus in replSetGetStatus during initial sync",
);
assert.eq(
    syncStatus.initialSyncStatus.initialSyncOplogStart,
    initiatingSetTs,
    "beginApplyingTimestamp should have been reset to the initiating set entry timestamp " +
        tojson(initiatingSetTs) +
        ", not the normal value " +
        tojson(normalBeginApplyingTs),
);
jsTestLog(
    "Validated: startApplyingTimestamp was reset from " +
        tojson(normalBeginApplyingTs) +
        " to initiating set entry timestamp: " +
        tojson(initiatingSetTs),
);

pauseBeforeCloningFp.off();

// Initial sync should complete successfully because:
//  - The earliest oplog entry on the sync source IS the "initiating set" noop entry.
//  - lastStableRecoveryTimestamp == initiatingSetTs (held by the failpoint).
//  - The diff between lastStableRecoveryTimestamp and the initiating set entry timestamp is
//    zero, which is within the default 3600 s threshold.
// The InitialSyncer therefore skips the wait and proceeds directly to cloning.
rst.awaitSecondaryNodes(null, [secondary]);
jsTestLog("Initial sync completed successfully via the initiating-set skip path.");

// Verify the inserted documents were synced.
assert.eq(3, secondary.getDB("test").coll.find().itcount());

holdStableFp.off();
rst.stopSet();
