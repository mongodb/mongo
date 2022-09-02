/**
 * Confirms that index builds on a stepped down primary are not aborted and will
 * wait for a commitIndexBuild from the new primary before committing.
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    // We want at least two electable nodes.
    nodes: [{}, {}, {arbiter: true}],
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

// Start index build on primary, but prevent it from finishing.
IndexBuildTest.pauseIndexBuilds(primary);
const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

// Wait for the index build to start on the secondary.
const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(testDB.getName());
const secondaryColl = secondaryDB.getCollection(coll.getName());
IndexBuildTest.waitForIndexBuildToStart(secondaryDB);
const indexMap =
    IndexBuildTest.assertIndexes(secondaryColl, 2, ["_id_"], ["a_1"], {includeBuildUUIDs: true});
const indexBuildUUID = indexMap['a_1'].buildUUID;

// Index build should be present in the config.system.indexBuilds collection.
assert(primary.getCollection('config.system.indexBuilds').findOne({_id: indexBuildUUID}));

const newPrimary = rst.getSecondary();
const newPrimaryDB = secondaryDB;
const newPrimaryColl = secondaryColl;

// Step down the primary.
// Expect failed createIndex command invocation in parallel shell due to stepdown even though
// the index build will continue in the background.
assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));
const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build being terminated');
checkLog.containsJson(primary, 20444);

// Unblock the index build on the old primary during the collection scanning phase.
// This index build will not complete because it has to wait for a commitIndexBuild oplog
// entry.
IndexBuildTest.resumeIndexBuilds(primary);
checkLog.containsJson(primary, 3856203);

// Step up the new primary.
rst.stepUp(newPrimary);

// A new index should be present on the old primary after processing the commitIndexBuild oplog
// entry from the new primary.
IndexBuildTest.waitForIndexBuildToStop(testDB);
IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);

// Check that index was created on the new primary.
IndexBuildTest.waitForIndexBuildToStop(newPrimaryDB);
IndexBuildTest.assertIndexes(newPrimaryColl, 2, ['_id_', 'a_1']);

// Index build should be removed from the config.system.indexBuilds collection.
assert.isnull(newPrimary.getCollection('config.system.indexBuilds').findOne({_id: indexBuildUUID}));

rst.stopSet();
})();
