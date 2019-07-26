/**
 * Test the write conflict behavior between updates to a document's shard key and other
 * updates/deletes.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

(function() {

"use strict";

load('jstests/libs/parallelTester.js');  // for ScopedThread.
load('jstests/sharding/libs/sharded_transactions_helpers.js');

let st = new ShardingTest({mongos: 1, shards: 2});
let kDbName = 'db';
let mongos = st.s0;
let ns = kDbName + '.foo';
let db = mongos.getDB(kDbName);

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);

// Shards the collection "db.foo" on shard key {"x" : 1} such that negative "x" values are on
// shard0 and positive on shard1
assert.commandWorked(db.foo.createIndex({"x": 1}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {"x": 1}}));
assert.commandWorked(mongos.adminCommand({split: ns, middle: {"x": 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {"x": 0}, to: st.shard1.shardName}));

assert.commandWorked(db.foo.insert({"x": -50, "a": 10}));
assert.commandWorked(db.foo.insert({"x": -100, "a": 4}));
assert.commandWorked(db.foo.insert({"x": -150, "a": 15}));
assert.commandWorked(db.foo.insert({"x": 50, "a": 6}));
assert.commandWorked(db.foo.insert({"x": 100, "a": 8}));
assert.commandWorked(db.foo.insert({"x": 150, "a": 20}));

assert.commandWorked(st.shard0.adminCommand({_flushDatabaseCacheUpdates: kDbName}));
assert.commandWorked(st.shard0.adminCommand({_flushRoutingTableCacheUpdates: ns}));
assert.commandWorked(st.shard1.adminCommand({_flushDatabaseCacheUpdates: kDbName}));
assert.commandWorked(st.shard1.adminCommand({_flushRoutingTableCacheUpdates: ns}));

let session = mongos.startSession({retryWrites: false});
let sessionDB = session.getDatabase(kDbName);

let session2 = mongos.startSession({retryWrites: true});
let sessionDB2 = session2.getDatabase(kDbName);

// Returns true if the command "cmdName" has started running on the server.
function opStarted(cmdName) {
    return mongos.getDB(kDbName).currentOp().inprog.some(op => {
        return op.active && (op.ns === "db.foo") && (op.op === cmdName);
    });
}

// Send update that will change the shard key causing the document to move shards. Wait to hit
// failpoint specified.
function setFailPointAndSendUpdateToShardKeyInParallelShell(
    failpoint, failpointMode, shard, codeToRunInParallelShell) {
    assert.commandWorked(shard.adminCommand({configureFailPoint: failpoint, mode: failpointMode}));
    let awaitShell = startParallelShell(codeToRunInParallelShell, st.s.port);
    waitForFailpoint("Hit " + failpoint, 1);
    clearRawMongoProgramOutput();
    return awaitShell;
}

/**
 * Test that an in-transaction update to the shard key and a non-transactional update to the
 * same document will conflict and the non-transactional update will retry indefinitely. Once
 * the transaction will conflict and the non-transactional update will retry indefinitely. Once
 * the transaction commits, the non-transactional update should complete. When 'maxTimeMS' is
 * specified, the non-transactional write will timeout.
 */
(() => {
    const originalShardKeyValue = 50;
    const updatedShardKeyValue = -10;

    session.startTransaction();
    assert.commandWorked(
        sessionDB.foo.update({"x": originalShardKeyValue}, {$set: {"x": updatedShardKeyValue}}));
    // Attempt to update the same doc not in a transaction, this update should timeout.
    assert.commandFailedWithCode(db.runCommand({
        update: "foo",
        updates: [{q: {"x": originalShardKeyValue}, u: {$inc: {"a": 1}}}],
        maxTimeMS: 100
    }),
                                 ErrorCodes.MaxTimeMSExpired);
    // Run the non-transactional update again in a separate thread and wait for it to start.
    function conflictingUpdate(host, kDbName, query, update) {
        const mongosConn = new Mongo(host);
        return mongosConn.getDB(kDbName).foo.update(query, update);
    }
    let thread = new ScopedThread(
        conflictingUpdate, st.s.host, kDbName, {"x": originalShardKeyValue}, {$inc: {"a": 1}});
    thread.start();
    assert.soon(() => opStarted("update"));
    // Once we commit the transaction, the non-transaction update should finish, but it should
    // not actually modify any documents since the transaction commited first.
    assert.commandWorked(session.commitTransaction_forTesting());
    thread.join();
    assert.commandWorked(thread.returnData());
    assert.eq(1, db.foo.find({"x": updatedShardKeyValue, "a": 6}).itcount());
    assert.eq(0, db.foo.find({"x": originalShardKeyValue}).itcount());
    assert.eq(0, db.foo.find({"a": 7}).itcount());
})();

