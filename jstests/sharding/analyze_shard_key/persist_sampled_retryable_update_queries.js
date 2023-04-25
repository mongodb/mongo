/**
 * Tests that retrying a retryable update doesn't cause it to have multiple sampled query documents.
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
    assert.commandWorked(coll.insert({a: 0}));
    const collectionUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    const updateOp0 = {q: {a: 0}, u: {$set: {b: 0}}, multi: false, upsert: false, sampleId: UUID()};
    const updateOp1 =
        {q: {a: {$lt: 1}}, u: {$set: {b: "$x"}}, multi: false, upsert: true, sampleId: UUID()};

    const originalCmdObj = {update: collName, updates: [updateOp0], lsid, txnNumber};
    const expectedSampledQueryDocs = [{
        sampleId: updateOp0.sampleId,
        cmdName: "update",
        cmdObj: QuerySamplingUtil.makeCmdObjIgnoreSessionInfo(originalCmdObj)
    }];

    const originalRes = assert.commandWorked(db.runCommand(originalCmdObj));
    assert.eq(originalRes.nModified, 1, originalRes);

    QuerySamplingUtil.assertSoonSampledQueryDocuments(
        primary, ns, collectionUuid, expectedSampledQueryDocs);

    // Retry updateOp0 with the same sampleId but batched with the new updateOp1.
    const retryCmdObj0 = Object.assign({}, originalCmdObj);
    retryCmdObj0.updates = [updateOp0, updateOp1];
    expectedSampledQueryDocs.push({
        sampleId: updateOp1.sampleId,
        cmdName: "update",
        cmdObj: Object.assign(QuerySamplingUtil.makeCmdObjIgnoreSessionInfo(retryCmdObj0),
                              {updates: [updateOp1]})
    });

    const retryRes0 = assert.commandWorked(db.runCommand(retryCmdObj0));
    assert.eq(retryRes0.nModified, 2, retryRes0);

    QuerySamplingUtil.assertSoonSampledQueryDocuments(
        primary, ns, collectionUuid, expectedSampledQueryDocs);

    // Retry both updateOp0 and updateOp1 different sampleIds.
    const retryCmdObj1 = Object.assign({}, retryCmdObj0);
    retryCmdObj1.updates = [
        Object.assign({}, updateOp0, {sampleId: UUID()}),
        Object.assign({}, updateOp1, {sampleId: UUID()})
    ];

    const retryRes1 = assert.commandWorked(db.runCommand(retryCmdObj1));
    assert.eq(retryRes1.nModified, 2, retryRes1);

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

    const updateOp0 = {q: {a: 0}, u: {$set: {b: 0}}, multi: false, upsert: false, sampleId: UUID()};
    const originalCmdObj = {update: collName, updates: [updateOp0], lsid, txnNumber};
    const expectedSampledQueryDocs = [{
        sampleId: updateOp0.sampleId,
        cmdName: "update",
        cmdObj: QuerySamplingUtil.makeCmdObjIgnoreSessionInfo(originalCmdObj)
    }];

    const fp = configureFailPoint(primary, "failAllUpdates");

    // The update fails after it has been added to the sample buffer.
    assert.commandFailedWithCode(db.runCommand(originalCmdObj), ErrorCodes.InternalError);

    QuerySamplingUtil.assertSoonSampledQueryDocuments(
        primary, ns, collectionUuid, expectedSampledQueryDocs);

    fp.off();

    // Retry with the same sampleId.
    const retryCmdObj = originalCmdObj;
    const retryRes = assert.commandWorked(db.runCommand(retryCmdObj));
    assert.eq(retryRes.nModified, 1, retryRes);

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
