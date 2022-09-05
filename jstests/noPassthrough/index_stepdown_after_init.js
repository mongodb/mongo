/**
 * Confirms that background index builds on a primary are aborted when the node steps down between
 * the initialization and collection scan phases.
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

checkLog.containsJson(primary, 20384, {
    namespace: coll.getFullName(),
    properties: (desc) => {
        return desc.name === 'a_1';
    },
});

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

// With two phase index builds, a stepdown will not abort the index build, which should complete
// after the node becomes primary again.
rst.awaitReplication();
IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);

const secondaryColl = rst.getSecondary().getCollection(coll.getFullName());
IndexBuildTest.assertIndexes(secondaryColl, 2, ['_id_', 'a_1']);

rst.stopSet();
})();