/**
 * When the non-transactional update or delete runs before the transactional update to the shard
 * key, the update to the shard key should fail with WriteConflict.
 */
(() => {
    const originalShardKeyValue = -10;
    let updatedShardKeyValue = 40;

    session.startTransaction();
    assert.commandWorked(sessionDB.runCommand({find: "foo"}));
    // Run a non-transactional update before updating the shard key.
    assert.commandWorked(db.foo.update({"x": originalShardKeyValue}, {$inc: {"a": 1}}));
    // Run transactional update to change the shard key for the same doc as updated above
    assert.commandFailedWithCode(
        sessionDB.foo.update({"x": originalShardKeyValue}, {$set: {"x": updatedShardKeyValue}}),
        ErrorCodes.WriteConflict);
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    assert.eq(1, db.foo.find({"x": originalShardKeyValue, "a": 7}).itcount());
    assert.eq(0, db.foo.find({"x": updatedShardKeyValue}).itcount());

    // Run a non-transactional delete before updating the shard key.
    updatedShardKeyValue = 20;
    session.startTransaction();
    assert.commandWorked(sessionDB.runCommand({find: "foo"}));
    assert.commandWorked(db.foo.remove({"x": originalShardKeyValue}));
    // Run transactional update to change the shard key for the same doc as updated above
    assert.commandFailedWithCode(
        sessionDB.foo.update({"x": originalShardKeyValue}, {$set: {"x": updatedShardKeyValue}}),
        ErrorCodes.WriteConflict);
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    assert.eq(0, db.foo.find({"x": originalShardKeyValue}).itcount());
    assert.eq(0, db.foo.find({"x": updatedShardKeyValue}).itcount());
})();

/**
 * Test scenarios where a concurrent update/delete that mutates the same document that a user is
 * updating the shard key for completes just before the update to the shard key throws
 * WouldChangeOwningShard.
 */

// Assert that if the concurrent update mutates the same document as the original update to the
// shard key, we get a write conflict.
(() => {
    let codeToRunInParallelShell =
        `{
                let session = db.getMongo().startSession();
                let sessionDB = session.getDatabase("db");
                session.startTransaction();
                let res = sessionDB.foo.update({"x": -50, "a" : 10}, {$set: {"x": 10}});
                assert.commandFailedWithCode(res, ErrorCodes.WriteConflict);
                assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                    ErrorCodes.NoSuchTransaction);
            }`;
    let awaitShell = setFailPointAndSendUpdateToShardKeyInParallelShell(
        "hangBeforeThrowWouldChangeOwningShard", "alwaysOn", st.shard0, codeToRunInParallelShell);
    // Send update that changes "a" so that the original update will no longer match this doc.
    // Turn off the failpoint so the server stops hanging.
    assert.commandWorked(sessionDB2.foo.update({"x": -50}, {$set: {"a": 300}}));
    assert.commandWorked(st.shard0.adminCommand({
        configureFailPoint: "hangBeforeThrowWouldChangeOwningShard",
        mode: "off",
    }));
    awaitShell();
    assert.eq(1, db.foo.find({"x": -50, "a": 300}).itcount());
    assert.eq(0, db.foo.find({"a": 10}).itcount());
    assert.eq(0, db.foo.find({"x": 10}).itcount());
})();

