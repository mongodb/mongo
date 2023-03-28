/**
 * Tests resource consumption metrics for TTL indexes on multiple databases.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');  // For IndexBuildTest
load("jstests/libs/fail_point_util.js");
load("jstests/libs/ttl_util.js");

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

const dbName1 = 'db1';
const dbName2 = 'db2';
const collName = 'coll';
const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const primaryDB1 = primary.getDB(dbName1);
const primaryDB2 = primary.getDB(dbName2);

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

// Create identical TTL indexes on both databases with immediate expiry.
assert.commandWorked(primaryDB1[collName].createIndex({x: 1}, {expireAfterSeconds: 0}));
assert.commandWorked(primaryDB2[collName].createIndex({x: 1}, {expireAfterSeconds: 0}));

const pauseTtl = configureFailPoint(primary, 'hangTTLMonitorWithLock');
pauseTtl.wait();

clearMetrics(primary);

// On DB 1 we expect all documents to be deleted.
const now = new Date();
assert.commandWorked(primaryDB1[collName].insert({_id: 0, x: now}));
assert.commandWorked(primaryDB1[collName].insert({_id: 1, x: now}));

// On DB2 we expect no documents to be deleted.
const later = new Date(now.getTime() + 1000 * 60 * 60);
assert.commandWorked(primaryDB2[collName].insert({_id: 0, x: later}));
assert.commandWorked(primaryDB2[collName].insert({_id: 1, x: later}));

assertMetrics(primary, (metrics) => {
    // With replication enabled, oplog writes are counted towards bytes written. Only assert that we
    // insert at least as many bytes in the documents.
    // Document size is 29 bytes.
    assert.gte(metrics[dbName1].docBytesWritten, 29 * 2);
    assert.gte(metrics[dbName1].docUnitsWritten, 2);
    assert.gte(metrics[dbName1].totalUnitsWritten, 2);

    assert.gte(metrics[dbName2].docBytesWritten, 29 * 2);
    assert.gte(metrics[dbName2].docUnitsWritten, 2);
    assert.gte(metrics[dbName2].totalUnitsWritten, 2);
});

// Clear metrics and wait for a TTL pass to delete the documents.
clearMetrics(primary);
pauseTtl.off();
TTLUtil.waitForPass(primaryDB1);

// Ensure that the TTL monitor deleted 2 documents on the primary and recorded read and write
// metrics.
assertMetrics(primary, (metrics) => {
    // The TTL monitor generates oplog entries for each deletion on the primary. Assert that we
    // write at least as many bytes in the documents. Document size is 29 bytes.
    assert.gte(metrics[dbName1].primaryMetrics.docBytesRead, 29 * 2);
    assert.gte(metrics[dbName1].primaryMetrics.docUnitsRead, 2);
    assert.gte(metrics[dbName1].docBytesWritten, 29 * 2);
    assert.gte(metrics[dbName1].docUnitsWritten, 2);
    assert.gte(metrics[dbName1].totalUnitsWritten, 2);
    // Key size is 12 bytes.
    assert.gte(metrics[dbName1].primaryMetrics.idxEntryBytesRead, 12 * 2);
    assert.gte(metrics[dbName1].primaryMetrics.idxEntryUnitsRead, 2);
    // At least 2 keys (_id and x) should be deleted for each document.
    assert.gte(metrics[dbName1].idxEntryUnitsWritten, 2 * 2);
    assert.gte(metrics[dbName1].idxEntryBytesWritten, 12 * 2);

    assert.eq(metrics[dbName2].primaryMetrics.docBytesRead, 0);
    assert.eq(metrics[dbName2].primaryMetrics.docUnitsRead, 0);
    assert.eq(metrics[dbName2].docBytesWritten, 0);
    assert.eq(metrics[dbName2].docUnitsWritten, 0);
    assert.eq(metrics[dbName2].totalUnitsWritten, 0);

    // We need to read in a few keys to determine whether there is data to delete. Since we haven't
    // stopped the TTL monitor, the value can be larger than expected.
    assert.gte(metrics[dbName2].primaryMetrics.idxEntryBytesRead, 24);
    assert.gte(metrics[dbName2].primaryMetrics.idxEntryUnitsRead, 2);
    assert.eq(metrics[dbName2].idxEntryUnitsWritten, 0);
    assert.eq(metrics[dbName2].idxEntryBytesWritten, 0);
});

rst.awaitReplication();

// There should be no activity on the secondary.
assertMetrics(secondary, (metrics) => {
    assert(!metrics.hasOwnProperty(dbName1));
    assert(!metrics.hasOwnProperty(dbName2));
});

// Ensure the correct documents were deleted.
assert.eq(primaryDB1[collName].count({}), 0);
assert.eq(primaryDB2[collName].count({}), 2);

const secondaryDB1 = secondary.getDB(dbName1);
const secondaryDB2 = secondary.getDB(dbName2);
assert.eq(secondaryDB1[collName].count({}), 0);
assert.eq(secondaryDB2[collName].count({}), 2);

rst.stopSet();
}());
