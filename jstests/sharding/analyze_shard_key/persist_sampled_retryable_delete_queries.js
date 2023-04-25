/**
 * Tests that retrying a retryable delete doesn't cause it to have multiple sampled query documents.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

// Make the periodic job for writing sampled queries have a period of 1 second to speed up the test.
const queryAnalysisWriterIntervalSecs = 1;

function testRetryExecutedWrite(rst) {
    const dbName = "testDb";
    const collName = "testCollExecutedWrite";
    const ns = dbName + "." + collName;

    const lsid = {id: UUID()};
    const txnNumber = NumberLong(1);

    const primary = rst.getPrimary();
    const db = primary.getDB(dbName);
    const coll = db.getCollection(collName);
    assert.commandWorked(coll.insert([{a: -1}, {a: 0}]));
    const collectionUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    const deleteOp0 = {q: {a: 0}, limit: 1, sampleId: UUID()};
    const deleteOp1 = {q: {a: {$lt: 1}}, limit: 1, sampleId: UUID()};

    const originalCmdObj = {delete: collName, deletes: [deleteOp0], lsid, txnNumber};
    const expectedSampledQueryDocs = [{
        sampleId: deleteOp0.sampleId,
        cmdName: "delete",
        cmdObj: QuerySamplingUtil.makeCmdObjIgnoreSessionInfo(originalCmdObj)
    }];

    const originalRes = assert.commandWorked(db.runCommand(originalCmdObj));
    assert.eq(originalRes.n, 1, originalRes);

    QuerySamplingUtil.assertSoonSampledQueryDocuments(
        primary, ns, collectionUuid, expectedSampledQueryDocs);

    // Retry deleteOp0 with the same sampleId but batched with the new deleteOp1.
    const retryCmdObj0 = Object.assign({}, originalCmdObj);
    retryCmdObj0.deletes = [deleteOp0, deleteOp1];
    expectedSampledQueryDocs.push({
        sampleId: deleteOp1.sampleId,
        cmdName: "delete",
        cmdObj: Object.assign(QuerySamplingUtil.makeCmdObjIgnoreSessionInfo(retryCmdObj0),
                              {deletes: [deleteOp1]})
    });

    const retryRes0 = assert.commandWorked(db.runCommand(retryCmdObj0));
    assert.eq(retryRes0.n, 2, retryRes0);

    QuerySamplingUtil.assertSoonSampledQueryDocuments(
        primary, ns, collectionUuid, expectedSampledQueryDocs);

    // Retry both deleteOp0 and deleteOp1 different sampleIds.
    const retryCmdObj1 = Object.assign({}, retryCmdObj0);
    retryCmdObj1.deletes = [
        Object.assign({}, deleteOp0, {sampleId: UUID()}),
        Object.assign({}, deleteOp1, {sampleId: UUID()})
    ];

    const retryRes1 = assert.commandWorked(db.runCommand(retryCmdObj1));
    assert.eq(retryRes1.n, 2, retryRes1);

    // Wait for one interval to verify that no writes occurred as a result of the retry.
    sleep(queryAnalysisWriterIntervalSecs * 1000);

    QuerySamplingUtil.assertSoonSampledQueryDocuments(
        primary, ns, collectionUuid, expectedSampledQueryDocs);
}

function testRetryUnExecutedWrite(rst) {
    const dbName = "testDb";
    const collName = "testCollUnExecutedWrite";
    const ns = dbName + "." + collName;

    const lsid = {id: UUID()};
    const txnNumber = NumberLong(1);

    const primary = rst.getPrimary();
    const db = primary.getDB(dbName);
    const coll = db.getCollection(collName);
    assert.commandWorked(coll.insert({a: 0}));
    const collectionUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    const deleteOp0 = {q: {a: 0}, limit: 1, sampleId: UUID()};
    const originalCmdObj = {delete: collName, deletes: [deleteOp0], lsid, txnNumber};
    const expectedSampledQueryDocs = [{
        sampleId: deleteOp0.sampleId,
        cmdName: "delete",
        cmdObj: QuerySamplingUtil.makeCmdObjIgnoreSessionInfo(originalCmdObj)
    }];

    const fp = configureFailPoint(primary, "failAllRemoves");

    // The delete fails after it has been added to the sample buffer.
    assert.commandFailedWithCode(db.runCommand(originalCmdObj), ErrorCodes.InternalError);

    QuerySamplingUtil.assertSoonSampledQueryDocuments(
        primary, ns, collectionUuid, expectedSampledQueryDocs);

    fp.off();

    // Retry with the same sampleId.
    const retryCmdObj = originalCmdObj;
    const retryRes = assert.commandWorked(db.runCommand(retryCmdObj));
    assert.eq(retryRes.n, 1, retryRes);

    // Wait for one interval to verify that no writes occurred as a result of the retry.
    sleep(queryAnalysisWriterIntervalSecs * 1000);

    QuerySamplingUtil.assertSoonSampledQueryDocuments(
        primary, ns, collectionUuid, expectedSampledQueryDocs);
}

const st = new ShardingTest({
    shards: 1,
    rs: {
        nodes: 2,
        // Make the periodic job for writing sampled queries have a period of 1 second to speed up
        // the test.
        setParameter: {queryAnalysisWriterIntervalSecs}
    }
});

// Force samples to get persisted even though query sampling is not enabled.
QuerySamplingUtil.skipActiveSamplingCheckWhenPersistingSamples(st);

testRetryExecutedWrite(st.rs0);
testRetryUnExecutedWrite(st.rs0);

st.stop();
})();
