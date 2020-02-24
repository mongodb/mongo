/**
 * Confirms that unique index builds on a primary are aborted when the node steps down during the
 * collection scan phase. This applies to both two phase and single phase index builds.
 * TODO: Handle JSON logs. See SERVER-45140
 * @tags: [requires_replication, requires_text_logs]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {},
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

IndexBuildTest.pauseIndexBuilds(primary);

const createIdx =
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {unique: true});

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

// Wait for the IndexBuildCoordinator thread, not the command thread, to report the index build
// as failed.
checkLog.contains(primary, /IndexBuildsCoordinatorMongod-0.*Index build failed: /);
IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

rst.stopSet();
})();
