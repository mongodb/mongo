/**
 * Test that when refreshing the timestamp in the sessions collection fails due to errors (like ShardNotFound or
 * WriteConcernErrors), the sessions pending to be ended are still processed.
 * @tags: [assumes_balancer_off]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test makes assertions about the number of sessions, which are not compatible with
// implicit sessions.
TestData.disableImplicitSessions = true;

const refreshCommand = {refreshLogicalSessionCacheNow: 1};
const startSessionCommand = {startSession: 1};

function runTest(conn, initialSessionsCount, enableFailpointsFunc, disableFailpointsFunc, destroyFunc) {
    const admin = conn.getDB("admin");
    const config = conn.getDB("config");
    function getSessionCacheStats() {
        return admin.serverStatus().logicalSessionRecordCache;
    }

    jsTest.log.info("Initial refresh");
    // Initial refresh to ensure clean start.
    assert.commandWorked(admin.runCommand(refreshCommand), "failed initial refresh");
    const initialSessions = config.system.sessions.count();
    assert.eq(
        initialSessions,
        initialSessionsCount,
        "should have " + initialSessionsCount + " initial session records",
    );

    jsTest.log.info("Create 10 sessions");
    let sessions = [];
    for (let i = 0; i < 10; i++) {
        const res = admin.runCommand(startSessionCommand);
        assert.commandWorked(res, "unable to start session");
        sessions.push(res);
    }

    jsTest.log.info("Refresh after creating sessions");
    assert.commandWorked(admin.runCommand(refreshCommand), "failed to refresh after creating sessions");
    assert.eq(config.system.sessions.count() - initialSessions, 10, "should have 10 session records");
    const statsAfterCreatingSessions = getSessionCacheStats();
    jsTest.log.info("Session cache stats after creating 10 entries: " + tojson(statsAfterCreatingSessions));
    assert.eq(
        statsAfterCreatingSessions.lastSessionsCollectionJobEntriesEnded,
        0,
        "should not have ended any sessions",
    );
    assert.eq(
        statsAfterCreatingSessions.lastSessionsCollectionJobEntriesRefreshed,
        10,
        "Should have updated timestamps for 10 sessions created",
    );

    jsTest.log.info("End three sessions");

    // End the first three sessions.
    const endSessionsBatch1 = sessions.slice(0, 3).map((s) => s.id);
    assert.commandWorked(admin.runCommand({endSessions: endSessionsBatch1}), "failed to end first batch of sessions");
    jsTest.log.info("start one more session");
    const eleventhSession = admin.runCommand(startSessionCommand);
    assert.commandWorked(eleventhSession, "unable to start session");

    // The failAllUpdates failpoint will cause the refreshSessions operation to fail
    // since it uses update operations to refresh session lastUse timestamps.
    jsTest.log.info("Enable failpoint that will call subsequent refreshes to fail when updating session timestamp");
    enableFailpointsFunc();

    jsTest.log.info("Refresh after enabling failpoint");
    // Attempt refresh - should fail due to the failpoint.
    const refreshResult1 = admin.runCommand(refreshCommand);
    assert.commandFailed(refreshResult1, "refresh should have failed with failAllUpdates enabled");

    // The three sessions that were ended should have been removed from the collection.
    // However, the newly added eleventh session should not have been added.
    // Therefore, we expect there to be seven sessions in the collection.
    assert.eq(
        config.system.sessions.count() - initialSessions,
        7,
        "expect 7 sessions = 10 original sessions - 3 ended, without the 11th one added",
    );
    const statsAfterFirstBatch = getSessionCacheStats();
    jsTest.log.info("Session cache stats after first batch ended: " + tojson(statsAfterFirstBatch));
    assert.eq(
        statsAfterFirstBatch.lastSessionsCollectionJobEntriesEnded,
        3,
        "should have ended 3 sessions from the last batch",
    );
    assert.eq(
        statsAfterFirstBatch.lastSessionsCollectionJobEntriesRefreshed,
        0,
        "should have not been able to refresh any sessions",
    );
    assert.eq(
        statsAfterFirstBatch.lastSessionsCollectionJobEntriesFailedToRefresh,
        1,
        "should have failed to refresh the 11th session",
    );

    // End more sessions (sessions 4-6) while the failpoint is still active.
    jsTest.log.info("End three more sessions");
    const endSessionsBatch2 = sessions.slice(3, 6).map((s) => s.id);
    assert.commandWorked(admin.runCommand({endSessions: endSessionsBatch2}), "failed to end second batch of sessions");

    // Attempt refresh again - should still fail.
    jsTest.log.info("Attempt to refresh again - failpoint is still active");
    const refreshResult2 = admin.runCommand(refreshCommand);
    assert.commandFailed(refreshResult2, "refresh should still fail with failAllUpdates enabled");

    const statsAfterSecondBatch = getSessionCacheStats();
    jsTest.log.info("Session cache stats after second batch ended: " + tojson(statsAfterSecondBatch));
    assert.eq(
        statsAfterSecondBatch.lastSessionsCollectionJobEntriesEnded,
        3,
        "should have ended 3 sessions from the last batch",
    );
    assert.eq(
        statsAfterFirstBatch.lastSessionsCollectionJobEntriesRefreshed,
        0,
        "should have not been able to refresh any sessions",
    );
    assert.eq(
        statsAfterFirstBatch.lastSessionsCollectionJobEntriesFailedToRefresh,
        1,
        "should have failed to refresh the 11th session",
    );
    // Six sessions have now been ended, and we are still missing the 11th session due to the failpoint.
    assert.eq(
        config.system.sessions.count() - initialSessions,
        4,
        "expect 4 sessions = 10 original sessions - 3 ended in first batch - 3 ended in second batch, without the 11th one added",
    );

    // Disable the failpoint.
    disableFailpointsFunc();

    jsTest.log.info("End two more sessions after disabling failpoint");
    // End more sessions (sessions 7-8) without the failpoint enabled.
    const endSessionsBatch3 = sessions.slice(6, 8).map((s) => s.id);
    assert.commandWorked(admin.runCommand({endSessions: endSessionsBatch3}), "failed to end third batch of sessions");

    // Now refresh should succeed, and the eleventh session should be added to the collection.
    assert.commandWorked(admin.runCommand(refreshCommand), "refresh should succeed after disabling failpoint");
    assert.eq(
        config.system.sessions.count() - initialSessions,
        3,
        "expect 3 sessions = 10 original sessions - 3 ended in first batch - 3 ended in second batch - 2 ended in third batch + the 11th one",
    );

    // Get stats after successful refresh.
    const statsAfter = getSessionCacheStats();
    jsTest.log.info("Session cache stats after successful refresh: " + tojson(statsAfter));
    assert.eq(statsAfter.lastSessionsCollectionJobEntriesEnded, 2, "should have ended at least 2 accumulated sessions");
    assert.eq(
        statsAfter.lastSessionsCollectionJobEntriesRefreshed,
        1,
        "should have been able to refresh eleventh session",
    );
    assert.eq(
        statsAfter.lastSessionsCollectionJobEntriesFailedToRefresh,
        0,
        "should have no refresh failures after disabling failpoint",
    );

    // Cleanup: end remaining sessions.
    const remainingSessions = sessions.slice(8, 10).map((s) => s.id);
    assert.commandWorked(admin.runCommand({endSessions: remainingSessions}));
    const twelfthSession = admin.runCommand(startSessionCommand);
    assert.commandWorked(twelfthSession, "unable to start session");
    assert.commandWorked(admin.runCommand(refreshCommand));
    assert.eq(config.system.sessions.count() - initialSessions, 2, "original 10 sessions should be cleaned up");
    assert.commandWorked(admin.runCommand({endSessions: [eleventhSession.id]}));
    assert.commandWorked(admin.runCommand({endSessions: [twelfthSession.id]}));
    assert.commandWorked(admin.runCommand(refreshCommand));
    assert.eq(config.system.sessions.count(), initialSessions, "all sessions should be cleaned up");

    destroyFunc();
}

function shardedTest() {
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        rs: {nodes: 1},
        other: {
            mongosOptions: {
                setParameter: {
                    sessionMaxBatchSize: 500,
                },
            },
            rsOptions: {
                setParameter: {
                    sessionMaxBatchSize: 500,
                },
            },
        },
    });

    let fp, fp2;

    runTest(
        st.s,
        2,
        () => {
            const shard0Primary = st.rs0.getPrimary();
            fp = configureFailPoint(shard0Primary, "failCommand", {
                errorCode: ErrorCodes.InternalError,
                failCommands: ["update"],
                failInternalCommands: true,
                namespace: "config.system.sessions",
            });
            const shard1Primary = st.rs1.getPrimary();
            fp2 = configureFailPoint(shard1Primary, "failCommand", {
                errorCode: ErrorCodes.InternalError,
                failCommands: ["update"],
                failInternalCommands: true,
                namespace: "config.system.sessions",
            });
        },
        () => {
            fp.off();
            fp2.off();
        },
        () => {
            st.stop();
        },
    );
}

function replSetTest() {
    const rst = new ReplSetTest({nodes: 1});

    rst.startSet();
    rst.initiate();
    const db = rst.getPrimary().getDB("test");

    let fp;

    runTest(
        rst.getPrimary(),
        0,
        () => {
            fp = configureFailPoint(db, "failAllUpdates");
        },
        () => {
            fp.off();
        },
        () => {
            rst.stopSet();
        },
    );
}

function standaloneTest() {
    const conn = MongoRunner.runMongod();
    let fp;
    runTest(
        conn,
        0,
        () => {
            fp = configureFailPoint(conn, "failAllUpdates");
        },
        () => {
            fp.off();
        },
        () => {
            MongoRunner.stopMongod(conn);
        },
    );
}

/**
 * Test that in a sharded cluster, when refreshing the sessions collection fails on some shards
 * but succeeds on others, the failed sessions are retried on the next refresh while successfully
 * refreshed sessions are removed from activeSessions.
 */
