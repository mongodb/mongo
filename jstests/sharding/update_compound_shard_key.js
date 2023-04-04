/**
 * Tests to validate the functionality of update command in the presence of a compound shard key.
 *
 * @tags: [
 *   uses_multi_shard_transaction,
 *   uses_transactions,
 * ]
 */
(function() {
'use strict';

load("jstests/sharding/libs/sharded_transactions_helpers.js");
load("jstests/sharding/libs/update_shard_key_helpers.js");
load("jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js");

const st = new ShardingTest({mongos: 1, shards: 3});

const updateDocumentShardKeyUsingTransactionApiEnabled =
    isUpdateDocumentShardKeyUsingTransactionApiEnabled(st.s);

const kDbName = 'update_compound_sk';
const ns = kDbName + '.coll';
const session = st.s.startSession({retryWrites: true});
const sessionDB = session.getDatabase(kDbName);

assert.commandWorked(st.s0.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);

assert.commandWorked(
    st.s.getDB('config').adminCommand({shardCollection: ns, key: {x: 1, y: 1, z: 1}}));

let docsToInsert = [
    {_id: 0, x: 4, y: 3, z: 3},
    {_id: 1, x: 100, y: 50, z: 3, a: 5},
    {_id: 2, x: 100, y: 500, z: 3, a: 5}
];

// Make sure that shard0, shard1 and shard2 has _id 0,1 and 2 documents respectively.
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 100, y: 0, z: 3}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 100, y: 100, z: 3}}));

for (let i = 0; i < docsToInsert.length; i++) {
    assert.commandWorked(st.s.getDB(kDbName).coll.insert(docsToInsert[i]));
}

assert.commandWorked(st.s.adminCommand(
    {moveChunk: ns, find: {x: 100, y: 50, z: 3}, to: st.shard1.shardName, _waitForDelete: true}));
assert.commandWorked(st.s.adminCommand(
    {moveChunk: ns, find: {x: 100, y: 500, z: 3}, to: st.shard2.shardName, _waitForDelete: true}));

function assertUpdateWorked(query, update, isUpsert, _id) {
    const res = sessionDB.coll.update(query, update, {upsert: isUpsert});
    assert.commandWorked(res);
    assert.eq(0, res.nUpserted);
    assert.eq(1, res.nMatched);
    assert.eq(1, res.nModified);

    // Skip find based validation for pipleline update.
    if (!Array.isArray(update)) {
        if (update["$set"] != undefined) {
            update = update["$set"];
        }
        update["_id"] = _id;
        // Make sure that the update modified the document with the given _id.
        assert.eq(1, st.s.getDB(kDbName).coll.find(update).itcount());
    }
}

/**
 * For upserts this will insert a new document, for non-upserts it will be a no-op.
 */
function assertUpdateWorkedWithNoMatchingDoc(query, update, isUpsert, inTransaction) {
    const res = sessionDB.coll.update(query, update, {upsert: isUpsert});

    assert.commandWorked(res);
    assert.eq(isUpsert ? 1 : 0, res.nUpserted);
    assert.eq(0, res.nMatched);
    assert.eq(0, res.nModified);

    // Skip find based validation for pipleline update or when inside a transaction.
    if (Array.isArray(update) || inTransaction)
        return;

    // Make sure that the upsert inserted the correct document or update did not insert
    // anything.
    assert.eq(isUpsert ? 1 : 0,
              st.s.getDB(kDbName).coll.find(update["$set"] ? update["$set"] : update).itcount());
}

/**
 * Updates to a document's shard key that would change the document's owning shard will succeed if
 * the updateDocumentShardKeyUsingTransactionApi feature flag is enabled.
 */
function assertWouldChangeOwningShardUpdateResult(res, expectedUpdatedDoc) {
    const numDocsUpdated = st.s.getDB(kDbName).coll.find(expectedUpdatedDoc).itcount();

    if (updateDocumentShardKeyUsingTransactionApiEnabled) {
        assert.commandWorked(res);
        assert.eq(1, numDocsUpdated);
    } else {
        assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);
        assert.eq(0, numDocsUpdated);
    }
}

//
// Update Type Replacement-style.
//

