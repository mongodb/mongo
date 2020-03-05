/**
 * Tests that performing a stepdown on the primary during a dropDatabase command doesn't result in
 * any crashes when setting the drop-pending flag back to false.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const dbName = "test";
const collName = "coll";

const replSet = new ReplSetTest({nodes: 2});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
if (!IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    jsTestLog('Two phase index builds not enabled, skipping test.');
    replSet.stopSet();
    return;
}

let testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

var bulk = testColl.initializeUnorderedBulkOp();
for (var i = 0; i < 5; ++i) {
    bulk.insert({x: i});
}
assert.commandWorked(bulk.execute());
replSet.awaitReplication();

IndexBuildTest.pauseIndexBuilds(testDB.getMongo());
const awaitIndexBuild = IndexBuildTest.startIndexBuild(
    testDB.getMongo(), testColl.getFullName(), {x: 1}, {}, [ErrorCodes.IndexBuildAborted]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "x_1");

const failpoint = "dropDatabaseHangAfterWaitingForIndexBuilds";
assert.commandWorked(primary.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn"}));

// Run the dropDatabase command and stepdown the primary while it is running.
const awaitDropDatabase = startParallelShell(() => {
    assert.commandFailedWithCode(db.dropDatabase(), ErrorCodes.InterruptedDueToReplStateChange);
}, testDB.getMongo().port);

checkLog.containsJson(primary, 4612302);
IndexBuildTest.resumeIndexBuilds(testDB.getMongo());

awaitIndexBuild();

// Ensure the dropDatabase command has begun before stepping down.
checkLog.containsJson(primary, 4612300);

assert.commandWorked(testDB.adminCommand({replSetStepDown: 60, force: true}));
replSet.waitForState(primary, ReplSetTest.State.SECONDARY);

assert.commandWorked(primary.adminCommand({configureFailPoint: failpoint, mode: "off"}));

awaitDropDatabase();

const newPrimary = replSet.getPrimary();
assert(primary.port != newPrimary.port);

// The {x: 1} index was aborted and should not be present even though the dropDatabase command was
// interrupted. Only the _id index will exist.
let indexesRes = assert.commandWorked(newPrimary.getDB(dbName).runCommand({listIndexes: collName}));
assert.eq(1, indexesRes.cursor.firstBatch.length);

indexesRes =
    assert.commandWorked(replSet.getSecondary().getDB(dbName).runCommand({listIndexes: collName}));
assert.eq(1, indexesRes.cursor.firstBatch.length);

// Run dropDatabase on the new primary. The secondary (formerly the primary) should be able to
// drop the database too.
newPrimary.getDB(dbName).dropDatabase();
replSet.awaitReplication();

replSet.stopSet();
})();
