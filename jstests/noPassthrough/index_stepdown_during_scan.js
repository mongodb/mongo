/**
 * Confirms that background index builds on a primary are aborted when the node steps down during
 * the collection scan phase.
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

IndexBuildTest.pauseIndexBuilds(primary);

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

// When the index build starts, find its op id.
const opId = IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'a_1');

IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId);

try {
    // Step down the primary.
    assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));
} finally {
    IndexBuildTest.resumeIndexBuilds(primary);
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