// Test behaviours common to update and upsert.
[false,
 true]
    .forEach(function(isUpsert) {
        // Full shard key in query matches the update document.
        assertUpdateWorked({x: 4, y: 3, z: 3}, {x: 4, y: 3, z: 3, a: 0}, isUpsert, 0);
        assertUpdateWorked({x: 4, _id: 0, z: 3, y: 3}, {x: 4, y: 3, z: 3, a: 0}, isUpsert, 0);

        // Case when upsert needs to insert a new document and the new document should belong in the
        // same shard as the targeted shard. For non-upserts, it will be a no-op.
        assertUpdateWorkedWithNoMatchingDoc(
            {x: 4, y: 0, z: 0}, {x: 1, z: 3, y: 110, a: 90}, isUpsert);
    });

//
// Test behaviours specific to non-upsert updates.
//

// Partial shard key in query can target a single shard, and shard key of existing document is
// the same as the replacement's.
assertUpdateWorked({x: 4}, {x: 4, y: 3, z: 3, a: 1}, false, 0);
assertUpdateWorked({x: 4, _id: 0, z: 3}, {y: 3, x: 4, z: 3, a: 3}, false, 0);

// Parital shard key in the query, update succeeds with no op when there is no matching document
// for the query.
assertUpdateWorkedWithNoMatchingDoc({x: 10}, {x: 10, y: 3, z: 3, a: 5}, false);
assertUpdateWorkedWithNoMatchingDoc({x: 100, y: 55, a: 15}, {x: 100, y: 55, z: 3, a: 6}, false);
assertUpdateWorkedWithNoMatchingDoc({x: 11, _id: 3}, {x: 11, y: 3, z: 3, a: 7}, false);

// Shard key field modifications do not have to specify full shard key.
if (!WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(st.s)) {
    // Partial shard key in query can target a single shard, but fails while attempting to
    // modify shard key value.
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update(
            {x: 100, y: 50, a: 5}, {x: 100, y: 55, z: 3, a: 1}, {upsert: false}),
        31025);
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update({x: 4, z: 3}, {x: 4, y: 3, z: 4, a: 1}, {upsert: false}),
        31025);
}

// Full shard key in query, matches no document.
assertUpdateWorkedWithNoMatchingDoc({x: 4, y: 0, z: 0}, {x: 1110, y: 55, z: 3, a: 111}, false);

// Partial shard key in query, but can still target a single shard.
assertUpdateWorkedWithNoMatchingDoc({x: 100, y: 51, a: 5}, {x: 110, y: 55, z: 3, a: 8}, false);

// Partial shard key in query cannot target a single shard, targeting happens using update
// document.

// When query doesn't match any doc.
assertUpdateWorkedWithNoMatchingDoc({x: 4, y: 0}, {x: 110, y: 55, z: 3, a: 110}, false);
assertUpdateWorkedWithNoMatchingDoc({_id: 1}, {x: 110, y: 55, z: 3, a: 110}, false);

// When query matches a doc and updates sucessfully.
assertUpdateWorked({_id: 0, y: 3}, {z: 3, x: 4, y: 3, a: 2}, false, 0);
assertUpdateWorked({_id: 0}, {z: 3, x: 4, y: 3, replStyle: 2}, false, 0);

// Shard key field modifications do not have to specify full shard key.
if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(st.s)) {
    const testDB = st.s.getDB("test");
    const testColl = testDB.coll;

    // Shard testColl on {x:1}, split it at {x:0}, and move chunk {x:1} to shard1. This collection
    // is used to for the update below which would use the write without shard key protocol, but
    // since the query is unspecified, any 1 random document could be modified. In order to not
    // break the state of the original test collection, 'testColl' is used specifically for the
    // single update below.
    st.shardColl(testColl, {x: 1}, {x: 0}, {x: 1});

    assert.commandWorked(testColl.insert({x: 1, _id: 1}));
    assert.commandWorked(testColl.insert({x: -1, _id: 0}));

    // TODO: SERVER-67429 Remove this try/catch since we can run in all configurations.
    // If we have a WouldChangeOwningShard update and we aren't running as a retryable
    // write or in a transaction, then this is an acceptable error.
    let updateRes;
    try {
        updateRes = testColl.update({}, {x: 110, y: 55, z: 3, a: 110}, false);
        assert.commandWorked(updateRes);
        assert.eq(1, updateRes.nMatched);
        assert.eq(1, updateRes.nModified);
        assert.eq(testColl.find({x: 110, y: 55, z: 3, a: 110}).itcount(), 1);
    } catch (err) {
        // If a WouldChangeOwningShard update is performed not as a retryable write or in a
        // transaction, expect an error.
        assert.eq(updateRes.getWriteError().code, ErrorCodes.IllegalOperation);
        assert(updateRes.getWriteError().errmsg.includes(
            "Must run update to shard key field in a multi-statement transaction or with " +
            "retryWrites: true."));
    }

} else {
    // When query matches a doc and fails to update because shard key needs to be updated.
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update({}, {x: 110, y: 55, z: 3, a: 110}, false), 31025);
}

