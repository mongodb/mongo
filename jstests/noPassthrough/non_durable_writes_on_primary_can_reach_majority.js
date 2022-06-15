/**
 * Tests that non-durable writes on the primary can be successfully majority confirmed by the
 * secondaries.
 *
 * Skipping persistence on the primary will hold back its durable timestamp used for cross-replica
 * set write concern confirmation.
 *
 * First tests that writes can be majority confirmed without the primary by two secondaries.
 * Then tests that writes cannot be majority confirmed without the primary and only one secondary.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const checkTimestamps = (cmp, a, b) => {
    if (cmp === "gt") {
        assert.gt(timestampCmp(a, b),
                  0,
                  `Expected timestamp to compare greater: ${tojson(a)} <= ${tojson(b)}`);
        return;
    }
    assert.eq(cmp, "eq", "Expected 'cmp' to be one of {'eq', 'gt'}");
    assert.eq(
        timestampCmp(a, b), 0, `Expected timestamps to be equal: ${tojson(a)} != ${tojson(b)}`);
};

const rst = new ReplSetTest({name: jsTest.name(), nodes: 3});
rst.startSet();
// Make sure there are no election timeouts. This should prevent primary stepdown. Normally we would
// set the secondary node votes to 0, but that would affect the feature that is being tested.
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
const dbName = "testDB";
const collName = jsTest.name();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

assert.commandWorked(primaryDB.createCollection(collName, {writeConcern: {w: "majority"}}));

// Do a write and then fetch and save the durable and majority timestamps on the primary.
// Use {w: 3, j: true} write concern to make sure the timestamps are stable.
const res = assert.commandWorked(
    primaryColl.insert({_id: "writeAllDurable"}, {writeConcern: {w: 3, j: true}}));
const primaryReplSetStatus = assert.commandWorked(primary.adminCommand("replSetGetStatus"));
const primaryPreFailPointDurableTs = primaryReplSetStatus.optimes.durableOpTime.ts;
const primaryPreFailPointMajorityTs = primaryReplSetStatus.optimes.readConcernMajorityOpTime.ts;
jsTestLog("Primary's optimes (initializing): " + tojson(primaryReplSetStatus.optimes));
assert.neq(
    primaryPreFailPointDurableTs, null, `Expected ${tojson(primaryPreFailPointDurableTs)} != null`);
assert.neq(primaryPreFailPointMajorityTs,
           null,
           `Expected ${tojson(primaryPreFailPointMajorityTs)} != null`);
checkTimestamps("eq", primaryPreFailPointDurableTs, primaryPreFailPointMajorityTs);

// Configure the primary to stop moving the durable timestamp forward. The primary will no longer be
// able to contribute to moving the replica set's majority timestamp forward because the replica
// set's majority timestamp depends upon each member's durable timestamp,
const failPoint = configureFailPoint(primaryDB, "skipDurableTimestampUpdates");

try {
    // Perform some writes with majority write concern. The primary cannot confirm them, so success
    // means that the secondaries have the writes durably.
    jsTestLog("Writes majority confirmed by secondaries.");
    assert.commandWorked(
        primaryColl.insert({_id: "majority1"}, {writeConcern: {w: "majority", j: true}}));
    assert.commandWorked(
        primaryColl.insert({_id: "majority2"}, {writeConcern: {w: "majority", j: true}}));

    // Check that the primary's durable timestamp has not moved forward, but the majority point has.
    const primaryStatus = assert.commandWorked(primary.adminCommand("replSetGetStatus"));
    const primaryPostWritesDurableTs = primaryStatus.optimes.durableOpTime.ts;
    const primaryPostWritesMajorityTs = primaryStatus.optimes.readConcernMajorityOpTime.ts;
    jsTestLog("Primary's optimes (when 3 nodes): " + tojson(primaryStatus.optimes));
    checkTimestamps("eq", primaryPostWritesDurableTs, primaryPreFailPointDurableTs);
    checkTimestamps("gt", primaryPostWritesMajorityTs, primaryPreFailPointDurableTs);

    // Check that the secondaries' durable timestamps have moved forward.
    rst.getSecondaries().forEach(function(secondary) {
        const secondaryStatus = assert.commandWorked(secondary.adminCommand("replSetGetStatus"));
        const secondaryDurableTs = secondaryStatus.optimes.durableOpTime.ts;
        jsTestLog("One secondary's optimes (when 3 nodes): " + tojson(secondaryStatus.optimes));
        checkTimestamps("gt", secondaryDurableTs, primaryPreFailPointDurableTs);
    });

    // Shutdown a secondary so that there is no longer a majority able to confirm the durability of
    // a write.
    jsTestLog("Stopping one of the two secondaries");
    let secondaries = rst.getSecondaries();
    assert.eq(secondaries.length, 2);
    let stoppedSecondary = secondaries[0];
    let runningSecondary = secondaries[1];
    rst.stop(stoppedSecondary);

    // Now writes cannot reach majority without the primary. We will do {w: 2, j: false} writes to
    // get the writes on both remaining nodes. Then follow up with fsync commands againt the two
    // nodes to make sure the durable timestamps move forward if possible -- this will work only on
    // the secondary, the primary's durable timestamp will not move.
    jsTestLog("Writes cannot become majority confirmed.");
    assert.commandWorked(
        primaryColl.insert({_id: "noMajority1"}, {writeConcern: {w: 2, j: false}}));
    assert.commandWorked(
        primaryColl.insert({_id: "noMajority2"}, {writeConcern: {w: 2, j: false}}));

    jsTest.log("Force checkpoints to move the durable timestamps forward");
    assert.commandWorked(primary.adminCommand({fsync: 1}));
    assert.commandWorked(runningSecondary.adminCommand({fsync: 1}));

    // Check that the primary's durable and majority timestamps have not moved forward.
    const primaryReplStatus = assert.commandWorked(primary.adminCommand("replSetGetStatus"));
    const primaryPostFsyncDurableTs = primaryReplStatus.optimes.durableOpTime.ts;
    const primaryPostFsyncMajorityTs = primaryReplStatus.optimes.readConcernMajorityOpTime.ts;
    jsTestLog("Primary's optimes (when 2 nodes): " + tojson(primaryReplStatus.optimes));
    checkTimestamps("eq", primaryPostFsyncDurableTs, primaryPreFailPointDurableTs);
    checkTimestamps("eq", primaryPostFsyncMajorityTs, primaryPostWritesMajorityTs);

    // Check that the secondary's durable timestamp has moved forward, but the majority has not.
    // Durable timestamp is advanced by the journal flusher, not the fsync command. Keep retrying
    // until the secondary's durable timestamp has advanced past the majority timestamp.
    assert.soonNoExcept(() => {
        const secondaryStatus =
            assert.commandWorked(runningSecondary.adminCommand("replSetGetStatus"));
        const secondaryDurableTs = secondaryStatus.optimes.durableOpTime.ts;
        const secondaryMajorityTs = secondaryStatus.optimes.readConcernMajorityOpTime.ts;
        jsTestLog("Secondary's optimes (when 2 nodes): " + tojson(secondaryStatus.optimes));
        checkTimestamps("gt", secondaryDurableTs, primaryPostFsyncMajorityTs);
        checkTimestamps("eq", secondaryMajorityTs, primaryPostFsyncMajorityTs);
        return true;
    }, "Secondary durable timestamp has not advanced past majority", 30000, 1000);
} finally {
    // Turn off the failpoint before allowing the test to end, so nothing hangs while the server
    // shuts down or in post-test hooks.
    failPoint.off();
}

rst.stopSet();
})();
