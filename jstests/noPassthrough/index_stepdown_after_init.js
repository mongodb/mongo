/**
 * Confirms that background index builds on a primary are aborted when the node steps down between
 * the initialization and collection scan phases.
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load('jstests/libs/check_log.js');
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

assert.commandWorked(primary.adminCommand(
    {configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'alwaysOn'}));

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

checkLog.contains(
    primary, 'index build: starting on ' + coll.getFullName() + ' properties: { v: 2, key: { a:');

try {
    // Step down the primary.
    assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));
} finally {
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'off'}));
}

// Wait for the index build to stop.
IndexBuildTest.waitForIndexBuildToStop(testDB);

const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build being terminated');

checkLog.contains(primary, 'IndexBuildAborted: Index build aborted: ');

// Check that no new index has been created.  This verifies that the index build was aborted
// rather than successfully completed.
IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

rst.stopSet();
})();
