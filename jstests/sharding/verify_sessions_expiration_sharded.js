// Tests valid coordination of the expiration and vivification of documents between the
// config.system.sessions collection and the logical session cache.
//
// 1. Sessions should be removed from the logical session cache when they expire from
//    the config.system.sessions collection.
// 2. getMores run on open cursors should update the lastUse field on documents in the
//    config.system.sessions collection, prolonging the time for expiration on said document
//    and corresponding session.
// 3. Open cursors that are not currently in use should be killed when their corresponding sessions
//    expire from the config.system.sessions collection.
// 4. Currently running operations corresponding to a session should prevent said session from
//    expiring from the config.system.sessions collection. If the expiration date has been reached
//    during a currently running operation, the logical session cache should vivify the session and
//    replace it in the config.system.sessions collection.
//
// @tags: [
//    # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
//    embedded_router_incompatible,
// ]

// This test makes assertions about the number of logical session records.
TestData.disableImplicitSessions = true;

import {withPinnedCursor} from "jstests/libs/pin_getmore_cursor.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/*
 * Refresh logical session cache on mongos and shard and check that each one of the session IDs in
 * the 'expectedSessionIDs' array exist. If 'expectToExist' is false, checks that they don't exist.
 */
const refreshSessionsAndVerifyExistence =
    (mongosConfig, shardConfig, expectedSessionIDs, expectToExist = true) => {
        const refresh = {refreshLogicalSessionCacheNow: 1};

        mongosConfig.runCommand(refresh);
        shardConfig.runCommand(refresh);

        const sessionIDs = mongosConfig.system.sessions.find().toArray().map(s => s._id.id);

        // Assert that 'expectedSessionIDs' is a subset of 'sessionIDs'
        assert(expectedSessionIDs.every(expectedId => {
            return sessionIDs.some(s => {
                return bsonBinaryEqual(s, expectedId);
            }) == expectToExist;
        }));
    };

const verifyOpenCursorCount = (db, expectedCount) => {
    assert.eq(db.serverStatus().metrics.cursor.open.total, expectedCount);
};

const getSessions = config => {
    return config.system.sessions.aggregate([{'$listSessions': {allUsers: true}}]).toArray();
};

const createSessions = (mongos, count) => Array(count).fill(null).map(() => {
    const session = mongos.startSession({});
    assert.commandWorked(session.getDatabase("admin").runCommand({usersInfo: 1}),
                         "initialize the session");
    return session;
});

const setup = (shardingTest, dbName, collName) => {
    const mongos = shardingTest.s;
    const db = mongos.getDB(dbName);
    const mongosConfig = mongos.getDB("config");
    const shardConfig = shardingTest.rs0.getPrimary().getDB("config");
    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));

    for (let i = 0; i < 10; i++) {
        db[collName].insert({_id: i, a: i, b: 1});
    }

    return {
        mongos,
        db,
        mongosConfig,
        shardConfig,
    };
};

// 1. Verify that sessions expire from config.system.sessions after the timeout has passed.
const testSessionExpiry = (shardingTest) => {
    const test = setup(shardingTest, "testSessionExpiry", "empty");
    const sessionIDs = [createSessions(test.mongos, 1)[0].getSessionId().id];
    refreshSessionsAndVerifyExistence(test.mongosConfig, test.shardConfig, sessionIDs);

    // Manually delete entries in config.system.sessions to simulate TTL expiration.
    assert.commandWorked(test.mongosConfig.system.sessions.remove({}));
    refreshSessionsAndVerifyExistence(
        test.mongosConfig, test.shardConfig, sessionIDs, false /* expectToExist */);
};