// Assert that if a concurrent delete removes the same document that the original update
// attempts to modify the shard key for, we get a write conflict.
(() => {
    let codeToRunInParallelShell =
        `{
                 let session = db.getMongo().startSession();
                 let sessionDB = session.getDatabase("db");
                 session.startTransaction();
                 let res = sessionDB.foo.update({"x": 100}, {$set: {"x": -1}});
                 assert.commandFailedWithCode(res, ErrorCodes.WriteConflict);
                 assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                    ErrorCodes.NoSuchTransaction);
             }`;
    let awaitShell = setFailPointAndSendUpdateToShardKeyInParallelShell(
        "hangBeforeThrowWouldChangeOwningShard", "alwaysOn", st.shard1, codeToRunInParallelShell);
    // Send update that changes "a" so that the original update will no longer match this doc.
    // Turn off the failpoint so the server stops hanging.
    assert.commandWorked(sessionDB2.foo.remove({"x": 100}));
    assert.commandWorked(st.shard1.adminCommand({
        configureFailPoint: "hangBeforeThrowWouldChangeOwningShard",
        mode: "off",
    }));
    awaitShell();
    assert.eq(0, db.foo.find({"x": 100}).itcount());
    assert.eq(0, db.foo.find({"x": -1}).itcount());
})();

// Assert that if the concurrent update also mutates the shard key (and remains on the same
// shard), the original update to the shard key will get a write conflict.
(() => {
    let codeToRunInParallelShell =
        `{
                let session = db.getMongo().startSession();
                let sessionDB = session.getDatabase("db");
                session.startTransaction();
                let res = sessionDB.foo.update({"x": -50}, {$set: {"x": 80}});
                assert.commandFailedWithCode(res, ErrorCodes.WriteConflict);
                assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                    ErrorCodes.NoSuchTransaction);
            }`;
    let awaitShell = setFailPointAndSendUpdateToShardKeyInParallelShell(
        "hangBeforeThrowWouldChangeOwningShard", "alwaysOn", st.shard0, codeToRunInParallelShell);
    // Send update that changes the shard key so that the original update will no longer match
    // this doc. This doc will still remain on its original shard. Turn off the failpoint so the
    // server stops hanging.
    assert.commandWorked(sessionDB2.foo.update({"x": -50}, {$set: {"x": -500}}));
    assert.commandWorked(st.shard0.adminCommand({
        configureFailPoint: "hangBeforeThrowWouldChangeOwningShard",
        mode: "off",
    }));
    awaitShell();
    assert.eq(0, db.foo.find({"x": -50}).itcount());
    assert.eq(1, db.foo.find({"x": -500}).itcount());
    assert.eq(0, db.foo.find({"x": 80}).itcount());
})();

/**
 * Test scenario where a concurrent update/delete that mutates the same document that a user is
 * updating the shard key for is sent just after the update to the shard key has deleted the
 * original document but before it has inserted the new one. The second update should not match
 * any documents.
 */

// Assert that if the concurrent update mutates the same document as the original update to the
// shard key, it does not match and documents.
(() => {
    let codeToRunInParallelShell =
        `{
                let session = db.getMongo().startSession();
                let sessionDB = session.getDatabase("db");
                session.startTransaction();
                let res = sessionDB.foo.update({"x": -100, "a" : 4}, {$set: {"x": 10}});
                assert.commandWorked(res);
                assert.eq(1, res.nMatched);
                assert.eq(1, res.nModified);
                assert.commandWorked(session.commitTransaction_forTesting());
            }`;
    let codeToRunInParallelShell2 =
        `{
                let session = db.getMongo().startSession();
                let sessionDB = session.getDatabase("db");
                let res = sessionDB.foo.update({"x": -100}, {$inc: {"a": 1}});
                assert.commandWorked(res);
                assert.eq(0, res.nMatched);
                assert.eq(0, res.nModified);
            }`;
    let awaitShell = setFailPointAndSendUpdateToShardKeyInParallelShell(
        "hangBeforeInsertOnUpdateShardKey", "alwaysOn", st.s, codeToRunInParallelShell);
    let awaitShell2 = startParallelShell(codeToRunInParallelShell2, st.s.port);
    assert.soon(() => opStarted("update"));
    assert.commandWorked(st.s.adminCommand({
        configureFailPoint: "hangBeforeInsertOnUpdateShardKey",
        mode: "off",
    }));
    awaitShell();
    awaitShell2();
    assert.eq(1, db.foo.find({"x": 10}).itcount());
    assert.eq(1, db.foo.find({"a": 4}).itcount());
    assert.eq(0, db.foo.find({"x": -100}).itcount());
    assert.eq(0, db.foo.find({"a": 5}).itcount());
})();

