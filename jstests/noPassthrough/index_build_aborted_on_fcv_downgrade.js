/**
 * Ensures that index builds are aborted when setFCV causes an FCV downgrade, and that during that
 * period new index builds are blocked.
 *
 * TODO (SERVER-68290): remove test when removing index build abort on FCV downgrade and reintroduce
 * "jstests/noPassthrough/index_downgrade_fcv.js".
 *
 * @tags: [
 *   requires_fcv_71,
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

function startParallelSetFcv(primary, expectedError) {
    return startParallelShell(
        funWithArgs(function(expectedError) {
            // Should fail due to failAfterReachingTransitioningState.
            assert.commandFailedWithCode(
                db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
                expectedError);
        }, expectedError), primary.port);
}

function testIndexBuildsBlockedAndAbortedOnFcvDowngrade() {
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

    const dbName = 'test';
    const collName = 'coll';
    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const primaryColl = primaryDB.getCollection(collName);

    assert.commandWorked(primaryColl.insert({a: 1}));

    rst.awaitReplication();

    // Clear log to ensure checkLog does not see unrelated log entries.
    assert.commandWorked(primaryDB.adminCommand({clearLog: 'global'}));

    // Hang an index build in the commit phase, to later check that FCV downgrade waits on a
    // commiting index build.
    const hangIndexBuildBeforeCommit = configureFailPoint(primary, "hangIndexBuildBeforeCommit");
    const createIdxCommit = IndexBuildTest.startIndexBuild(
        primary, primaryColl.getFullName(), {c: 1}, null, [ErrorCodes.IndexBuildAborted]);
    const commitBuildUUID =
        IndexBuildTest
            .assertIndexesSoon(primaryColl, 2, ['_id_'], ['c_1'], {includeBuildUUIDs: true})['c_1']
            .buildUUID;
    hangIndexBuildBeforeCommit.wait();

    // Setup index build to be aborted by the FCV downgrade.
    const hangAfterInitializingIndexBuild =
        configureFailPoint(primary, "hangAfterInitializingIndexBuild");
    const createIdxAborted = IndexBuildTest.startIndexBuild(
        primary, primaryColl.getFullName(), {a: 1}, null, [ErrorCodes.IndexBuildAborted]);

    const abortedBuildUUID =
        IndexBuildTest
            .assertIndexesSoon(
                primaryColl, 3, ['_id_'], ['a_1', 'c_1'], {includeBuildUUIDs: true})['a_1']
            .buildUUID;

    hangAfterInitializingIndexBuild.wait();

    const hangAfterBlockingIndexBuildsForFcvDowngrade =
        configureFailPoint(primary, "hangAfterBlockingIndexBuildsForFcvDowngrade");

    // Ensure index build block and abort happens during the FCV transitioning state.
    configureFailPoint(primary, "failAfterReachingTransitioningState");

    const awaitSetFcv = startParallelSetFcv(primary, 7555200);

    hangAfterBlockingIndexBuildsForFcvDowngrade.wait();

    // Start an index build while the block is active.
    const createIdxBlocked =
        IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), {b: 1});
    // "Index build: new index builds are blocked, waiting".
    checkLog.containsJson(primary, 7738700);

    hangAfterBlockingIndexBuildsForFcvDowngrade.off();

    // "About to abort all index builders running".
    assert.soon(() => checkLog.checkContainsWithCountJson(primary,
                                                          7738702,
                                                          {
                                                              reason: function(reason) {
                                                                  return reason.startsWith(
                                                                      "FCV downgrade in progress");
                                                              }
                                                          },
                                                          /*count=*/ 1));

    // "Index build: joined after abort".
    checkLog.containsJson(primary, 20655, {
        buildUUID: function(uuid) {
            return uuid && uuid["uuid"]["$uuid"] === extractUUIDFromObject(abortedBuildUUID);
        }
    });

    checkLog.containsJson(primary, 4725201, {
        indexBuilds: function(uuidArray) {
            return uuidArray && uuidArray.length == 1 &&
                uuidArray[0]["uuid"]["$uuid"] === extractUUIDFromObject(commitBuildUUID);
        }
    });
    hangIndexBuildBeforeCommit.off();
    hangAfterInitializingIndexBuild.off();

    jsTestLog("Waiting for threads to join");
    createIdxAborted();
    createIdxCommit();
    awaitSetFcv();
    createIdxBlocked();

    // The index build started before the FCV downgrade should have been aborted, while the build
    // started while the index build block was in place should have succeeded. The index build which
    // was already in the commit phase when the FCV downgrade took place should also have completed.
    IndexBuildTest.assertIndexesSoon(primaryColl, 3, ['_id_', 'b_1', 'c_1']);

    rst.stopSet();
}

function testIndexBuildsUnblockedOnSetFcvCommandInterrupt() {
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

    const dbName = 'test';
    const collName = 'coll';
    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const primaryColl = primaryDB.getCollection(collName);

    assert.commandWorked(primaryColl.insert({a: 1}));

    rst.awaitReplication();

    // Clear log to ensure checkLog does not see unrelated log entries.
    assert.commandWorked(primaryDB.adminCommand({clearLog: 'global'}));

    const hangAfterBlockingIndexBuildsForFcvDowngrade =
        configureFailPoint(primary, "hangAfterBlockingIndexBuildsForFcvDowngrade");

    // Verify the index builds are unblocked in case the setFCV command is interrupted.
    const awaitKilledSetFcv = startParallelSetFcv(primary, ErrorCodes.Interrupted);
    hangAfterBlockingIndexBuildsForFcvDowngrade.wait();

    const pipeline = [
        {$currentOp: {allUsers: true, idleConnections: true}},
        {$match: {"command.setFeatureCompatibilityVersion": {$exists: true}}}
    ];
    const curOpResult =
        primary.getDB("admin").aggregate(pipeline, {readConcern: {level: "local"}}).toArray();
    assert.eq(curOpResult.length, 1);
    assert.commandWorked(primary.adminCommand({killOp: 1, op: curOpResult[0].opid}));

    awaitKilledSetFcv();

    const createIdxBlocked =
        IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), {b: 1});
    createIdxBlocked();
    IndexBuildTest.assertIndexesSoon(primaryColl, 2, ['_id_', 'b_1']);

    rst.stopSet();
}

testIndexBuildsBlockedAndAbortedOnFcvDowngrade();
testIndexBuildsUnblockedOnSetFcvCommandInterrupt();