// 2. Verify that getMores after finds will update the 'lastUse' field on documents in the
// config.system.sessions collection.
const testSessionUpdates = (shardingTest) => {
    const dbName = "testSessionUpdates";
    const testCollName = "testColl";
    const test = setup(shardingTest, dbName, testCollName);

    // Make sure we have no opened sessions before starting the test. Creating a collection will
    // generate a new session during the commit phase of the create coordinator
    refreshSessionsAndVerifyExistence(
        test.mongosConfig, test.shardConfig, [], false /* expectToExist */);
    const openedSessionIDs = test.mongosConfig.system.sessions.find().toArray().map(s => s._id);
    assert.commandWorked(test.db.runCommand({endSessions: openedSessionIDs}));

    const sessions = createSessions(test.mongos, 5);
    const cursors = sessions.map(session => {
        const cursor = session.getDatabase(dbName)[testCollName].find({b: 1}).batchSize(1);
        assert(cursor.hasNext());
        return cursor;
    });
    const sessionIDs = sessions.map(session => session.getSessionId().id);

    refreshSessionsAndVerifyExistence(test.mongosConfig, test.shardConfig, sessionIDs);
    verifyOpenCursorCount(test.mongosConfig, 5);

    let lastUseValues = Array(sessionIDs.length).fill(new Date(0));
    for (let i = 0; i < 3; i++) {
        cursors.forEach(cursor => cursor.next());

        refreshSessionsAndVerifyExistence(test.mongosConfig, test.shardConfig, sessionIDs);
        verifyOpenCursorCount(test.mongosConfig, 5);

        // Get the sessions that are opened for the cursors
        const sessionsCollectionArray = getSessions(test.mongosConfig).filter(session => {
            return sessionIDs.some(s => {
                return bsonBinaryEqual(s, session._id.id);
            });
        });
        assert.eq(sessionsCollectionArray.length, cursors.length);

        sessionsCollectionArray.forEach((session, idx) =>
                                            assert.gt(session.lastUse, lastUseValues[idx]));
        lastUseValues = sessionsCollectionArray.map(session => session.lastUse);

        // Date_t has the granularity of milliseconds, so we have to make sure we don't run this
        // loop faster than that.
        sleep(10);
    }
};

// 3. Verify that letting sessions expire (simulated by manual deletion) will kill their
// cursors.
const testCursorInvalidation = (shardingTest) => {
    const dbName = "testCursorInvalidation";
    const testCollName = "testColl";
    const test = setup(shardingTest, dbName, testCollName);

    const session = createSessions(test.mongos, 1)[0];
    const cursor = session.getDatabase(dbName)[testCollName].find({b: 1}).batchSize(1);
    assert(cursor.hasNext());
    const sessionIDs = [session.getSessionId().id];
    refreshSessionsAndVerifyExistence(test.mongosConfig, test.shardConfig, sessionIDs);

    assert.commandWorked(test.mongosConfig.system.sessions.remove({}));

    refreshSessionsAndVerifyExistence(
        test.mongosConfig, test.shardConfig, sessionIDs, false /* expectToExist */);
    verifyOpenCursorCount(test.mongosConfig, 0);

    assert.commandFailedWithCode(
        test.db.runCommand({getMore: cursor._cursor._cursorid, collection: testCollName}),
        ErrorCodes.CursorNotFound,
        'expected getMore to fail because the cursor was killed');
};

// 4. Verify that an expired session (simulated by manual deletion) that has a currently
// running operation will be vivified during the logical session cache refresh.
const testVivification = (shardingTest) => {
    const dbName = "test";
    const test = setup(shardingTest, dbName, "empty");

    const pinnedCursorSession = test.mongos.startSession();
    const pinnedCursorSessionID = pinnedCursorSession.getSessionId().id;
    const pinnedCursorDB = pinnedCursorSession.getDatabase(dbName);

    withPinnedCursor({
        conn: test.mongos,
        sessionId: pinnedCursorSession,
        db: pinnedCursorDB,
        assertFunction: (cursorId, coll) => {
            assert.commandWorked(test.mongosConfig.system.sessions.remove({}));
            verifyOpenCursorCount(test.mongosConfig, 1);

            refreshSessionsAndVerifyExistence(
                test.mongosConfig, test.shardConfig, [pinnedCursorSessionID]);

            let db = coll.getDB();
            assert.commandWorked(db.runCommand({killCursors: coll.getName(), cursors: [cursorId]}));
        },
        runGetMoreFunc: (collName, cursorId, sessionId) => {
            db.runCommand({getMore: cursorId, collection: collName, lsid: sessionId});
        },
        failPointName: "waitAfterPinningCursorBeforeGetMoreBatch",
        assertEndCounts: false,
    });
};

const shardingTest = new ShardingTest({
    shards: 1,
});

testSessionExpiry(shardingTest);
testSessionUpdates(shardingTest);
testCursorInvalidation(shardingTest);
testVivification(shardingTest);

shardingTest.stop();