// Assert that if a concurrent delete removes the same document that the original update
// attempts to modify the shard key for, we get a write conflict.
(() => {
    let codeToRunInParallelShell =
        `{
                let session = db.getMongo().startSession();
                let sessionDB = session.getDatabase("db");
                session.startTransaction();
                let res = sessionDB.foo.update({"x": 10, "a" : 4}, {$set: {"x": -70}});
                assert.commandWorked(res);
                assert.eq(1, res.nMatched);
                assert.eq(1, res.nModified);
                assert.commandWorked(session.commitTransaction_forTesting());
            }`;
    let codeToRunInParallelShell2 =
        `{
                let session = db.getMongo().startSession();
                let sessionDB = session.getDatabase("db");
                let res = sessionDB.foo.remove({"x": 10});
                assert.commandWorked(res);
                assert.eq(0, res.nMatched);
                assert.eq(0, res.nModified);
            }`;
    let awaitShell = setFailPointAndSendUpdateToShardKeyInParallelShell(
        "hangBeforeInsertOnUpdateShardKey", "alwaysOn", st.s, codeToRunInParallelShell);
    let awaitShell2 = startParallelShell(codeToRunInParallelShell2, st.s.port);
    assert.soon(() => opStarted("remove"));
    assert.commandWorked(st.s.adminCommand({
        configureFailPoint: "hangBeforeInsertOnUpdateShardKey",
        mode: "off",
    }));
    awaitShell();
    awaitShell2();
    assert.eq(0, db.foo.find({"x": 10}).itcount());
    assert.eq(1, db.foo.find({"x": -70}).itcount());
})();

/**
 * Attempt to update the shard key in two different transactions. The second transaction should
 * fail with WriteConflict.
 */
(() => {
    session2 = mongos.startSession();
    sessionDB2 = session2.getDatabase(kDbName);
    // Start transactions on both sessions and then run the two change shard key updates for the
    // same document
    session.startTransaction();
    assert.commandWorked(sessionDB.runCommand({find: "foo"}));
    session2.startTransaction();
    // The first update will complete and the second should get a write conflict
    assert.commandWorked(sessionDB2.foo.update({"x": -500}, {$set: {"x": 25}}));
    assert.commandFailedWithCode(sessionDB.foo.update({"x": -500}, {$set: {"x": 250}}),
                                 ErrorCodes.WriteConflict);
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    assert.commandWorked(session2.commitTransaction_forTesting());
    assert.eq(1, db.foo.find({"x": 25}).itcount());
    assert.eq(0, db.foo.find({"x": 250}).itcount());
    assert.eq(0, db.foo.find({"x": -500}).itcount());
})();

/**
 * Test scenarios where a user sends an update as a retryable write that changes the shard key
 * and there is a concurrent update/delete that mutates the same document which completes after
 * the change to the shard key throws WouldChangeOwningShard the first time, but before mongos
 * starts a transaction to change the shard key.
 *
 * The scenario looks like:
 * 1. user sends db.foo.update({shardKey: x}, {shardKey: new x})
 * 2. shard throws WCOS for this update
 * 3. user sends db.foo.update({shardKey: x}, {otherFieldInDoc: y}) on a different thread, this
 * write completes successfully
 * 4. mongos starts a transaction and resends the update on line 1
 * 5. mongos deletes the old doc, inserts a doc with the updated shard key, and commits the txn
 */