assert.commandFailedWithCode(
    st.s.getDB(kDbName).coll.update({_id: 2}, {x: 110, y: 55, z: 3, a: 110}, false), 31025);

//
// Test upsert-specific behaviours.
//

// Case when upsert needs to insert a new document and the new document should belong in a shard
// other than the one targeted by the update. These upserts can only succeed when the
// updateDocumentShardKeyUsingTransactionApi feature flag is enabled or if not enabled, when the
// upsert is run in a multi-statement transaction or with retryWrites: true.
const updateDoc = {
    x: 1110,
    y: 55,
    z: 3,
    replStyleUpdate: true
};
const updateRes = st.s.getDB(kDbName).coll.update({x: 4, y: 0, z: 0}, updateDoc, {upsert: true});
assertWouldChangeOwningShardUpdateResult(updateRes, updateDoc);

const updateDocTxn = {
    x: 1110,
    y: 55,
    z: 4,
    replStyleUpdate: true
};
session.startTransaction();
assertUpdateWorkedWithNoMatchingDoc(
    {x: 4, y: 0, z: 0}, updateDocTxn, true /*isUpsert*/, true /*inTransaction*/);
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq(1, sessionDB.coll.find(updateDocTxn).itcount());

// Shard key field modifications do not have to specify full shard key.
if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(st.s)) {
    // Query on partial shard key.
    assertUpdateWorkedWithNoMatchingDoc({x: 101, y: 50, a: 5},
                                        {x: 100, y: 55, z: 3, a: 1},
                                        true /* isUpsert */,
                                        false /* inTransaction */);
    assertUpdateWorkedWithNoMatchingDoc({x: 102, y: 50, nonExistingField: true},
                                        {x: 102, y: 55, z: 3, a: 1},
                                        true /* isUpsert */,
                                        false /* inTransaction */);

    // Query on partial shard key with _id.
    assertUpdateWorkedWithNoMatchingDoc({x: 100, y: 50, a: 5, _id: 23},
                                        {x: 100, y: 55, z: 3, a: 2},
                                        true /* isUpsert */,
                                        false /* inTransaction */);
    assertUpdateWorkedWithNoMatchingDoc({x: 100, y: 50, a: 5, _id: 24, nonExistingField: true},
                                        {x: 100, y: 55, z: 3, a: 3},
                                        true /* isUpsert */,
                                        false /* inTransaction */);

    // Query on only _id.
    assertUpdateWorkedWithNoMatchingDoc(
        {_id: 25}, {z: 3, x: 4, y: 3, a: 4}, true /* isUpsert */, false /* inTransaction */);
    assertUpdateWorkedWithNoMatchingDoc({_id: "nonExisting"},
                                        {z: 3, x: 4, y: 3, a: 5},
                                        true /* isUpsert */,
                                        false /* inTransaction */);
} else {
    // Full shard key not specified in query.

    // Query on partial shard key.
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update(
            {x: 100, y: 50, a: 5}, {x: 100, y: 55, z: 3, a: 1}, {upsert: true}),
        ErrorCodes.ShardKeyNotFound);
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update(
            {x: 100, y: 50, nonExistingField: true}, {x: 100, y: 55, z: 3, a: 1}, {upsert: true}),
        ErrorCodes.ShardKeyNotFound);

    // Query on partial shard key with _id.
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update(
            {x: 100, y: 50, a: 5, _id: 0}, {x: 100, y: 55, z: 3, a: 1}, {upsert: true}),
        ErrorCodes.ShardKeyNotFound);
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update({x: 100, y: 50, a: 5, _id: 0, nonExistingField: true},
                                        {x: 100, y: 55, z: 3, a: 1},
                                        {upsert: true}),
        ErrorCodes.ShardKeyNotFound);

    // Query on only _id.
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update({_id: 0}, {z: 3, x: 4, y: 3, a: 2}, {upsert: true}),
        ErrorCodes.ShardKeyNotFound);
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update(
            {_id: "nonExisting"}, {z: 3, x: 4, y: 3, a: 2}, {upsert: true}),
        ErrorCodes.ShardKeyNotFound);
}

