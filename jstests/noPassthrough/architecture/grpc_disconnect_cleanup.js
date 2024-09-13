/**
 * Test that gRPC-based connections to mongos have any open cursors and transactions cleaned
 * up upon disconnection.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kThisFile = "jstests/noPassthrough/grpc_disconnect_cleanup.js";
const kTestName = "grpc_disconnect_cleanup";

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

// Opens a cursor and then waits on the countdown latch.
function openCursor(host, dbName, collName, comment, countdownLatch) {
    jsTestLog("Opening new connection in which to open a cursor.");
    const conn = new Mongo(`mongodb://${host}/?gRPC=true`);
    const testDB = conn.getDB(dbName);
    const result = testDB.runCommand({find: collName, comment: comment, batchSize: 1});
    assert.commandWorked(result);
    const cursorId = result.cursor.id;
    assert.neq(cursorId, NumberLong(0));

    jsTestLog("Waiting for main thread");
    countdownLatch.await();

    jsTestLog("Closing cursor thread connection");
    conn.close();
    return cursorId;
}

function runCursorTest(conn) {
    const admin = conn.getDB("admin");
    let countdownLatch = new CountDownLatch(1);

    // Start the cursor thread
    jsTestLog("Starting thread to open cursor");
    const dbName = kTestName;
    const collName = kTestName;
    const comment = kTestName;
    const cursorThread =
        new Thread(openCursor, conn.host, dbName, collName, comment, countdownLatch);
    cursorThread.start();

    // Wait until we see the cursor is idle
    jsTestLog("Waiting for the cursor to become idle");
    let idleCursor = {};
    assert.soon(() => {
        const curopCursor = admin.aggregate([
            {$currentOp: {allUsers: true, idleCursors: true, localOps: true}},
            {$match: {type: "idleCursor"}},
            {$match: {"cursor.originatingCommand.comment": comment}}
        ]);
        if (curopCursor.hasNext()) {
            idleCursor = curopCursor.next().cursor;
            return true;
        }
        return false;
    }, "Couldn't find cursor opened by cursorThread");

    // Join the cursor thread
    jsTestLog("Detected idle cursor, joining cursor thread");
    countdownLatch.countDown();
    cursorThread.join();

    // Assert that the idle cursor we found is the same one from the thread
    let cursorId = cursorThread.returnData();
    assert.eq(idleCursor.cursorId, cursorId);

    // Assert that the cursor is cleaned up
    jsTestLog("Waiting for the cursor to get cleaned up");
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
    }, "The cursor was not cleaned up", 10000, 1000);
}

// Starts a transaction and then waits on the countdown latch
function startTransaction(host, dbName, collName, appName, countdownLatch) {
    jsTestLog("Opening new connection in which to start a transaction.");
    const conn = new Mongo(`mongodb://${host}/?appName=${appName}&gRPC=true`);

    // We manually generate a logical session and send it to the server explicitly, to prevent
    // the shell from making its own logical session object which will attempt to explicitly
    // abort the transaction on disconnection.
    const session = {id: UUID()};
    const txnNumber = NumberLong(0);

    const result = conn.getDB(dbName).runCommand({
        find: collName,
        batchSize: 1,
        lsid: session,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false
    });
    assert.commandWorked(result);

    jsTestLog("Waiting for main thread");
    countdownLatch.await();

    jsTestLog("Closing transaction thread connection");
    conn.close();

    return [session, txnNumber];
}

function runTransactionTest(conn) {
    const admin = conn.getDB("admin");
    let countdownLatch = new CountDownLatch(1);

    // capture txn statistics before opening and aborting the txn.
    const preStatus = admin.adminCommand({'serverStatus': 1}).transactions;

    const dbName = kTestName;
    const collName = kTestName;
    const appName = kTestName;

    const transactionThread =
        new Thread(startTransaction, conn.host, dbName, collName, appName, countdownLatch);
    transactionThread.start();

    let idleSession = {};

    // Wait until we can see the transaction, identified by the appName, as idle.
    jsTestLog("Waiting for the transaction's session to become idle");
    assert.soon(() => {
        const curopCursor = admin.aggregate([
            {$currentOp: {allUsers: true, idleCursors: true, localOps: true, idleSessions: true}},
            {$match: {type: "idleSession"}},
            {$match: {appName: appName}}
        ]);
        if (curopCursor.hasNext()) {
            idleSession = curopCursor.next();
            return true;
        }
        return false;
    }, "Couldn't find transaction opened by transactionThread.");

    // Join the transaction thread.
    jsTestLog("Detected idle transaction, joining transaction thread");
    countdownLatch.countDown();
    transactionThread.join();

    // Assert that the idle session we found was indeed the one from the thread.
    const [sessionLsid, txnNumber] = transactionThread.returnData();
    assert.eq(idleSession.lsid.id, sessionLsid.id);
    assert.eq(idleSession.transaction.parameters.txnNumber, txnNumber);

    // Assert that the transaction is cleaned up.
    jsTestLog("Waiting for the transaction to be cleaned up.");
    const numPrevInterrupted =
        preStatus.abortCause.hasOwnProperty('Interrupted') ? preStatus.abortCause.Interrupted : 0;
    assert.soon(() => {
        const postStatus = admin.adminCommand({'serverStatus': 1}).transactions;
        return (postStatus.totalAborted == preStatus.totalAborted + 1) &&
            (postStatus.abortCause.Interrupted, numPrevInterrupted + 1);
    });
}

function runTest(conn) {
    runCursorTest(conn);
    runTransactionTest(conn);
}

if (typeof inner == 'undefined') {
    jsTestLog("Outer shell: setting up test environment");
    let st = new ShardingTest({shards: 2});

    if (!FeatureFlagUtil.isPresentAndEnabled(st.s.getDB("admin"), "GRPC")) {
        jsTestLog("Skipping grpc_disconnect_cleanup.js test due to featureFlagGRPC being disabled");
        st.stop();
        quit();
    }

    setupShardedCollection(st, kTestName, kTestName);

    const mongosHost = st.s.host.split(":")[0];
    const grpcUri = `mongodb://localhost:${st.s.fullOptions.grpcPort}/?gRPC=true`;
    jsTestLog("Outer shell: launching inner shell to connect over gRPC to " + grpcUri);

    const exitCode = runMongoProgram('mongo', grpcUri, '--eval', `const inner=true;`, kThisFile);
    assert.eq(exitCode, 0);
    jsTestLog("Outer shell: inner shell exited cleanly.");

    st.stop();
} else {
    jsTestLog("Inner shell: running test under gRPC connection.");
    runTest(db.getMongo());
}
