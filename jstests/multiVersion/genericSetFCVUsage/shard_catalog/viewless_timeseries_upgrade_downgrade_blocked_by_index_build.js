/**
 * TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.
 *
 * Tests that setFCV is blocked by in-progress index builds on timeseries collections during
 * viewless timeseries upgrade/downgrade, for both replica sets and sharded clusters.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {assertCommandWorkedInParallelShell} from "jstests/libs/parallel_shell_helpers.js";

if (lastLTSFCV != "8.0") {
    jsTest.log.info("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

function runTest(db, hangIndexBuildConn, startFCV, targetFCV) {
    const coll = db.ts;

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: startFCV, confirm: true}));

    assert(coll.drop());
    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));

    // Force a two-phase index build to hang.
    assert.commandWorked(coll.insertOne({t: ISODate(), m: 1}));
    IndexBuildTest.pauseIndexBuilds(hangIndexBuildConn);
    const awaitBuild = assertCommandWorkedInParallelShell(db.getMongo(), db, {
        createIndexes: coll.getName(),
        indexes: [{key: {m: 1}, name: "m_1"}],
    });
    IndexBuildTest.waitForIndexBuildToStart(hangIndexBuildConn.getDB(db.getName()), coll.getName(), "m_1");

    // setFCV fails while an index build is in progress.
    assert.commandFailedWithCode(
        db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}),
        ErrorCodes.BackgroundOperationInProgressForNamespace,
    );

    // The FCV transition succeeds after the index build finishes.
    IndexBuildTest.resumeIndexBuilds(hangIndexBuildConn);
    awaitBuild();
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
}

// Replica set
{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const db = rst.getPrimary().getDB("test");
    runTest(db, rst.getPrimary(), latestFCV, lastLTSFCV);
    runTest(db, rst.getPrimary(), lastLTSFCV, latestFCV);
    rst.stopSet();
}

// Sharded cluster
{
    const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
    const db = st.s.getDB("test");
    runTest(db, st.rs0.getPrimary(), latestFCV, lastLTSFCV);
    runTest(db, st.rs0.getPrimary(), lastLTSFCV, latestFCV);
    st.stop();
}
