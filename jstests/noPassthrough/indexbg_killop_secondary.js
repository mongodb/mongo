/**
 * Confirms that aborting a background index builds on a secondary does not leave node in an
 * inconsistent state.
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");
load("jstests/libs/log.js");  // for checkLog
load('jstests/noPassthrough/libs/index_build.js');

// This test triggers an unclean shutdown (an fassert), which may cause inaccurate fast counts.
TestData.skipEnforceFastCountOnValidate = true;

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary. This allows the primary to commit without waiting
            // for the secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
            slowms: 30000,  // Don't log slow operations on secondary. See SERVER-44821.
        },
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

let secondary = rst.getSecondary();
IndexBuildTest.pauseIndexBuilds(secondary);

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

// When the index build starts, find its op id.
let secondaryDB = secondary.getDB(testDB.getName());
const opId = IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

IndexBuildTest.assertIndexBuildCurrentOpContents(secondaryDB, opId, (op) => {
    jsTestLog('Inspecting db.currentOp() entry for index build: ' + tojson(op));
    assert.eq(coll.getFullName(),
              op.ns,
              'Unexpected ns field value in db.currentOp() result for index build: ' + tojson(op));
});

// Wait for the primary to complete the index build and replicate a commit oplog entry.
// "Index build: completed successfully"
checkLog.containsJson(primary, 20663);

// Kill the index build.
assert.commandWorked(secondaryDB.killOp(opId));

const gracefulIndexBuildFlag = FeatureFlagUtil.isEnabled(testDB, "IndexBuildGracefulErrorHandling");
if (!gracefulIndexBuildFlag) {
    // We expect this to crash the secondary because this error is not recoverable
    assert.soon(function() {
        return rawMongoProgramOutput().search(/Fatal assertion.*(51101)/) >= 0;
    });
} else {
    // Expect the secondary to crash. Depending on timing, this can be either because the secondary
    // was waiting for a primary abort when a 'commitIndexBuild' is applied, or because the build
    // fails and tries to request an abort while a 'commitIndexBuild' is being applied.
    assert.soon(function() {
        return rawMongoProgramOutput().search(/Fatal assertion.*(7329403|7329407)/) >= 0;
    });
}

// After restarting the secondary, expect that the index build completes successfully.
rst.stop(secondary.nodeId, undefined, {forRestart: true, allowedExitCode: MongoRunner.EXIT_ABORT});
rst.start(secondary.nodeId, undefined, true /* restart */);

secondary = rst.getSecondary();
secondaryDB = secondary.getDB(testDB.getName());

// Wait for the restarted secondary node to reach SECONDARY state again.
rst.waitForState(secondary, ReplSetTest.State.SECONDARY);

// Wait for the index build to complete on all nodes.
rst.awaitReplication();

// Expect successful createIndex command invocation in parallel shell. A new index should be present
// on the primary and secondary.
createIdx();

IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);

// Check that index was created on the secondary despite the attempted killOp().
const secondaryColl = secondaryDB.getCollection(coll.getName());
IndexBuildTest.assertIndexes(secondaryColl, 2, ['_id_', 'a_1']);

rst.stopSet();
})();
