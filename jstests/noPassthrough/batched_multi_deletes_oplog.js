/**
 * Validate oplog behaviour of batched multi-deletes.
 *
 * @tags: [
 *  requires_fcv_61,
 *  requires_replication,
 * ]
 */

(function() {
"use strict";

// Verifies that batches replicate as applyOps entries.
function validateBatchedDeletesOplogDocsPerBatch(conn) {
    const db = conn.getDB("test");
    const coll = db.getCollection("c");

    const docsPerBatch = 100;
    const collCount = 5017;  // Intentionally not a multiple of docsPerBatch.

    // Disable size-based batching
    assert.commandWorked(db.adminCommand({setParameter: 1, batchedDeletesTargetStagedDocBytes: 0}));
    // Disable time-based batching
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

// Verifies the batched deleter cuts batches that stay within the 16MB BSON limit of one applyOps
// entry.
function validateBatchedDeletesOplogBatchAbove16MB(conn) {
    const db = conn.getDB("test");
    const coll = db.getCollection("c");

    // With non-clustered collections (_id of ObjectId type) the batched deleter's conservative
    // applyOps estimation would cut a batch at ~63k documents. Create a collection >> 63k
    // documents, make the docsPerBatch target high enough to hit the 16MB BSON limit with applyOps,
    // and disable other batch tunables that could cut a batch earlier than expected.
    const docsPerBatch = 500000;
    const collCount = 130000;
    const expectedApplyOpsEntries =
        Math.ceil(collCount / 63600 /* max docs per batch, see comment above. */);
    // Disable size-based batching
    assert.commandWorked(db.adminCommand({setParameter: 1, batchedDeletesTargetStagedDocBytes: 0}));
    // Disable time-based batching
    assert.commandWorked(db.adminCommand({setParameter: 1, batchedDeletesTargetBatchTimeMS: 0}));
    // Set artificially high docs per batch target
    assert.commandWorked(
        db.adminCommand({setParameter: 1, batchedDeletesTargetBatchDocs: docsPerBatch}));

    coll.drop();
    assert.commandWorked(coll.insertMany([...Array(collCount).keys()].map(x => ({_id: x}))));

    assert.eq(collCount, coll.find().itcount());
    assert.commandWorked(coll.deleteMany({_id: {$gte: 0}}));
    assert.eq(0, coll.find().itcount());

    // The whole deletion breaks down the deletion into multiple batches/applyOps.
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
    assert.eq(expectedApplyOpsEntries, oplogEntriesApplyOps.length);
    let aggregateApplyOpsDeleteEntries = 0;
    for (let i = 0; i < expectedApplyOpsEntries; i++) {
        aggregateApplyOpsDeleteEntries += oplogEntriesApplyOps[i]['opsPerApplyOps'];
    }
    assert.eq(collCount, aggregateApplyOpsDeleteEntries);
}

function runTestInIsolation(testFunc, serverParam) {
    const nodeOptions = serverParam ? serverParam : {};
    const rst = new ReplSetTest({
        name: "batched_multi_deletes_test",
        nodes: 2,
        nodeOptions,
    });
    rst.startSet();
    rst.initiate();
    rst.awaitNodesAgreeOnPrimary();
    testFunc(rst.getPrimary());
    rst.awaitReplication();
    rst.checkReplicatedDataHashes();
    rst.stopSet();
}

// Validates oplog content for batches that fit within 16MB worth of oplog.
runTestInIsolation(validateBatchedDeletesOplogDocsPerBatch);
// Validates oplog content for batches that would exceed 16MB worth of oplog.
runTestInIsolation(validateBatchedDeletesOplogBatchAbove16MB);
})();