//
// Update Type Op-style.
//

// Test behaviours common to update and upsert.
[false,
 true]
    .forEach(function(isUpsert) {
        // Full shard key in query.
        assertUpdateWorked({x: 4, _id: 0, z: 3, y: 3}, {"$set": {opStyle: 1}}, isUpsert, 0);
        assertUpdateWorked({x: 4, z: 3, y: 3}, {"$set": {opStyle: 2}}, isUpsert, 0);

        // Case when upsert needs to insert a new document and the new document should belong in the
        // same shard as the targetted shard. For non-upserts, it will be a no op.
        assertUpdateWorkedWithNoMatchingDoc(
            {x: 4, y: 0, z: 0}, {"$set": {x: 1, z: 3, y: 111, a: 90}}, isUpsert);
    });

// Test behaviours specific to non-upsert updates.

// Full shard key in query, matches no document.
assertUpdateWorkedWithNoMatchingDoc(
    {x: 4, y: 0, z: 0}, {"$set": {x: 2110, y: 55, z: 3, a: 111}}, false);

// Partial shard key in query, but can still target a single shard.
assertUpdateWorkedWithNoMatchingDoc(
    {x: 100, y: 51, a: 112}, {"$set": {x: 110, y: 55, z: 3, a: 8}}, false);

// Query on _id works for update.
assertUpdateWorked({_id: 0}, {"$set": {opStyle: 6}}, false, 0);
assertUpdateWorked({_id: 0, y: 3}, {"$set": {opStyle: 8, y: 3, x: 4}}, false, 0);

// Parital shard key in the query targets single shard. Update succeeds with no op when there is
// no matching document for the query.
assertUpdateWorkedWithNoMatchingDoc({x: 14, _id: 0}, {"$set": {opStyle: 5}}, false);
assertUpdateWorkedWithNoMatchingDoc({x: 14}, {"$set": {opStyle: 5}}, false);

assertUpdateWorkedWithNoMatchingDoc({x: -1, y: 0}, {"$set": {z: 3, y: 110, a: 91}}, false);

// Partial shard key in query can target a single shard and doesn't try to update shard key
// value.
assertUpdateWorked({x: 4, z: 3}, {"$set": {opStyle: 3}}, false, 0);
assertUpdateWorked({x: 4, _id: 0, z: 3}, {"$set": {y: 3, x: 4, z: 3, opStyle: 4}}, false, 0);

// Shard key field modifications do not have to specify full shard key.
if (!WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(st.s)) {
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update({}, {x: 110, y: 55, z: 3, a: 110}, false), 31025);
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update({_id: 2}, {x: 110, y: 55, z: 3, a: 110}, false), 31025);
    // Partial shard key in query can target a single shard, but fails while attempting to modify
    // shard key value.
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update(
            {_id: 1, x: 100, z: 3, a: 5}, {"$set": {y: 55, a: 11}}, {upsert: false}),
        31025);
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update(
            {x: 4, z: 3}, {"$set": {x: 4, y: 3, z: 4, a: 1}}, {upsert: false}),
        31025);
}

// Test upsert-specific behaviours.

// Case when upsert needs to insert a new document and the new document should belong in a shard
// other than the one targeted by the update. These upserts can only succeed when the
// updateDocumentShardKeyUsingTransactionApi feature flag is enabled or if not enabled, when the
// upsert is run in a multi-statement transaction or with retryWrites: true.
const upsertDoc = {
    x: 2110,
    y: 55,
    z: 3,
    opStyle: true
};
const upsertRes =
    st.s.getDB(kDbName).coll.update({x: 4, y: 0, z: 0, opStyle: true}, upsertDoc, {upsert: true});
assertWouldChangeOwningShardUpdateResult(upsertRes, upsertDoc);

const upsertDocTxn = {
    "$set": {x: 2110, y: 55, z: 4, opStyle: true}
};
session.startTransaction();
assertUpdateWorkedWithNoMatchingDoc({x: 4, y: 0, z: 0, opStyle: true}, upsertDocTxn, true, true);
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq(1, sessionDB.coll.find(upsertDocTxn["$set"]).itcount());

