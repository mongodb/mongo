/**
 * Confirms that background index builds on a primary can be aborted using killop
 * on the client connection operation when the IndexBuildsCoordinator is enabled.
 * @tags: [requires_replication]
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
    ],
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

const res = assert.commandWorked(primary.adminCommand(
    {configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'alwaysOn'}));
const failpointTimesEntered = res.count;

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

try {
    assert.commandWorked(primary.adminCommand({
        waitForFailPoint: "hangAfterInitializingIndexBuild",
        timesEntered: failpointTimesEntered + 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    // When the index build starts, find its op id. This will be the op id of the client
    // connection, not the thread pool task managed by IndexBuildsCoordinatorMongod.
    const filter = {"desc": {$regex: /conn.*/}};
    const opId = IndexBuildTest.waitForIndexBuildToStart(testDB, coll.getName(), 'a_1', filter);

    // Kill the index build and wait for it to abort.
    assert.commandWorked(testDB.killOp(opId));
    checkLog.containsJson(primary, 4656003);
} finally {
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'off'}));
}

// Wait for the index build to stop.
IndexBuildTest.waitForIndexBuildToStop(testDB);

const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build being terminated');

checkLog.containsJson(primary, 20443);

// Check that no new index has been created.  This verifies that the index build was aborted
// rather than successfully completed.
IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

rst.stopSet();
})();
