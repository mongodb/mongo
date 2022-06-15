/**
 * Test that a confirmed write against a primary with oplog holes behind it when a crash occurs will
 * be truncated on startup recovery.
 *
 * There must be more than 1 voting node, otherwise the write concern behavior changes to waiting
 * for no holes for writes with {j: true} write concern, and no confirmed writes will be truncated.
 *
 * @tags: [
 *   requires_replication,
 *   # The primary is restarted and must retain its data.
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({name: jsTest.name(), nodes: 2});
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

const failPoint = configureFailPoint(primaryDB,
                                     "hangAfterCollectionInserts",
                                     {collectionNS: primaryColl.getFullName(), first_id: "b"});
let ps = undefined;
try {
    // Hold back the durable timestamp by leaving an uncommitted transaction hanging.

    TestData.dbName = dbName;
    TestData.collName = collName;

    ps = startParallelShell(() => {
        jsTestLog("Insert a document that will hang before the insert completes.");
        // Crashing the server while this command is running may cause the parallel shell code to
        // error and stop executing. We will therefore ignore the result of this command and
        // parallel shell. Test correctness is guaranteed by waiting for the failpoint this command
        // hits.
        db.getSiblingDB(TestData.dbName)[TestData.collName].insert({_id: "b"});
    }, primary.port);

    jsTest.log("Wait for async insert to hit the failpoint.");
    failPoint.wait();

    // Execute an insert with confirmation that it made it to disk ({j: true});
    //
    // The primary's durable timestamp should be pinned by the prior hanging uncommitted write. So
    // this second write will have an oplog hole behind it and will be truncated after a crash.
    assert.commandWorked(
        primaryColl.insert({_id: "writeAfterHole"}, {writeConcern: {w: 1, j: true}}));

    const findResult = primaryColl.findOne({_id: "writeAfterHole"});
    assert.eq(findResult, {"_id": "writeAfterHole"});

    jsTest.log("Force a checkpoint so the primary has data on startup recovery after a crash");
    assert.commandWorked(primary.adminCommand({fsync: 1}));

    // Crash and restart the primary, which should truncate the second successful write, because
    // the first write never committed and left a hole in the oplog.
    rst.stop(primary, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
} catch (error) {
    // Turn off the failpoint before allowing the test to end, so nothing hangs while the server
    // shuts down or in post-test hooks.
    failPoint.off();
    throw error;
} finally {
    if (ps) {
        ps({checkExitSuccess: false});
    }
}

rst.start(primary);

// Wait for the restarted node to complete startup recovery and start accepting user requests.
// Note: no new primary will be elected because of the high election timeout set on the replica set.
assert.soonNoExcept(function() {
    const nodeState = assert.commandWorked(primary.adminCommand("replSetGetStatus")).myState;
    return nodeState == ReplSetTest.State.SECONDARY;
});

// Confirm that the write with the oplog hold behind it is now gone (truncated) as expected.
primary.setSecondaryOk();
const find = primary.getDB(dbName).getCollection(collName).findOne({_id: "writeAfterHole"});
assert.eq(find, null);

rst.stopSet();
})();