// Shard key field modifications do not have to specify full shard key.
if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(st.s)) {
    // Partial shard key updates are successful.
    assertUpdateWorkedWithNoMatchingDoc(
        {x: 14}, {"$set": {opStyle: 5}}, true /* isUpsert */, false /* inTransaction */);
    assertUpdateWorkedWithNoMatchingDoc({x: 100, y: 51, nonExistingField: true},
                                        {"$set": {x: 110, y: 55, z: 3, a: 8}},
                                        true /* isUpsert */,
                                        false /* inTransaction */);
    assertUpdateWorkedWithNoMatchingDoc(
        {y: 4}, {"$set": {z: 3, x: 4, y: 3, a: 2}}, true /* isUpsert */, false /* inTransaction */);

    // Upserts that match on _id are successful.
    assertUpdateWorkedWithNoMatchingDoc({_id: 20},
                                        {"$set": {x: 2, y: 11, z: 10, opStyle: 7}},
                                        true /* isUpsert */,
                                        false /* inTransaction */);
    assertUpdateWorkedWithNoMatchingDoc({_id: 21, y: 3},
                                        {"$set": {z: 3, x: 4, y: 3, a: 1}},
                                        true /* isUpsert */,
                                        false /* inTransaction */);
} else {
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update(
            {_id: 0}, {"$set": {x: 2, y: 11, z: 10, opStyle: 7}}, {upsert: true}),
        ErrorCodes.ShardKeyNotFound);

    // Partial shard key can target single shard. This style of update can work if SERVER-41243 is
    // implemented.
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update({x: 14}, {"$set": {opStyle: 5}}, {upsert: true}),
        ErrorCodes.ShardKeyNotFound);
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update({x: 100, y: 51, nonExistingField: true},
                                        {"$set": {x: 110, y: 55, z: 3, a: 8}},
                                        {upsert: true}),
        ErrorCodes.ShardKeyNotFound);

    // Partial shard key cannot target single shard.
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update(
            {_id: 0, y: 3}, {"$set": {z: 3, x: 4, y: 3, a: 2}}, {upsert: true}),
        ErrorCodes.ShardKeyNotFound);
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update({y: 3}, {"$set": {z: 3, x: 4, y: 3, a: 2}}, {upsert: true}),
        ErrorCodes.ShardKeyNotFound);
}
//
// Update with pipeline.
//

// Test behaviours common to update and upsert.
[false,
 true]
    .forEach(function(isUpsert) {
        // Full shard key in query.
        assertUpdateWorked(
            {_id: 0, x: 4, z: 3, y: 3}, [{$addFields: {pipelineUpdate: isUpsert}}], isUpsert, 0);
        assert.eq(1,
                  st.s.getDB(kDbName)
                      .coll.find({_id: 0, x: 4, z: 3, y: 3, pipelineUpdate: isUpsert})
                      .itcount());
        assertUpdateWorkedWithNoMatchingDoc(
            {_id: 15, x: 44, z: 3, y: 3}, [{$addFields: {pipelineUpdate: true}}], isUpsert);
        assert.eq(isUpsert ? 1 : 0,
                  st.s.getDB(kDbName)
                      .coll.find({_id: 15, x: 44, z: 3, y: 3, pipelineUpdate: true})
                      .itcount());

        assertUpdateWorkedWithNoMatchingDoc(
            {x: 45, z: 4, y: 3}, [{$addFields: {pipelineUpdate: true}}], isUpsert);
        assert.eq(
            isUpsert ? 1 : 0,
            st.s.getDB(kDbName).coll.find({x: 45, z: 4, y: 3, pipelineUpdate: true}).itcount());

        // Case when upsert needs to insert a new document and the new document should belong in the
        // same shard as the targeted shard.
        assertUpdateWorkedWithNoMatchingDoc({x: 4, y: 0, z: 0},
                                            [{
                                                "$project": {
                                                    x: {$literal: 3},
                                                    y: {$literal: 33},
                                                    z: {$literal: 3},
                                                    pipelineUpdate: {$literal: true}
                                                }
                                            }],
                                            isUpsert);
        assert.eq(
            isUpsert ? 1 : 0,
            st.s.getDB(kDbName).coll.find({x: 3, z: 3, y: 33, pipelineUpdate: true}).itcount());
    });

