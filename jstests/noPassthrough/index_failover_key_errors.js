/**
 * Confirms that an index build is aborted after step-up by a new primary when there are key
 * generation errors. This test orchestrates a scenario such that a secondary detects (and
 * ignores) an indexing error. After step-up, the node retries indexing the skipped record before
 * completing. The expected result is that the node, now primary, aborts the index build for the
 * entire replica set.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {},
    ],
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

// Insert a document that cannot be indexed because it causes a CannotIndexParallelArrays error
// code.
const badDoc = {
    _id: 0,
    a: [0, 1],
    b: [2, 3]
};
assert.commandWorked(coll.insert(badDoc));

// Start an index build on primary and secondary, but prevent the primary from scanning the
// collection. Do not stop the secondary; intentionally let it scan the invalid document, which we
// will resolve later.

// We are using this fail point to pause the index build before it starts the collection scan.
// This is important for this test because we are mutating the collection state before the index
// builder is able to observe the invalid document.
// By comparison, IndexBuildTest.pauseIndexBuilds() stalls the index build in the middle of the
// collection scan.
assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'alwaysOn'}));
const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1, b: 1});

// Wait for the index build to start on the secondary.
const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(testDB.getName());
const secondaryColl = secondaryDB.getCollection(coll.getName());
IndexBuildTest.waitForIndexBuildToStart(secondaryDB);
rst.awaitReplication();
IndexBuildTest.assertIndexes(secondaryColl, 2, ["_id_"], ["a_1_b_1"], {includeBuildUUIDs: true});

// Step down the primary.
const stepDown = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({"replSetStepDown": 60, "force": true}));
}, primary.port);

// Expect a failed createIndex command invocation in the parallel shell due to stepdown even though
// the index build will continue in the background.
const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build being terminated');
checkLog.containsJson(primary, 20444);

// Wait for stepdown to complete.
stepDown();

// Unblock the index build on the old primary.
assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'off'}));

const newPrimary = rst.getPrimary();
const newPrimaryDB = newPrimary.getDB('test');
const newPrimaryColl = newPrimaryDB.getCollection('test');

// Ensure the old primary doesn't take over again.
assert.neq(primary.port, newPrimary.port);

// The index should not be present on the old primary after processing the abortIndexBuild oplog
// entry from the new primary.
jsTestLog("waiting for index build to stop on old primary");
IndexBuildTest.waitForIndexBuildToStop(testDB);
rst.awaitReplication();
IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

// Check that index was not built on the new primary.
jsTestLog("waiting for index build to stop on new primary");
IndexBuildTest.waitForIndexBuildToStop(newPrimaryDB);
IndexBuildTest.assertIndexes(newPrimaryColl, 1, ['_id_']);

rst.stopSet();
})();
