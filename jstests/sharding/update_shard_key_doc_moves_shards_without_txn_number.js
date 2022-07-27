/**
 * Test that a client not running a retryable write or transaction can update a document's shard
 * key if the updateDocumentShardKeyUsingTransactionApi feature flag is enabled.
 *
 * @tags: [
 *    requires_sharding,
 *    uses_transactions,
 *    uses_multi_shard_transaction,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");

const st = new ShardingTest({shards: 2});
const shard0Primary = st.rs0.getPrimary();

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
const db = st.getDB(dbName);
const testColl = db.getCollection(collName);

const updateDocumentShardKeyUsingTransactionApiEnabled =
    isUpdateDocumentShardKeyUsingTransactionApiEnabled(st.s);

// Set up a sharded collection with two shards:
// shard0: [MinKey, 0]
// shard1: [0, MaxKey]
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: ns, find: {x: MinKey}, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1}, to: st.shard1.shardName}));

/**
 * Asserts that when the updateDocumentShardKeyUsingTransactionApi feature flag is enabled, ordinary
 * updates that modify a document's shard key will complete.
 */
function runAndVerifyCommandChangingOwningShard(cmdObj, expectedUpdatedDoc) {
    const updateRes = db.runCommand(cmdObj);
    const numDocsUpdated = testColl.find(expectedUpdatedDoc).itcount();

    if (updateDocumentShardKeyUsingTransactionApiEnabled) {
        assert.commandWorked(updateRes);
        assert.eq(1, numDocsUpdated);
    } else {
        assert.commandFailedWithCode(updateRes, ErrorCodes.IllegalOperation);
        assert.eq(0, numDocsUpdated);
    }
}

// This test is meant to test updating document shard keys without a session.
TestData.disableImplicitSessions = true;

assert.commandWorked(db.runCommand({insert: collName, documents: [{x: -1}]}));

/**
 * Verify that a client with the feature flag updateDocumentShardKeyUsingTransactionApi enabled can
 * update a shard key that change's the document's owning shard without a session.
 * Test 1: update()
 * Test 2: findAndModify()
 */
(() => {
    assert.commandWorked(db.runCommand({insert: collName, documents: [{x: -1}]}));
    let cmdObj = {update: collName, updates: [{q: {x: -1}, u: {"$set": {x: 1}}, upsert: false}]};
    runAndVerifyCommandChangingOwningShard(cmdObj, {x: 1});

    assert.commandWorked(db.runCommand({insert: collName, documents: [{x: -2}]}));
    cmdObj = {findAndModify: collName, query: {x: -2}, update: {"$set": {x: 2}}, upsert: false};
    runAndVerifyCommandChangingOwningShard(cmdObj, {x: 2});
})();

/**
 * Verify that a client can update a document's shard key without a session, where the update
 * upserts a document.
 * Test 1: update() with upsert: true
 * Test 2: findAndModify() with upsert: true
 */
(() => {
    let cmdObj = {update: collName, updates: [{q: {x: -3}, u: {"$set": {x: 3}}, upsert: true}]};
    assert.eq(0, testColl.find({x: -3}).itcount());
    runAndVerifyCommandChangingOwningShard(cmdObj, {x: 3});

    cmdObj = {findAndModify: collName, query: {x: -4}, update: {"$set": {x: 4}}, upsert: true};
    assert.eq(0, testColl.find({x: -4}).itcount());
    runAndVerifyCommandChangingOwningShard(cmdObj, {x: 4});
})();

/**
 * Verify that a client update a document's shard key inside a session, that would cause an existing
 * doc to move owning shard.
 * Test 1: update() inside session
 * Test 2: findAndModify() inside session
 */
const lsid = ({id: UUID()});
(() => {
    assert.commandWorked(db.runCommand({insert: collName, documents: [{x: -5}]}));
    let cmdObj = {
        update: collName,
        updates: [{q: {x: -5}, u: {"$set": {x: 5}}, upsert: false}],
        lsid: lsid
    };
    runAndVerifyCommandChangingOwningShard(cmdObj, {x: 5});

    assert.commandWorked(db.runCommand({insert: collName, documents: [{x: -6}]}));
    cmdObj = {
        findAndModify: collName,
        query: {x: -6},
        update: {"$set": {x: 6}},
        upsert: false,
        lsid: lsid
    };
    runAndVerifyCommandChangingOwningShard(cmdObj, {x: 6});
})();

/**
 * Verify that a client can update a document's shard key inside a session with an update
 * that would cause the document to move from shard0 to shard1, where the update upserts a document.
 * Test 1: update() with upsert: true inside session
 * Test 2: findAndModify() with upsert: inside session
 */
(() => {
    let cmdObj = {
        update: collName,
        updates: [{q: {x: -7}, u: {"$set": {x: 7}}, upsert: true}],
        lsid: lsid
    };
    assert.eq(0, testColl.find({x: -7}).itcount());
    runAndVerifyCommandChangingOwningShard(cmdObj, {x: 7});

    cmdObj = {
        findAndModify: collName,
        query: {x: -8},
        update: {"$set": {x: 8}},
        upsert: true,
        lsid: lsid
    };
    assert.eq(0, testColl.find({x: -8}).itcount());
    runAndVerifyCommandChangingOwningShard(cmdObj, {x: 8});
})();

st.stop();
})();
