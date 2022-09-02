/**
 * Confirms that an index build is committed after step-up by a new primary when there are key
 * generation errors that are eventually resolved. This test orchestrates a scenario such that a
 * secondary detects (and ignores) an indexing error while performing a collection scan, but before
 * receiving an update that resolves the error. After step-up, the node retries indexing the skipped
 * record before completing. The expected result is that the node, now primary, commits the index
 * build for the entire replica set.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    // We want at least two electable nodes.
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

// We are using this fail point to pause the index build before it starts the
// collection scan. This is important for this test because we are mutating the collection state
// before the index builder is able to observe the invalid geo document. By comparison,
// IndexBuildTest.pauseIndexBuilds() stalls the index build in the middle of the collection scan.
assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'alwaysOn'}));
const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1, b: 1});

// Wait for the index build to start on the secondary.
const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(testDB.getName());
const secondaryColl = secondaryDB.getCollection(coll.getName());
IndexBuildTest.waitForIndexBuildToStart(secondaryDB);
IndexBuildTest.assertIndexes(secondaryColl, 2, ["_id_"], ["a_1_b_1"], {includeBuildUUIDs: true});

// Resolve the key generation error so that the index build succeeds on the primary before it scans
// the invalid document.
assert.commandWorked(coll.update({_id: 0}, {a: 1, b: 1}));

// Unblock the index build on the old primary during the collection scanning phase, and block after
// the collection scan phase.
assert.commandWorked(testDB.adminCommand(
    {configureFailPoint: 'hangAfterIndexBuildDumpsInsertsFromBulk', mode: 'alwaysOn'}));
assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'off'}));

// Step down the primary.
const stepDown = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({"replSetStepDown": 60, "force": true}));
}, primary.port);

// Expect a failed createIndex command invocation in the parallel shell due to stepdown even though
// the index build will continue in the background.
const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build being terminated');
checkLog.containsJson(primary, 20444);

// Unblock the index build on the old primary during the collection scanning phase, this lets
// stepdown complete.
assert.commandWorked(testDB.adminCommand(
    {configureFailPoint: 'hangAfterIndexBuildDumpsInsertsFromBulk', mode: 'off'}));

// Wait for stepdown to complete.
stepDown();

const newPrimary = rst.getPrimary();
const newPrimaryDB = newPrimary.getDB('test');
const newPrimaryColl = newPrimaryDB.getCollection('test');

// Ensure the old primary doesn't take over again.
assert.neq(primary.port, newPrimary.port);

// A new index should be present on the old primary after processing the commitIndexBuild oplog
// entry from the new primary.
IndexBuildTest.waitForIndexBuildToStop(testDB);
IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1_b_1']);

// Check that index was created on the new primary.
IndexBuildTest.waitForIndexBuildToStop(newPrimaryDB);
IndexBuildTest.assertIndexes(newPrimaryColl, 2, ['_id_', 'a_1_b_1']);

rst.stopSet();
})();
