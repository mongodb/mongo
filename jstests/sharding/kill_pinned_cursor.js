/**
 * Run a query on a sharded cluster where one of the shards hangs. Running killCursors on the mongos
 * should always succeed.
 *
 * Uses getMore to pin an open cursor.
 * @tags: [requires_getmore]
 */

(function() {
    "use strict";

    const kFailPointName = "waitAfterPinningCursorBeforeGetMoreBatch";
    const kFailpointOptions = {shouldCheckForInterrupt: true};

    const st = new ShardingTest({shards: 2});
    const kDBName = "test";
    const mongosDB = st.s.getDB(kDBName);
    const shard0DB = st.shard0.getDB(kDBName);
    const shard1DB = st.shard1.getDB(kDBName);

    let coll = mongosDB.jstest_kill_pinned_cursor;
    coll.drop();

    for (let i = 0; i < 10; i++) {
        assert.writeOK(coll.insert({_id: i}));
    }

    st.shardColl(coll, {_id: 1}, {_id: 5}, {_id: 6}, kDBName, false);
    st.ensurePrimaryShard(kDBName, st.shard0.name);

    // Tests that the various cursors involved in a sharded query can be killed, even when pinned.
    //
    // Sets up a sharded cursor, opens a mongos cursor, and uses failpoints to cause the mongos
    // cursor to hang due to getMore commands hanging on each of the shards. Then invokes
    // 'killFunc', and verifies the cursors on the shards and the mongos cursor get cleaned up.
    //
    // 'getMoreErrCode' is the error code with which we expect the getMore to fail (e.g. a
    // killCursors command should cause getMore to fail with "CursorKilled", but killOp should cause
    // a getMore to fail with "Interrupted").
    function testShardedKillPinned({killFunc: killFunc, getMoreErrCode: getMoreErrCode}) {
        let getMoreJoiner = null;
        let cursorId;

        try {
            // Set up the mongods to hang on a getMore request. ONLY set the failpoint on the
            // mongods. Setting the failpoint on the mongos will only cause it to spin, and not
            // actually send any requests out.
            assert.commandWorked(shard0DB.adminCommand(
                {configureFailPoint: kFailPointName, mode: "alwaysOn", data: kFailpointOptions}));
            assert.commandWorked(shard1DB.adminCommand(
                {configureFailPoint: kFailPointName, mode: "alwaysOn", data: kFailpointOptions}));

            // Run a find against mongos. This should open cursors on both of the shards.
            let cmdRes = mongosDB.runCommand({find: coll.getName(), batchSize: 2});
            assert.commandWorked(cmdRes);
            cursorId = cmdRes.cursor.id;
            assert.neq(cursorId, NumberLong(0));

            // Now run a getMore in a parallel shell. This will require issuing getMores to the
            // shards, which hang due to the fail point. In turn, mongos hangs waiting for the
            // shards.
            const runGetMore = function() {
                let response =
                    db.runCommand({getMore: cursorId, collection: collName, batchSize: 4});
                // We expect that the operation will get interrupted and fail.
                assert.commandFailedWithCode(response, getMoreErrCode);
            };
            let code = `let cursorId = ${cursorId.toString()};`;
            code += `let collName = "${coll.getName()}";`;
            code += `let getMoreErrCode = ${getMoreErrCode};`;
            code += `(${runGetMore.toString()})();`;
            getMoreJoiner = startParallelShell(code, st.s.port);

            // Sleep until we know the mongod cursors are pinned.
            assert.soon(() => shard0DB.serverStatus().metrics.cursor.open.pinned > 0);
            assert.soon(() => shard1DB.serverStatus().metrics.cursor.open.pinned > 0);

            // Use the function provided by the caller to kill the sharded query.
            killFunc(cursorId);

            // The getMore should finish now that we've killed the cursor (even though the failpoint
            // is still enabled).
            getMoreJoiner();
            getMoreJoiner = null;

            // By now, the getMore run against the mongos has returned with an indication that the
            // cursor has been killed.  Verify that the cursor is really gone by running a
            // killCursors command, and checking that the cursor is reported as "not found".
            let killRes = mongosDB.runCommand({killCursors: coll.getName(), cursors: [cursorId]});
            assert.commandWorked(killRes);
            assert.eq(killRes.cursorsAlive, []);
            assert.eq(killRes.cursorsNotFound, [cursorId]);
            assert.eq(killRes.cursorsUnknown, []);

            // Eventually the cursors on the mongods should also be cleaned up. They should be
            // killed by mongos when the mongos cursor gets killed.
            assert.soon(() => shard0DB.serverStatus().metrics.cursor.open.pinned == 0);
            assert.soon(() => shard1DB.serverStatus().metrics.cursor.open.pinned == 0);
            assert.eq(shard0DB.serverStatus().metrics.cursor.open.total, 0);
            assert.eq(shard1DB.serverStatus().metrics.cursor.open.total, 0);
        } finally {
            assert.commandWorked(
                shard0DB.adminCommand({configureFailPoint: kFailPointName, mode: "off"}));
            assert.commandWorked(
                shard1DB.adminCommand({configureFailPoint: kFailPointName, mode: "off"}));
            if (getMoreJoiner) {
                getMoreJoiner();
            }
        }
    }

    // Test that running 'killCursors' against a pinned mongos cursor (with pinned mongod cursors)
    // correctly cleans up all of the involved cursors.
    testShardedKillPinned({
        killFunc: function(mongosCursorId) {
            // Run killCursors against the mongos cursor. Verify that the cursor is reported as
            // killed successfully, and does not hang or return a "CursorInUse" error.
            let cmdRes =
                mongosDB.runCommand({killCursors: coll.getName(), cursors: [mongosCursorId]});
            assert.commandWorked(cmdRes);
            assert.eq(cmdRes.cursorsKilled, [mongosCursorId]);
            assert.eq(cmdRes.cursorsAlive, []);
            assert.eq(cmdRes.cursorsNotFound, []);
            assert.eq(cmdRes.cursorsUnknown, []);
        },
        getMoreErrCode: ErrorCodes.CursorKilled
    });

    // Test that running killOp against one of the cursors pinned on mongod causes all involved
    // cursors to be killed.
    testShardedKillPinned({
        // This function ignores the mongos cursor id, since it instead uses currentOp to obtain an
        // op id to kill.
        killFunc: function() {
            let currentGetMoresArray =
                shard0DB.getSiblingDB("admin")
                    .aggregate([{$currentOp: {}}, {$match: {"command.getMore": {$exists: true}}}])
                    .toArray();
            assert.eq(1, currentGetMoresArray.length);
            let currentGetMore = currentGetMoresArray[0];
            let killOpResult = shard0DB.killOp(currentGetMore.opid);
            assert.commandWorked(killOpResult);
        },
        getMoreErrCode: ErrorCodes.Interrupted
    });

    // Test that running killCursors against one of the cursors pinned on mongod causes all involved
    // cursors to be killed.
    testShardedKillPinned({
        // This function ignores the mongos cursor id, since it instead uses currentOp to obtain the
        // cursor id of one of the shard cursors.
        killFunc: function() {
            let currentGetMoresArray =
                shard0DB.getSiblingDB("admin")
                    .aggregate([{$currentOp: {}}, {$match: {"command.getMore": {$exists: true}}}])
                    .toArray();
            assert.eq(1, currentGetMoresArray.length);
            let currentGetMore = currentGetMoresArray[0];
            let shardCursorId = currentGetMore.command.getMore;
            let cmdRes =
                shard0DB.runCommand({killCursors: coll.getName(), cursors: [shardCursorId]});
            assert.commandWorked(cmdRes);
            assert.eq(cmdRes.cursorsKilled, [shardCursorId]);
            assert.eq(cmdRes.cursorsAlive, []);
            assert.eq(cmdRes.cursorsNotFound, []);
            assert.eq(cmdRes.cursorsUnknown, []);
        },
        getMoreErrCode: ErrorCodes.CursorKilled
    });

    st.stop();
})();
