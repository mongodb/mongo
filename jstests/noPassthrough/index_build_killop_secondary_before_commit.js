/**
 * Sends a killop to an index build on a secondary node before it commits and confirms that:
 * - the index build is canceled on all nodes if killop is before voting for commit.
 * - the killop results in the secondary crashing if the killop is after voting for commit.
 *
 * @tags: [
 *   requires_fcv_71,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");
load('jstests/noPassthrough/libs/index_build.js');

TestData.skipEnforceFastCountOnValidate = true;

function killopIndexBuildOnSecondaryOnFailpoint(rst, failpointName, shouldSucceed) {
    const primary = rst.getPrimary();
    const testDB = primary.getDB('test');
    const coll = testDB.getCollection('test');
    let secondary = rst.getSecondary();
    let secondaryDB = secondary.getDB(testDB.getName());

    coll.drop();
    assert.commandWorked(coll.insert({a: 1}));

    // Pause the index build on the primary so that it does not commit.
    IndexBuildTest.pauseIndexBuilds(primary);
    IndexBuildTest.pauseIndexBuilds(secondary);

    let expectedErrors = shouldSucceed ? ErrorCodes.IndexBuildAborted : [];

    const fp = configureFailPoint(secondary, failpointName);
    const createIdx =
        IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {}, expectedErrors);

    // When the index build starts, find its op id.
    const opId = IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

    IndexBuildTest.assertIndexBuildCurrentOpContents(secondaryDB, opId, (op) => {
        jsTestLog('Inspecting db.currentOp() entry for index build: ' + tojson(op));
        assert.eq(
            coll.getFullName(),
            op.ns,
            'Unexpected ns field value in db.currentOp() result for index build: ' + tojson(op));
    });

    // Resume index build to the desired failpoint, and kill it.
    IndexBuildTest.resumeIndexBuilds(secondary);
    fp.wait();
    assert.commandWorked(secondaryDB.killOp(opId));
    fp.off();

    if (shouldSucceed) {
        // "attempting to abort index build".
        checkLog.containsJson(primary, 4656010);

        IndexBuildTest.resumeIndexBuilds(primary);
        // "Index build: joined after abort".
        checkLog.containsJson(primary, 20655);

        // Wait for the index build abort to replicate.
        rst.awaitReplication();

        // Expect the index build to fail and for the index to not exist on either node.
        createIdx();

        IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

        const secondaryColl = secondaryDB.getCollection(coll.getName());
        IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_']);
    } else {
        // We expect this to crash the secondary because this error is not recoverable.
        assert.soon(function() {
            return rawMongoProgramOutput().search(/Fatal assertion.*(51101)/) >= 0;
        });

        // After restarting the secondary, expect that the index build completes successfully.
        rst.stop(secondary.nodeId,
                 undefined,
                 {forRestart: true, allowedExitCode: MongoRunner.EXIT_ABORT});
        rst.start(secondary.nodeId, undefined, true /* restart */);

        secondary = rst.getSecondary();
        secondaryDB = secondary.getDB(testDB.getName());

        IndexBuildTest.resumeIndexBuilds(primary);
        // Expect the index build to succeed.
        createIdx();

        // Wait for the index build commit to replicate.
        rst.awaitReplication();
        IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);

        const secondaryColl = secondaryDB.getCollection(coll.getName());
        IndexBuildTest.assertIndexes(secondaryColl, 2, ['_id_', 'a_1']);
    }
}

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary, but allow it to participate in commitQuorum.
            rsConfig: {
                priority: 0,
            },
            slowms: 30000,  // Don't log slow operations on secondary. See SERVER-44821.
        },
    ]
});
rst.startSet();
rst.initiate();

// Kill the build before it has voted for commit.
jsTestLog("killOp index build on secondary before vote for commit readiness");
killopIndexBuildOnSecondaryOnFailpoint(
    rst, 'hangAfterIndexBuildFirstDrain', /*shouldSucceed*/ true);

jsTestLog("killOp index build on secondary after vote for commit readiness");
killopIndexBuildOnSecondaryOnFailpoint(
    rst, 'hangIndexBuildAfterSignalPrimaryForCommitReadiness', /*shouldSucceed*/ false);

rst.stopSet();
})();
