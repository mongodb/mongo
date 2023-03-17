/**
 * Confirms that aborting a background index builds on a secondary before the primary commits
 * results in a consistent state with no crashing.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");
load('jstests/noPassthrough/libs/index_build.js');

// This test triggers an unclean shutdown (an fassert), which may cause inaccurate fast counts.
TestData.skipEnforceFastCountOnValidate = true;

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Lower the primary, but allow the secondary to vote and participate in commitQuorum.
            rsConfig: {
                priority: 0,
            },
            slowms: 30000,  // Don't log slow operations on secondary. See SERVER-44821.
        },
    ]
});
const nodes = rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let primaryDB = primary.getDB('test');
let primaryColl = primaryDB.getCollection('test');

assert.commandWorked(primaryColl.insert({a: 1}));

let secondary = rst.getSecondary();
IndexBuildTest.pauseIndexBuilds(secondary);

const gracefulIndexBuildFlag =
    FeatureFlagUtil.isEnabled(primaryDB, "IndexBuildGracefulErrorHandling");

const createIdx = (gracefulIndexBuildFlag)
    ? IndexBuildTest.startIndexBuild(
          primary, primaryColl.getFullName(), {a: 1}, {}, ErrorCodes.IndexBuildAborted)
    : IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), {a: 1});

// When the index build starts, find its op id.
let secondaryDB = secondary.getDB(primaryDB.getName());
const opId = IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

IndexBuildTest.assertIndexBuildCurrentOpContents(secondaryDB, opId, (op) => {
    jsTestLog('Inspecting db.currentOp() entry for index build: ' + tojson(op));
    assert.eq(primaryColl.getFullName(),
              op.ns,
              'Unexpected ns field value in db.currentOp() result for index build: ' + tojson(op));
});

// Kill the index build on the secondary. With the feature flag enabled, this should signal the
// primary to abort the index build.
assert.commandWorked(secondaryDB.killOp(opId));

if (!gracefulIndexBuildFlag) {
    // We expect this to crash the secondary because this error is not recoverable
    assert.soon(function() {
        return rawMongoProgramOutput().search(/Fatal assertion.*(51101)/) >= 0;
    });

    // After restarting the secondary, expect that the index build completes successfully.
    rst.stop(
        secondary.nodeId, undefined, {forRestart: true, allowedExitCode: MongoRunner.EXIT_ABORT});
    rst.start(secondary.nodeId, undefined, true /* restart */);

} else {
    // Expect the secondary to successfully prevent the primary from committing the index build.
    checkLog.containsJson(secondary, 20655);
}

primary = rst.getPrimary();
primaryDB = primary.getDB('test');
primaryColl = primaryDB.getCollection('test');

secondary = rst.getSecondary();
secondaryDB = secondary.getDB(primaryDB.getName());
const secondaryColl = secondaryDB.getCollection(primaryColl.getName());

// Wait for the index build to complete on all nodes.
rst.awaitReplication();

// Expect successful createIndex command invocation in parallel shell. A new index should be present
// on the primary and secondary.
createIdx();

if (!gracefulIndexBuildFlag) {
    // Check that index was created despite the attempted killOp().
    IndexBuildTest.assertIndexes(primaryColl, 2, ['_id_', 'a_1']);
    IndexBuildTest.assertIndexes(secondaryColl, 2, ['_id_', 'a_1']);
} else {
    // Check that index was aborted by the killOp().
    IndexBuildTest.assertIndexes(primaryColl, 1, ['_id_']);
    IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_']);
}

rst.stopSet();
})();