function onlyOneShardFailedTest() {
    const sessionMaxBatchSize = 100;
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        rs: {nodes: 1},
        other: {
            mongosOptions: {
                setParameter: {
                    sessionMaxBatchSize: sessionMaxBatchSize,
                },
            },
            rsOptions: {
                setParameter: {
                    sessionMaxBatchSize: sessionMaxBatchSize,
                },
            },
        },
    });

    const mongos = st.s;
    const admin = mongos.getDB("admin");
    const config = mongos.getDB("config");

    function getSessionCacheStats() {
        return admin.serverStatus().logicalSessionRecordCache;
    }

    jsTest.log.info("Splitting sessions collection to distribute across shards");

    // Split at middle of keyspace.
    const middle = UUID("80000000-0000-0000-0000-000000000000");
    assert.commandWorked(
        admin.runCommand({
            split: "config.system.sessions",
            middle: {
                _id: {
                    id: middle,
                    uid: BinData(0, ""),
                },
            },
        }),
    );

    // Move one chunk to shard1.
    assert.commandWorked(
        admin.runCommand({
            moveChunk: "config.system.sessions",
            find: {_id: {id: MaxKey, uid: BinData(0, "")}},
            to: st.shard1.shardName,
        }),
    );

    // Make sure other chunk is on shard 0.
    assert.commandWorked(
        admin.runCommand({
            moveChunk: "config.system.sessions",
            find: {_id: {id: MinKey, uid: BinData(0, "")}},
            to: st.shard0.shardName,
        }),
    );

    jsTest.log.info("Creating sessions from mongos");
    let sessions = [];
    let numOnShard0 = 0;
    let numOnShard1 = 0;
    const initialMinPerShard = 5;
    while (numOnShard0 < initialMinPerShard || numOnShard1 < initialMinPerShard) {
        const res = admin.runCommand(startSessionCommand);
        assert.commandWorked(res, "unable to start session " + (numOnShard0 + numOnShard1));
        if (res.id.id < middle) {
            numOnShard0++;
        } else {
            numOnShard1++;
        }
        sessions.push(res);
    }
    jsTest.log.info("Should create " + numOnShard0 + " on shard 0 and " + numOnShard1 + " on shard 1");

    // Refresh to persist all sessions to the collection.
    assert.commandWorked(admin.runCommand(refreshCommand));
    const initialCount = config.system.sessions.count();
    jsTest.log.info("Initial session count: " + initialCount);
    assert.gte(initialCount, initialMinPerShard * 2, "should have at least " + initialMinPerShard * 2 + " sessions");
    const shard0Primary = st.rs0.getPrimary();
    const shard0DB = shard0Primary.getDB("config");
    const shard1DB = st.rs1.getPrimary().getDB("config");

    const shard0Sessions = shard0DB.system.sessions.count();
    const shard1Sessions = shard1DB.system.sessions.count();
    jsTest.log.info(`Sessions on shard0: ${shard0Sessions}, shard1: ${shard1Sessions}`);

    // End 3 sessions
    jsTest.log.info("Ending 3 sessions");
    const endSessionsBatch = sessions.slice(0, 3).map((s) => s.id);
    assert.commandWorked(admin.runCommand({endSessions: endSessionsBatch}));

    // Create more sessions that need to be refreshed
    let newSessions = [];
    numOnShard0 = 0;
    numOnShard1 = 0;
    const minTwoBatchesPerShard = 2 * sessionMaxBatchSize;
    while (numOnShard0 < minTwoBatchesPerShard || numOnShard1 < minTwoBatchesPerShard) {
        const res = admin.runCommand(startSessionCommand);
        assert.commandWorked(res, "unable to start session " + (numOnShard0 + numOnShard1));
        if (res.id.id < middle) {
            numOnShard0++;
        } else {
            numOnShard1++;
        }
        newSessions.push(res);
    }
    jsTest.log.info("Should create " + numOnShard0 + " on shard 0 and " + numOnShard1 + " on shard 1");
    const statsBeforeRefreshFailure = getSessionCacheStats();
    assert.gte(
        statsBeforeRefreshFailure.activeSessionsCount,
        numOnShard0 + numOnShard1,
        "should have created active sessions on both shards",
    );

    jsTest.log.info("Enabling failCommand on shard0 to fail updates");
    const fp = configureFailPoint(shard0Primary, "failCommand", {
        errorCode: ErrorCodes.InternalError,
        failCommands: ["update"],
        failInternalCommands: true,
        namespace: "config.system.sessions",
    });

    jsTest.log.info("Attempting refresh with partial failure");
    // With the new partial failure handling, refresh should succeed overall
    // but return the failed sessions for retry.
    const refreshResult = admin.runCommand(refreshCommand);
    assert.commandFailed(refreshResult, "refresh should have failed with failAllUpdates enabled");

    // Check the stats to see how many sessions failed to refresh.
    const statsAfterPartialFailure = getSessionCacheStats();
    jsTest.log.info("Stats after partial failure refresh: " + tojson(statsAfterPartialFailure));
    assert.gte(
        statsAfterPartialFailure.lastSessionsCollectionJobEntriesRefreshed,
        sessionMaxBatchSize,
        "should have been able to refresh at least one batch of sessions",
    );
    assert.gte(
        statsAfterPartialFailure.lastSessionsCollectionJobEntriesFailedToRefresh,
        minTwoBatchesPerShard,
        "Should have failed to refresh at least two batches of sessions",
    );
    assert.eq(
        statsAfterPartialFailure.lastSessionsCollectionJobEntriesEnded,
        3,
        "Should have ended the three sessions",
    );
    assert.lte(
        statsAfterPartialFailure.activeSessionsCount,
        numOnShard0 + numOnShard1 - sessionMaxBatchSize,
        "Should have fewer active sessions in cache after at least one successful batch",
    );

    // Disable the failpoint.
    fp.off();

    // Now refresh again - previously failed sessions should be retried.
    jsTest.log.info("Refresh again after disabling failpoint");
    assert.commandWorked(admin.runCommand(refreshCommand));

    const statsAfterRecovery = getSessionCacheStats();
    jsTest.log.info("Stats after recovery refresh: " + tojson(statsAfterRecovery));
    assert.gte(
        statsAfterRecovery.lastSessionsCollectionJobEntriesRefreshed,
        minTwoBatchesPerShard,
        "should have refreshed at least two batches of sessions",
    );
    assert.lte(
        statsAfterRecovery.lastSessionsCollectionJobEntriesRefreshed,
        numOnShard0 + numOnShard1,
        "should not have retried sessions that succeeded last time",
    );
    assert.eq(
        statsAfterRecovery.lastSessionsCollectionJobEntriesFailedToRefresh,
        0,
        "Should have been able to refresh all sessions with failpoint disabled",
    );

    // Verify the new sessions was eventually added
    const finalCount = config.system.sessions.count();
    jsTest.log("Final session count: " + finalCount);

    st.stop();
}

shardedTest();
standaloneTest();
replSetTest();

onlyOneShardFailedTest();
