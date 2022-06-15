/**
 * Validate batched multi-deleter's parameters.
 *
 * @tags: [
 *  requires_fcv_61,
 *  requires_replication,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/fail_point_util.js");  // For 'configureFailPoint()'

const rst = new ReplSetTest({
    nodes: 2,
});
rst.startSet();
rst.initiate();
rst.awaitNodesAgreeOnPrimary();
const conn = rst.getPrimary();

const db = conn.getDB("test");
const coll = db.getCollection("c");

function validateTargetDocsPerBatch() {
    const collCount = 1234;

    assert.commandWorked(db.adminCommand({setParameter: 1, batchedDeletesTargetBatchTimeMS: 0}));

    for (let docsPerBatch of [0, 1, 20, 100]) {
        jsTestLog("Validating targetBatchDocs=" + docsPerBatch);

        coll.drop();
        assert.commandWorked(
            coll.insertMany([...Array(collCount).keys()].map(x => ({_id: x, a: "a".repeat(10)}))));

        assert.commandWorked(
            db.adminCommand({setParameter: 1, batchedDeletesTargetBatchDocs: docsPerBatch}));

        // batchedDeletesTargetBatchDocs := 0 means no limit.
        const expectedBatches = docsPerBatch ? Math.ceil(collCount / docsPerBatch) : 1;
        const serverStatusBatchesBefore = db.serverStatus()['batchedDeletes']['batches'];
        const serverStatusDocsBefore = db.serverStatus()['batchedDeletes']['docs'];

        assert.eq(collCount, coll.find().itcount());
        assert.commandWorked(coll.deleteMany({_id: {$gte: 0}}));
        assert.eq(0, coll.find().itcount());

        const serverStatusBatchesAfter = db.serverStatus()['batchedDeletes']['batches'];
        const serverStatusDocsAfter = db.serverStatus()['batchedDeletes']['docs'];
        const serverStatusDocsExpected = serverStatusDocsBefore + collCount;
        const serverStatusBatchesExpected = serverStatusBatchesBefore + expectedBatches;
        assert.eq(serverStatusBatchesAfter, serverStatusBatchesExpected);
        assert.eq(serverStatusDocsAfter, serverStatusDocsExpected);

        rst.awaitReplication();
        rst.checkReplicatedDataHashes();
    }
}

function validateTargetBatchTimeMS() {
    const collCount = 10;

    assert.commandWorked(db.adminCommand({setParameter: 1, batchedDeletesTargetBatchDocs: 0}));

    for (let targetBatchTimeMS of [0, 1000]) {
        jsTestLog("Validating targetBatchTimeMS=" + targetBatchTimeMS);

        coll.drop();
        assert.commandWorked(
            coll.insertMany([...Array(collCount).keys()].map(x => ({_id: x, a: "a".repeat(10)}))));

        assert.commandWorked(
            db.adminCommand({setParameter: 1, batchedDeletesTargetBatchTimeMS: targetBatchTimeMS}));

        // batchedDeletesTargetBatchTimeMS := 0 means no limit.
        const expectedBatches = targetBatchTimeMS ? collCount : 1;
        const serverStatusBatchesBefore = db.serverStatus()['batchedDeletes']['batches'];
        const serverStatusDocsBefore = db.serverStatus()['batchedDeletes']['docs'];

        assert.eq(collCount, coll.find().itcount());

        // Make every delete take >> targetBatchTimeMS.
        const fp = configureFailPoint(db,
                                      "batchedDeleteStageSleepAfterNDocuments",
                                      {nDocs: 1, ns: coll.getFullName(), sleepMs: 2000});

        assert.commandWorked(coll.deleteMany({_id: {$gte: 0}}));
        assert.eq(0, coll.find().itcount());

        fp.off();
        const serverStatusBatchesAfter = db.serverStatus()['batchedDeletes']['batches'];
        const serverStatusDocsAfter = db.serverStatus()['batchedDeletes']['docs'];
        const serverStatusDocsExpected = serverStatusDocsBefore + collCount;
        const serverStatusBatchesExpected = serverStatusBatchesBefore + expectedBatches;
        assert.eq(serverStatusBatchesAfter, serverStatusBatchesExpected);
        assert.eq(serverStatusDocsAfter, serverStatusDocsExpected);

        rst.awaitReplication();
        rst.checkReplicatedDataHashes();
    }
}

function validateTargetStagedDocsBytes() {
    const collCount = 10000;
    const docPaddingBytes = 1024;
    const cumulativePaddingBytes = collCount *
        (bsonsize({_id: ObjectId(), a: 'a'}) +
         100 /* allow for getMemUsage() own metadata and overestimation */ + docPaddingBytes);

    assert.commandWorked(db.adminCommand({setParameter: 1, batchedDeletesTargetBatchTimeMS: 0}));
    assert.commandWorked(db.adminCommand({setParameter: 1, batchedDeletesTargetBatchDocs: 0}));

    for (let stagedDocsBytes of [0, 1024 * 1024, 5 * 1024 * 1024]) {
        jsTestLog("Validating stagedDocsBytes=" + stagedDocsBytes);

        assert.commandWorked(db.adminCommand(
            {setParameter: 1, batchedDeletesTargetStagedDocBytes: stagedDocsBytes}));

        coll.drop();
        assert.commandWorked(coll.insertMany(
            [...Array(collCount).keys()].map(x => ({a: "a".repeat(docPaddingBytes)}))));

        // batchedDeletesTargetStagedDocsBytes := 0 means no limit.
        const expectedBatches =
            stagedDocsBytes ? Math.ceil(cumulativePaddingBytes / stagedDocsBytes) : 1;
        const serverStatusBatchesBefore = db.serverStatus()['batchedDeletes']['batches'];
        const serverStatusDocsBefore = db.serverStatus()['batchedDeletes']['docs'];

        assert.eq(collCount, coll.find().itcount());
        assert.commandWorked(coll.deleteMany({}));
        assert.eq(0, coll.find().itcount());

        const serverStatusBatchesAfter = db.serverStatus()['batchedDeletes']['batches'];
        const serverStatusDocsAfter = db.serverStatus()['batchedDeletes']['docs'];
        const serverStatusDocsExpected = serverStatusDocsBefore + collCount;
        const serverStatusBatchesExpected = serverStatusBatchesBefore + expectedBatches;
        assert.eq(serverStatusBatchesAfter, serverStatusBatchesExpected);
        assert.eq(serverStatusDocsAfter, serverStatusDocsExpected);

        rst.awaitReplication();
        rst.checkReplicatedDataHashes();
    }
}

validateTargetDocsPerBatch();
validateTargetBatchTimeMS();
validateTargetStagedDocsBytes();

rst.stopSet();
})();
