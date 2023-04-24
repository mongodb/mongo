/**
 * Test aborting an index build after setup but before transitioning to in-progress.
 *
 * @tags: [
 *   requires_fcv_71,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');
load('jstests/libs/fail_point_util.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        },
    ]
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(testDB.getName());
const secondaryColl = secondaryDB.getCollection('test');

// Avoid optimization on empty colls.
assert.commandWorked(coll.insert({a: 1}));

// Pause the index builds on the primary, before transitioning to "kInProgress"
const failpointHangBeforeRunning = configureFailPoint(testDB, "hangBeforeRunningIndexBuild");

// Create the index and start the build. Set commitQuorum of 2 nodes explicitly, otherwise as only
// primary is voter, it would immediately commit.
const createIdx = IndexBuildTest.startIndexBuild(
    primary, coll.getFullName(), {a: 1}, {}, {}, /*commitQuorum: */ 2);

failpointHangBeforeRunning.wait();

const disableFailpointAfterDrop = startParallelShell(function() {
    // Wait until index has been aborted.
    checkLog.containsJson(db, 465611);

    // Unblock index build thread.
    assert.commandWorked(
        db.adminCommand({'configureFailPoint': 'hangBeforeRunningIndexBuild', 'mode': 'off'}));
}, testDB.getMongo().port);

assert.commandWorked(coll.dropIndexes());

const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build failing');

// Assert index does not exist.
IndexBuildTest.assertIndexes(coll, 1, ['_id_'], []);
IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_'], []);

disableFailpointAfterDrop();

rst.stopSet();
})();