// Assert that if the concurrent update modifies the document so that the update which changes
// the shard key no longer matches the doc, it does not modify the doc.
(() => {
    let codeToRunInParallelShell =
        `{
                let session = db.getMongo().startSession({retryWrites : true});
                let sessionDB = session.getDatabase("db");
                let res = sessionDB.foo.update({"x": -150, "a" : 15}, {$set: {"x": 1000}});
                assert.commandWorked(res);
                assert.eq(0, res.nMatched);
                assert.eq(0, res.nModified);
            }`;
    let awaitShell = setFailPointAndSendUpdateToShardKeyInParallelShell(
        "hangAfterThrowWouldChangeOwningShardRetryableWrite",
        "alwaysOn",
        st.s,
        codeToRunInParallelShell);
    // Send update that changes "a" so that the original update will no longer match this doc.
    // Turn off the failpoint so the server stops hanging.
    assert.commandWorked(sessionDB2.foo.update({"x": -150}, {$set: {"a": 3000}}));
    assert.commandWorked(st.s.adminCommand({
        configureFailPoint: "hangAfterThrowWouldChangeOwningShardRetryableWrite",
        mode: "off",
    }));
    awaitShell();
    assert.eq(1, db.foo.find({"x": -150, "a": 3000}).itcount());
    assert.eq(0, db.foo.find({"a": 15}).itcount());
    assert.eq(0, db.foo.find({"x": 1000}).itcount());
})();

// Assert that if the concurrent update modifies the document and the update which changes the
// shard key still matches the doc, the final document reflects both updates.
(() => {
    let codeToRunInParallelShell =
        `{
                let session = db.getMongo().startSession({retryWrites : true});
                let sessionDB = session.getDatabase("db");
                let res = sessionDB.foo.update({"x": 150}, {$set: {"x": -1000}});
                assert.commandWorked(res);
                assert.eq(1, res.nMatched);
                assert.eq(1, res.nModified);
            }`;
    let awaitShell = setFailPointAndSendUpdateToShardKeyInParallelShell(
        "hangAfterThrowWouldChangeOwningShardRetryableWrite",
        "alwaysOn",
        st.s,
        codeToRunInParallelShell);
    // Send update that changes "a". The original update will still match this doc because it
    // queries only on the shard key. Turn off the failpoint so the server stops hanging.
    assert.commandWorked(sessionDB2.foo.update({"x": 150}, {$set: {"a": -200}}));
    assert.commandWorked(st.s.adminCommand({
        configureFailPoint: "hangAfterThrowWouldChangeOwningShardRetryableWrite",
        mode: "off",
    }));
    awaitShell();
    assert.eq(1, db.foo.find({"x": -1000, "a": -200}).itcount());
    assert.eq(0, db.foo.find({"a": 20}).itcount());
    assert.eq(0, db.foo.find({"x": 150}).itcount());
})();

// Assert that if a concurrent delete removes the same document that the original update
// attempts to modify the shard key for, we don't match any docs.
(() => {
    let codeToRunInParallelShell =
        `{
                let session = db.getMongo().startSession({retryWrites : true});
                let sessionDB = session.getDatabase("db");
                let res = sessionDB.foo.update({"x": -150}, {$set: {"x": 1000}});
                assert.commandWorked(res);
                assert.eq(0, res.nMatched);
                assert.eq(0, res.nModified);
            }`;
    let awaitShell = setFailPointAndSendUpdateToShardKeyInParallelShell(
        "hangAfterThrowWouldChangeOwningShardRetryableWrite",
        "alwaysOn",
        st.s,
        codeToRunInParallelShell);
    // Remove this doc so that the original update will no longer match any doc.
    // Turn off the failpoint so the server stops hanging.
    assert.commandWorked(sessionDB2.foo.remove({"x": -150}));
    assert.commandWorked(st.s.adminCommand({
        configureFailPoint: "hangAfterThrowWouldChangeOwningShardRetryableWrite",
        mode: "off",
    }));
    awaitShell();
    assert.eq(0, db.foo.find({"x": -150}).itcount());
    assert.eq(0, db.foo.find({"a": 3000}).itcount());
    assert.eq(0, db.foo.find({"x": 1000}).itcount());
})();

st.stop();
}());
