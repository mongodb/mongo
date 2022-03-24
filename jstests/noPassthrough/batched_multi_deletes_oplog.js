/**
 * Validate oplog behaviour of batched multi-deletes.
 *
 * @tags: [
 *  # Running as a replica set requires journaling.
 *  requires_journaling,
 * ]
 */

(function() {
"use strict";

// Verifies that batches replicate as applyOps entries.
function validateBatchedDeletesOplogDocsPerBatch(conn) {
    // '__internalBatchedDeletesTesting.Collection0' is a special, hardcoded namespace that batches
    // multi-doc deletes if the 'internalBatchUserMultiDeletesForTest' server parameter is set.
    // TODO (SERVER-63044): remove this special handling.
    const db = conn.getDB("__internalBatchedDeletesTesting");
    const coll = db.getCollection('Collection0');

    const docsPerBatch = 100;
    const collCount = 5017;  // Intentionally not a multiple of docsPerBatch.

    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalBatchUserMultiDeletesForTest: 1}));
    // Disable time-based batching
    assert.commandWorked(db.adminCommand({setParameter: 1, batchedDeletesTargetBatchBytes: 0}));
    // Disable size-based batching
    assert.commandWorked(db.adminCommand({setParameter: 1, batchedDeletesTargetBatchTimeMS: 0}));
    // Set docs per batch target
    assert.commandWorked(
        db.adminCommand({setParameter: 1, batchedDeletesTargetBatchDocs: docsPerBatch}));

    coll.drop();
    assert.commandWorked(
        coll.insertMany([...Array(collCount).keys()].map(x => ({_id: x, a: "a".repeat(1024)}))));

    assert.eq(collCount, coll.find().itcount());
    assert.commandWorked(coll.deleteMany({_id: {$gte: 0}}));
    assert.eq(0, coll.find().itcount());

    // The deletion replicates as one applyOps per batch
    const applyOpsEntriesFull =
        db.getSiblingDB('local').oplog.rs.find({'o.applyOps': {$size: docsPerBatch}}).itcount();
    const applyOpsEntriesLast =
        db.getSiblingDB('local')
            .oplog.rs.find({'o.applyOps': {$size: collCount % docsPerBatch}})
            .itcount();
    const expectedApplyOpsEntries = Math.ceil(collCount / docsPerBatch);
    assert.eq(applyOpsEntriesFull + applyOpsEntriesLast, expectedApplyOpsEntries);
}

// Verifies that a large batch that would result in an applyOps entry beyond the 16MB BSON limit
// generates more than one applyOps entry.
function validateBatchedDeletesOplogBatchAbove16MB(conn) {
    const db = conn.getDB("__internalBatchedDeletesTesting");
    const coll = db.getCollection('Collection0');

    // With _id's of ObjectId type, and applyOps entry reaches the 16MB BSON limit at ~140k entries.
    // Create a collection >> 140k documents, make the docsPerBatch target high enough to hit the
    // 16MB BSON limit with applyOps, and disable other batch tunables that could cut a batch
    // earlier than expected.
    const docsPerBatch = 500000;
    const collCount = 200000;
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalBatchUserMultiDeletesForTest: 1}));
    // Disable time-based batching
    assert.commandWorked(db.adminCommand({setParameter: 1, batchedDeletesTargetBatchBytes: 0}));
    // Disable size-based batching
    assert.commandWorked(db.adminCommand({setParameter: 1, batchedDeletesTargetBatchTimeMS: 0}));
    // Set artificially high docs per batch target
    assert.commandWorked(
        db.adminCommand({setParameter: 1, batchedDeletesTargetBatchDocs: docsPerBatch}));

    coll.drop();
    assert.commandWorked(coll.insertMany([...Array(collCount).keys()].map(x => ({_id: x}))));

    assert.eq(collCount, coll.find().itcount());
    assert.commandWorked(coll.deleteMany({_id: {$gte: 0}}));
    assert.eq(0, coll.find().itcount());

    // The whole deletion replicates as two applyOps.
    const oplogEntriesApplyOps = db.getSiblingDB('local')
                                     .oplog.rs
                                     .aggregate([
                                         {
                                             $match: {
                                                 ns: "admin.$cmd",
                                                 'o.applyOps.op': 'd',
                                                 'o.applyOps.ns': coll.getFullName()
                                             }
                                         },
                                         {$project: {opsPerApplyOps: {$size: '$o.applyOps'}}}
                                     ])
                                     .toArray();
    assert.eq(2, oplogEntriesApplyOps.length);
    assert.eq(
        collCount,
        oplogEntriesApplyOps[0]['opsPerApplyOps'] + oplogEntriesApplyOps[1]['opsPerApplyOps']);
}

function runTestInIsolation(testFunc) {
    const rst = new ReplSetTest({
        name: "batched_multi_deletes_test",
        nodes: 1,
    });
    rst.startSet();
    rst.initiate();
    rst.awaitNodesAgreeOnPrimary();
    testFunc(rst.getPrimary());
    rst.stopSet();
}

runTestInIsolation(validateBatchedDeletesOplogDocsPerBatch);

// TODO (SERVER-64860): re-enable this test.
// runTestInIsolation(validateBatchedDeletesOplogBatchAbove16MB);
})();
