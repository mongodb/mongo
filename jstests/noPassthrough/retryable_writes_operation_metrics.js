/**
 * Tests resource consumption metrics for retryable writes.
 * Retryable writes persist transaction documents in "config.transaction" and all reads and writes
 * to "config.transaction" should not trigger any operation metrics.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/retryable_writes_util.js");

function setupReplicaSet() {
    var rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {setParameter: {"aggregateOperationResourceConsumptionMetrics": true}}
    });

    rst.startSet();
    rst.initiate();
    return rst;
}

function clearMetrics(conn) {
    conn.getDB('admin').aggregate([{$operationMetrics: {clearMetrics: true}}]);
}

function getMetrics(conn) {
    const cursor = conn.getDB('admin').aggregate([{$operationMetrics: {}}]);

    let allMetrics = {};
    while (cursor.hasNext()) {
        let doc = cursor.next();
        allMetrics[doc.db] = doc;
    }
    return allMetrics;
}

function assertMetricsZeroRead(metrics, dbName) {
    assert(metrics[dbName]);
    assert.eq(metrics[dbName].primaryMetrics.docBytesRead, 0);
    assert.eq(metrics[dbName].primaryMetrics.docUnitsRead, 0);
    assert.eq(metrics[dbName].primaryMetrics.idxEntryBytesRead, 0);
    assert.eq(metrics[dbName].primaryMetrics.idxEntryUnitsRead, 0);
}

function assertMetricsWritten(metrics, dbName, expectedWritten) {
    assert(metrics[dbName]);
    assert.eq(metrics[dbName].docBytesWritten, expectedWritten.docBytesWritten);
    assert.eq(metrics[dbName].docUnitsWritten, expectedWritten.docUnitsWritten);
    assert.eq(metrics[dbName].idxEntryBytesWritten, expectedWritten.idxEntryBytesWritten);
    assert.eq(metrics[dbName].idxEntryUnitsWritten, expectedWritten.idxEntryUnitsWritten);
    assert.eq(metrics[dbName].totalUnitsWritten, expectedWritten.totalUnitsWritten);
}

function runRetryableTransaction(sessionDb, txnNumber, cmdObj) {
    let cmd = Object.assign(
        {}, cmdObj, {txnNumber: NumberLong(txnNumber), startTransaction: true, autocommit: false});

    assert.commandWorked(sessionDb.runCommand(cmd));
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"}
    }));
}

function runRetryableWriteCmd(sessionDb, txnNumber, cmdObj) {
    let cmd = Object.assign({}, cmdObj, {txnNumber: NumberLong(txnNumber)});
    jsTest.log("run retryable write with command :" + tojson(cmd));
    assert.commandWorked(sessionDb.runCommand(cmd));
}

const rst = setupReplicaSet();
const primary = rst.getPrimary();
const kDbName = "testDb";
const kCollName = "testColl";
const testDb = primary.getDB(kDbName);

assert.commandWorked(testDb.createCollection(kCollName));

const session = testDb.getMongo().startSession();
let sessionDB = session.getDatabase(kDbName);
let txnNumber = 0;

let makeInsertCmdObj = (docs) => {
    return {insert: kCollName, documents: docs, ordered: false};
};

let makeDocs = (fromId, toId) => {
    let docs = [];
    for (let i = fromId; i <= toId; i++) {
        docs.push({_id: i, number: i});
    }
    return docs;
};

let expectedWritten;

jsTest.log("Tests non-retryable insert comamnd which has no transaction number.");
{
    clearMetrics(primary);
    let cmdObj = makeInsertCmdObj(makeDocs(1, 3));
    assert.commandWorked(testDb.runCommand(cmdObj));
    let metrics = getMetrics(primary);

    assertMetricsZeroRead(metrics, kDbName);

    // Init the expected doc, index and total data written. The following retryable writes
    // should have the same output as the size of inserted user data is same.
    expectedWritten = {
        docBytesWritten: metrics[kDbName].docBytesWritten,
        docUnitsWritten: metrics[kDbName].docUnitsWritten,
        idxEntryBytesWritten: metrics[kDbName].idxEntryBytesWritten,
        idxEntryUnitsWritten: metrics[kDbName].idxEntryUnitsWritten,
        totalUnitsWritten: metrics[kDbName].totalUnitsWritten
    };
}

jsTest.log("Tests retryable commitTransaction command which inserts documents.");
{
    clearMetrics(primary);
    let cmdObj = makeInsertCmdObj(makeDocs(4, 6));
    runRetryableTransaction(sessionDB, txnNumber, cmdObj);
    let metrics = getMetrics(primary);
    assertMetricsZeroRead(metrics, kDbName);
    assertMetricsWritten(metrics, kDbName, expectedWritten);
}

txnNumber++;
jsTest.log("Tests retryable insert command.");
{
    clearMetrics(primary);
    let cmdObj = makeInsertCmdObj(makeDocs(7, 9));
    runRetryableWriteCmd(sessionDB, txnNumber, cmdObj);
    let metrics = getMetrics(primary);
    assertMetricsZeroRead(metrics, kDbName);
    assertMetricsWritten(metrics, kDbName, expectedWritten);
}

session.endSession();
rst.stopSet();
}());