// Test behaviours specific to non-upsert updates.

// Full shard key in query, matches no document.
assertUpdateWorkedWithNoMatchingDoc({x: 4, y: 0, z: 0},
                                    [{
                                        "$project": {
                                            x: {$literal: 2111},
                                            y: {$literal: 55},
                                            z: {$literal: 3},
                                            pipelineUpdate: {$literal: true}
                                        }
                                    }],
                                    false);
assert.eq(0, st.s.getDB(kDbName).coll.find({x: 2111, z: 3, y: 55, pipelineUpdate: true}).itcount());

// Partial shard key in query targets single shard but doesn't match any document on that shard.
assertUpdateWorkedWithNoMatchingDoc({_id: 14, z: 4, x: 3}, [{$addFields: {foo: 4}}], false);

// Partial shard key in query can target a single shard and doesn't try to update shard key
// value.
assertUpdateWorkedWithNoMatchingDoc(
    {x: 46, z: 4}, [{$addFields: {y: 10, pipelineUpdateNoOp: false}}], false);
assertUpdateWorked({x: 4, z: 3}, [{$addFields: {pipelineUpdateDoc: false}}], false, 0);

// Shard key field modifications do not have to specify full shard key.
if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(st.s)) {
    assert.commandWorked(
        st.s.getDB(kDbName).coll.update({z: 3, y: 3}, [{$addFields: {foo: 4}}], {upsert: false}));
} else {
    // Partial shard key in query cannot target a single shard.
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update({z: 3, y: 3}, [{$addFields: {foo: 4}}], {upsert: false}),
        [72, ErrorCodes.InvalidOptions]);
}

// Test upsert-specific behaviours.

// Case when upsert needs to insert a new document and the new document should belong in a shard
// other than the one targeted by the update. These upserts can only succeed when run in a
// multi-statement transaction or with retryWrites: true or if the
// updateDocumentShardKeyUsingTransactionApi feature flag is enabled.
const upsertProjectDoc = {
    x: 2111,
    y: 55,
    z: 3
};
const upsertProjectRes = st.s.getDB(kDbName).coll.update({x: 4, y: 0, z: 0},
                                                         [{
                                                             "$project": {
                                                                 x: {$literal: 2111},
                                                                 y: {$literal: 55},
                                                                 z: {$literal: 3},
                                                                 pipelineUpdate: {$literal: true}
                                                             }
                                                         }],
                                                         {upsert: true});
assertWouldChangeOwningShardUpdateResult(upsertProjectRes, upsertProjectDoc);

const upsertProjectTxnDoc = {
    x: 2111,
    y: 55,
    z: 4
};
session.startTransaction();
assertUpdateWorkedWithNoMatchingDoc({x: 4, y: 0, z: 0, pipelineUpdate: true},
                                    [{
                                        "$project": {
                                            x: {$literal: 2111},
                                            y: {$literal: 55},
                                            z: {$literal: 4},
                                            pipelineUpdate: {$literal: true}
                                        }
                                    }],
                                    true);
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq(1, sessionDB.coll.find(upsertProjectTxnDoc).itcount());

if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(st.s)) {
    assertUpdateWorkedWithNoMatchingDoc({_id: 18, z: 4, x: 3},
                                        [{$addFields: {foo: 4}}],
                                        true /* isUpsert */,
                                        false /* inTransaction */);
    assertUpdateWorkedWithNoMatchingDoc({_id: 22},
                                        [{
                                            "$project": {
                                                x: {$literal: 2111},
                                                y: {$literal: 55},
                                                z: {$literal: 3},
                                                pipelineUpdate: {$literal: true}
                                            }
                                        }],
                                        true /* isUpsert */,
                                        false /* inTransaction */);
} else {
    // Full shard key not specified in query.
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update(
            {_id: 18, z: 4, x: 3}, [{$addFields: {foo: 4}}], {upsert: true}),
        ErrorCodes.ShardKeyNotFound);
    assert.commandFailedWithCode(
        st.s.getDB(kDbName).coll.update({_id: 0},
                                        [{
                                            "$project": {
                                                x: {$literal: 2111},
                                                y: {$literal: 55},
                                                z: {$literal: 3},
                                                pipelineUpdate: {$literal: true}
                                            }
                                        }],
                                        {upsert: true}),
        ErrorCodes.ShardKeyNotFound);
}
st.stop();
})();
