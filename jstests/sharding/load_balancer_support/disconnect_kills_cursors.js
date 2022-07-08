/**
 * Tests that when a load-balanced client disconnects, its cursors are killed.
 */

(() => {
    "use strict";
    load("jstests/libs/parallelTester.js");

    function setupShardedCollection(st, dbName, collName) {
        const fullNss = dbName + "." + collName;
        const admin = st.s.getDB("admin");
        // Shard collection; ensure docs on each shard
        assert.commandWorked(admin.runCommand({enableSharding: dbName}));
        assert.commandWorked(admin.runCommand({movePrimary: dbName, to: st.shard0.shardName}));
        assert.commandWorked(admin.runCommand({shardCollection: fullNss, key: {_id: 1}}));
        assert.commandWorked(admin.runCommand({split: fullNss, middle: {_id: 0}}));
        assert.commandWorked(
            admin.runCommand({moveChunk: fullNss, find: {_id: 0}, to: st.shard1.shardName}));

        // Insert some docs on each shard
        let coll = admin.getSiblingDB(dbName).getCollection(collName);
        var bulk = coll.initializeUnorderedBulkOp();
        for (let i = -150; i < 150; i++) {
            bulk.insert({_id: i});
        }
        assert.commandWorked(bulk.execute());
    }

    function openCursor(mongosHost, dbName, collName, countdownLatch, identifyingComment) {
        const newDBConn = new Mongo(mongosHost).getDB(dbName);
        assert.commandWorked(newDBConn.getSiblingDB("admin").adminCommand(
            {configureFailPoint: "clientIsFromLoadBalancer", mode: "alwaysOn"}));
        let cmdRes =
            newDBConn.runCommand({find: collName, comment: identifyingComment, batchSize: 1});
        assert.commandWorked(cmdRes);
        const cursorId = cmdRes.cursor.id;
        assert.neq(cursorId, NumberLong(0));
        // Wait until countdownLatch value is 0.
        countdownLatch.await();
        return cursorId;
    }

    const testName = "load_balanced_disconnect_kills_cursors";
    let st = new ShardingTest({shards: 2, mongos: 1});
    const dbName = "foo";
    const collName = "bar";
    const admin = st.s.getDB("admin");
    const identifyingComment = "loadBalancedDisconnectComment";

    setupShardedCollection(st, dbName, collName);
    let countdownLatch = new CountDownLatch(1);

    let cursorOpeningThread =
        new Thread(openCursor, st.s.host, dbName, collName, countdownLatch, identifyingComment);
    cursorOpeningThread.start();

    let idleCursor = {};

    // Wait until we can see the cursor opened by cursorOpeningThread, identified by the comment, as
    // idle.
    assert.soon(() => {
        const curopCursor = admin.aggregate([
            {$currentOp: {allUsers: true, idleCursors: true, localOps: true}},
            {$match: {type: "idleCursor"}},
            {$match: {"cursor.originatingCommand.comment": identifyingComment}}
        ]);
        if (curopCursor.hasNext()) {
            idleCursor = curopCursor.next().cursor;
            return true;
        }
        return false;
    }, "Couldn't find cursor opened by cursorOpeningThread");

    // We've found the cursor opened by cursorOpeningThread.
    // We now join that thread, and therefore end its connection to the server.
    countdownLatch.countDown();
    cursorOpeningThread.join();

    let cursorId = cursorOpeningThread.returnData();
    assert.eq(idleCursor.cursorId, cursorId);

    // Make sure we can't find that cursor anymore/it has been killed.
    assert.soon(() => {
        const numCursorsFoundWithId =
            admin
                .aggregate([
                    {$currentOp: {allUsers: true, idleCursors: true, localOps: true}},
                    {$match: {type: "idleCursor"}},
                    {$match: {"cursor.cursorId": cursorId}}
                ])
                .itcount();
        return (numCursorsFoundWithId == 0);
    });

    assert.commandWorked(
        admin.adminCommand({configureFailPoint: "clientIsFromLoadBalancer", mode: "off"}));
    st.stop();
})();
