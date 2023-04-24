/**
 * Tests that retrying a retryable findAndModify doesn't cause it to have multiple sampled query
 * documents.
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

    const originalCmdObj = {
        findAndModify: collName,
        query: {a: 0},
        update: {$inc: {a: 1}},
        sampleId: UUID(),
        lsid,
        txnNumber
    };
    const expectedSampledQueryDocs = [{
        sampleId: originalCmdObj.sampleId,
        cmdName: "findAndModify",
        cmdObj: QuerySamplingUtil.makeCmdObjIgnoreSessionInfo(originalCmdObj)
    }];

    const originalRes = assert.commandWorked(db.runCommand(originalCmdObj));
    assert.eq(originalRes.lastErrorObject.n, 1, originalRes);

    QuerySamplingUtil.assertSoonSampledQueryDocuments(
        primary, ns, collectionUuid, expectedSampledQueryDocs);

    // Retry with the same sampleId.
    const retryCmdObj0 = originalCmdObj;
    const retryRes0 = assert.commandWorked(db.runCommand(retryCmdObj0));
    assert.eq(retryRes0.lastErrorObject.n, 1, retryRes0);

    // Wait for one interval to verify that no writes occurred as a result of the retry.
    sleep(queryAnalysisWriterIntervalSecs * 1000);

    QuerySamplingUtil.assertSoonSampledQueryDocuments(
        primary, ns, collectionUuid, expectedSampledQueryDocs);

    // Retry with a different sampleId.
    const retryCmdObj1 = Object.assign({}, originalCmdObj);
    retryCmdObj1.sampleId = UUID();
    const retryRes1 = assert.commandWorked(db.runCommand(retryCmdObj1));
    assert.eq(retryRes1.lastErrorObject.n, 1, retryRes1);

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

    const originalCmdObj = {
        findAndModify: collName,
        query: {a: 0},
        update: {$inc: {a: 1}},
        sampleId: UUID(),
        lsid,
        txnNumber
    };
    const expectedSampledQueryDocs = [{
        sampleId: originalCmdObj.sampleId,
        cmdName: "findAndModify",
        cmdObj: QuerySamplingUtil.makeCmdObjIgnoreSessionInfo(originalCmdObj)
    }];

    const fp = configureFailPoint(primary, "failAllFindAndModify");

    // The findAndModify fails after it has been added to the sample buffer.
    assert.commandFailedWithCode(db.runCommand(originalCmdObj), ErrorCodes.InternalError);

    QuerySamplingUtil.assertSoonSampledQueryDocuments(
        primary, ns, collectionUuid, expectedSampledQueryDocs);

    fp.off();

    // Retry with the same sampleId.
    const retryCmdObj = originalCmdObj;
    const retryRes = assert.commandWorked(db.runCommand(retryCmdObj));
    assert.eq(retryRes.lastErrorObject.n, 1, retryRes);

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

testRetryExecutedWrite(st.rs0);
testRetryUnExecutedWrite(st.rs0);

st.stop();
})();
