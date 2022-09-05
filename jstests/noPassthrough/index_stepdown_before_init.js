/**
 * Confirms that background index builds on a primary are aborted when the node steps down between
 * scheduling on the thread pool and initialization.
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

const res = assert.commandWorked(primary.adminCommand(
    {configureFailPoint: 'hangBeforeInitializingIndexBuild', mode: 'alwaysOn'}));
const failpointTimesEntered = res.count;

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

try {
    assert.commandWorked(primary.adminCommand({
        waitForFailPoint: "hangBeforeInitializingIndexBuild",
        timesEntered: failpointTimesEntered + 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    // Step down the primary.
    assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));
} finally {
    assert.commandWorked(primary.adminCommand(
        {configureFailPoint: 'hangBeforeInitializingIndexBuild', mode: 'off'}));
}

// Wait for the index build to stop.
IndexBuildTest.waitForIndexBuildToStop(testDB);

const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build being terminated');

// With both single-phase and two-phase index builds, a stepdown at this point will abort the index
// build because the builder thread cannot generate an optime. Wait for the command thread, not the
// IndexBuildsCoordinator, to report the index build as failed.
checkLog.containsJson(primary, 20449);

// Check that no new index has been created.  This verifies that the index build was aborted
// rather than successfully completed.
IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

const secondaryColl = rst.getSecondary().getCollection(coll.getFullName());
IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_']);

rst.stopSet();
})();
