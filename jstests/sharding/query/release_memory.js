/**
 * Make sure that running releaseMemory command does not affect the normal query execution.
 *
 * Uses getMore to pin an open cursor.
 * @tags: [
 *   requires_getmore,
 *   requires_fcv_81,
 *   uses_parallel_shell,
 *   # TODO (SERVER-102377): Re-enable this test.
 *   DISABLED_TEMPORARILY_DUE_TO_FCV_UPGRADE,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test manually simulates a session, which is not compatible with implicit sessions.
TestData.disableImplicitSessions = true;

const kFailPointName = "waitAfterPinningCursorBeforeGetMoreBatch";
const kFailpointOptions = {
    shouldCheckForInterrupt: true
};

const st = new ShardingTest({shards: 2});
const kDBName = "test";
const mongosDB = st.s.getDB(kDBName);
const shard0DB = st.shard0.getDB(kDBName);
const shard1DB = st.shard1.getDB(kDBName);

st.s.adminCommand({enablesharding: kDBName, primaryShard: st.shard0.name});

let coll = mongosDB.jstest_release_memory;

let docs = [];
for (let i = 0; i < 10; i++) {
    docs.push({_id: i});
}
assert.commandWorked(coll.insertMany(docs));

st.shardColl(coll, {_id: 1}, {_id: 5}, {_id: 6}, kDBName, false);

function assertGetMore(targetDB, collName, cursorId, useSession, sessionId, results, docs) {
    const testDB = targetDB ? targetDB : db;
    let getMoreCmd = {getMore: cursorId, collection: collName, batchSize: 4};

    if (useSession) {
        getMoreCmd.lsid = sessionId;
    }

    while (cursorId != 0) {
        jsTest.log(`Running getMore command ${tojson(getMoreCmd)}`);
        const getMoreRes = testDB.runCommand(getMoreCmd);
        assert.commandWorked(getMoreRes);
        const cursor = getMoreRes.cursor;
        cursorId = cursor.id;
        results.push(...cursor.nextBatch);
    }
    if (useSession) {
        assert.commandWorked(testDB.adminCommand({endSessions: [sessionId]}));
    }
    assert.sameMembers(results, docs);
}

// Tests that the various cursors involved in a sharded query can release memory.
function testReleaseMemory({func, useSession, cursorsNum, pinCursor, unknownCursor}) {
    let cursorIdsArr = [];
    let cursorIdx = 0;
    let sessionIdsArr = [];
    let getMoreJoiner = null;
    let results = [];

    for (let i = 0; i < cursorsNum; ++i) {
        // Run a find against mongos. This should open cursors on both of the shards.
        let findCmd = {find: coll.getName(), batchSize: 2};

        if (useSession) {
            // Manually start a session so it can be continued from inside a parallel shell.
            const sessionId = assert.commandWorked(mongosDB.adminCommand({startSession: 1})).id;
            findCmd.lsid = sessionId;
            sessionIdsArr.push(sessionId);
        }

        jsTest.log(`Running find command ${i}`);
        const findRes = mongosDB.runCommand(findCmd);
        assert.commandWorked(findRes);
        let cursor = findRes.cursor;
        assert.neq(cursor.id, NumberLong(0));
        results.push(cursor.firstBatch);
        cursorIdsArr.push(cursor.id);
    }

    let shard0DBFailpoint;
    let shard1DBFailpoint;
    if (pinCursor) {
        assert.gte(cursorsNum, cursorIdx + 1);
        shard0DBFailpoint = configureFailPoint(shard0DB, kFailPointName, kFailpointOptions);
        shard1DBFailpoint = configureFailPoint(shard1DB, kFailPointName, kFailpointOptions);

        getMoreJoiner = startParallelShell(funWithArgs(assertGetMore,
                                                       null,
                                                       coll.getName(),
                                                       cursorIdsArr[cursorIdx],
                                                       useSession,
                                                       sessionIdsArr[cursorIdx],
                                                       results[cursorIdx],
                                                       docs),
                                           st.s.port);

        // Wait until we know the mongod cursors are pinned.
        shard0DBFailpoint.wait();
        shard1DBFailpoint.wait();
        ++cursorIdx;
    }

    if (unknownCursor) {
        assert.gte(cursorsNum, cursorIdx + 1);
        // Kill the cursor to make it go away.
        jsTest.log(`killing cursor ${cursorIdsArr[cursorIdx]}`);
        let cmdRes = assert.commandWorked(
            mongosDB.runCommand({killCursors: coll.getName(), cursors: [cursorIdsArr[cursorIdx]]}));
        assert.eq(cmdRes.cursorsKilled, [cursorIdsArr[cursorIdx]]);
        assert.eq(cmdRes.cursorsAlive, []);
        assert.eq(cmdRes.cursorsNotFound, []);
        assert.eq(cmdRes.cursorsUnknown, []);
        ++cursorIdx;
    }

    // Use the function provided by the caller to call the releaseMemory command.
    func(cursorIdsArr);

    if (pinCursor) {
        shard0DBFailpoint.off();
        shard1DBFailpoint.off();

        // The getMore should finish now
        getMoreJoiner();
        getMoreJoiner = null;
    }

    for (; cursorIdx < cursorIdsArr.length; ++cursorIdx) {
        assertGetMore(mongosDB,
                      coll.getName(),
                      cursorIdsArr[cursorIdx],
                      useSession,
                      sessionIdsArr[cursorIdx],
                      results[cursorIdx],
                      docs);
    }
}

for (let useSession of [false, true]) {
    // Test single cursor.
    testReleaseMemory({
        func: function(mongosCursorIdArr) {
            jsTest.log(`Running releaseMemory command for single cursor ${
                tojson({releaseMemory: mongosCursorIdArr})}`);
            let cmdRes =
                assert.commandWorked(mongosDB.runCommand({releaseMemory: mongosCursorIdArr}));
            assert.eq(cmdRes.cursorsReleased, mongosCursorIdArr);
            assert.eq(cmdRes.cursorsCurrentlyPinned, []);
            assert.eq(cmdRes.cursorsNotFound, []);
        },
        useSession: useSession,
        cursorsNum: 1,
        pinCursor: false,
        unknownCursor: false
    });

    // Test multiple cursors.
    testReleaseMemory({
        func: function(mongosCursorIdsArr) {
            jsTest.log(`Running releaseMemory command for multiple cursors ${
                tojson({releaseMemory: mongosCursorIdsArr})}`);
            let cmdRes =
                assert.commandWorked(mongosDB.runCommand({releaseMemory: mongosCursorIdsArr}));
            assert.eq(cmdRes.cursorsReleased, mongosCursorIdsArr);
            assert.eq(cmdRes.cursorsCurrentlyPinned, []);
            assert.eq(cmdRes.cursorsNotFound, []);
        },
        useSession: useSession,
        cursorsNum: 2,
        pinCursor: false,
        unknownCursor: false
    });

    // Test not found cursor
    testReleaseMemory({
        func: function(mongosCursorIdArr) {
            jsTest.log(`Running releaseMemory command for single unknown cursor ${
                tojson({releaseMemory: mongosCursorIdArr})}`);
            let cmdRes =
                assert.commandWorked(mongosDB.runCommand({releaseMemory: mongosCursorIdArr}));
            assert.eq(cmdRes.cursorsReleased, []);
            assert.eq(cmdRes.cursorsCurrentlyPinned, []);
            assert.eq(cmdRes.cursorsNotFound, mongosCursorIdArr);
        },
        useSession: useSession,
        cursorsNum: 1,
        pinCursor: false,
        unknownCursor: true
    });

    // Test pinned cursor
    testReleaseMemory({
        func: function(mongosCursorIdArr) {
            jsTest.log(`Running releaseMemory command for single cursor pinned ${
                tojson({releaseMemory: mongosCursorIdArr})}`);
            let cmdRes =
                assert.commandWorked(mongosDB.runCommand({releaseMemory: mongosCursorIdArr}));
            assert.eq(cmdRes.cursorsReleased, []);
            assert.eq(cmdRes.cursorsCurrentlyPinned, mongosCursorIdArr);
            assert.eq(cmdRes.cursorsNotFound, []);
        },
        useSession: useSession,
        cursorsNum: 1,
        pinCursor: true,
        unknownCursor: false
    });

    // Test cursor combinations
    testReleaseMemory({
        func: function(mongosCursorIdArr) {
            jsTest.log(`Running releaseMemory command for all types of cursors ${
                tojson({releaseMemory: mongosCursorIdArr})}`);
            let cmdRes =
                assert.commandWorked(mongosDB.runCommand({releaseMemory: mongosCursorIdArr}));
            assert.eq(cmdRes.cursorsReleased, [mongosCursorIdArr[2]]);
            assert.eq(cmdRes.cursorsCurrentlyPinned, [mongosCursorIdArr[0]]);
            assert.eq(cmdRes.cursorsNotFound, [mongosCursorIdArr[1]]);
        },
        useSession: useSession,
        cursorsNum: 3,
        pinCursor: true,
        unknownCursor: true
    });
}

st.stop();
