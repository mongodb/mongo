/**
 * Tests resource consumption metrics for TTL indexes.
 *
 * @tags: [
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');  // For IndexBuildTest
load("jstests/libs/fail_point_util.js");

var rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            "aggregateOperationResourceConsumptionMetrics": true,
            "ttlMonitorSleepSecs": 1,
        }
    }
});
rst.startSet();
rst.initiate();

const dbName = 'test';
const collName = 'test';
const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const primaryDB = primary.getDB(dbName);
const secondaryDB = secondary.getDB(dbName);

const clearMetrics = (conn) => {
    conn.getDB('admin').aggregate([{$operationMetrics: {clearMetrics: true}}]);
};

// Get aggregated metrics keyed by database name.
const getMetrics = (conn) => {
    const cursor = conn.getDB('admin').aggregate([{$operationMetrics: {}}]);

    let allMetrics = {};
    while (cursor.hasNext()) {
        let doc = cursor.next();
        allMetrics[doc.db] = doc;
    }
    return allMetrics;
};

const assertMetrics = (conn, assertFn) => {
    let metrics = getMetrics(conn);
    try {
        assertFn(metrics);
    } catch (e) {
        print("caught exception while checking metrics on " + tojson(conn) +
              ", metrics: " + tojson(metrics));
        throw e;
    }
};

const waitForTtlPass = (db) => {
    // Wait for the TTL monitor to run at least twice (in case we weren't finished setting up our
    // collection when it ran the first time).
    let ttlPass = db.serverStatus().metrics.ttl.passes;
    assert.soon(function() {
        return db.serverStatus().metrics.ttl.passes >= ttlPass + 2;
    }, "TTL monitor didn't run before timing out.");
};

// Create a TTL index and pause the thread.
assert.commandWorked(primaryDB[collName].createIndex({x: 1}, {expireAfterSeconds: 0}));

let pauseTtl = configureFailPoint(primary, 'hangTTLMonitorWithLock');
pauseTtl.wait();

clearMetrics(primary);

let now = new Date();
let later = new Date(now.getTime() + 1000 * 60 * 60);
assert.commandWorked(primaryDB[collName].insert({_id: 0, x: now}));
assert.commandWorked(primaryDB[collName].insert({_id: 1, x: now}));
assert.commandWorked(primaryDB[collName].insert({_id: 2, x: later}));

assertMetrics(primary, (metrics) => {
    // With replication enabled, oplog writes are counted towards bytes written. Only assert that we
    // insert at least as many bytes in the documents.
    // Document size is 29 bytes.
    assert.gte(metrics[dbName].docBytesWritten, 29 * 3);
    assert.gte(metrics[dbName].docUnitsWritten, 3);
    assert.gte(metrics[dbName].totalUnitsWritten, 3);
});

// Clear metrics and wait for a TTL pass to delete the documents.
clearMetrics(primary);
pauseTtl.off();
waitForTtlPass(primaryDB);

// Ensure that the TTL monitor deleted 2 documents on the primary and recorded read and write
// metrics.
assertMetrics(primary, (metrics) => {
    // The TTL monitor generates oplog entries for each deletion on the primary. Assert that we
    // write at least as many bytes in the documents. Document size is 29 bytes.
    assert.gte(metrics[dbName].primaryMetrics.docBytesRead, 29 * 2);
    assert.gte(metrics[dbName].primaryMetrics.docUnitsRead, 2);
    assert.gte(metrics[dbName].docBytesWritten, 29 * 2);
    assert.gte(metrics[dbName].docUnitsWritten, 2);
    assert.gte(metrics[dbName].totalUnitsWritten, 2);
    // Key size is 12 bytes.
    assert.gte(metrics[dbName].primaryMetrics.idxEntryBytesRead, 12 * 2);
    assert.gte(metrics[dbName].primaryMetrics.idxEntryUnitsRead, 2);
    // At least 2 keys (_id and x) should be deleted for each document.
    assert.gte(metrics[dbName].idxEntryUnitsWritten, 2 * 2);
    assert.gte(metrics[dbName].idxEntryBytesWritten, 12 * 2);
});

rst.awaitReplication();

// There should be no activity on the secondary.
assertMetrics(secondary, (metrics) => {
    assert(!metrics.hasOwnProperty(dbName));
});

// Ensure the last document was not deleted.
assert.eq(primaryDB[collName].count({}), 1);
assert.eq(secondaryDB[collName].count({}), 1);

rst.stopSet();
}());
