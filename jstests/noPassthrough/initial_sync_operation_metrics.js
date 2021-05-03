/**
 * Test that initial sync does not contribute to operation metrics on the sync source when cloning
 * data.
 *
 * @tags: [
 *   requires_replication,
 *   requites_wiredtiger,
 * ]
 */

(function() {
"use strict";

const assertMetricsExist = function(metrics) {
    try {
        assert.neq(metrics, undefined);
        let primaryMetrics = metrics.primaryMetrics;
        let secondaryMetrics = metrics.secondaryMetrics;
        [primaryMetrics, secondaryMetrics].forEach((readMetrics) => {
            assert.gte(readMetrics.docBytesRead, 0);
            assert.gte(readMetrics.docUnitsRead, 0);
            assert.gte(readMetrics.idxEntryBytesRead, 0);
            assert.gte(readMetrics.idxEntryUnitsRead, 0);
            assert.gte(readMetrics.keysSorted, 0);
            assert.gte(readMetrics.docUnitsReturned, 0);
            assert.gte(readMetrics.cursorSeeks, 0);
        });

        assert.gte(metrics.cpuNanos, 0);
        assert.gte(metrics.docBytesWritten, 0);
        assert.gte(metrics.docUnitsWritten, 0);
        assert.gte(metrics.idxEntryBytesWritten, 0);
        assert.gte(metrics.idxEntryUnitsWritten, 0);
    } catch (e) {
        print("caught exception while checking metrics output: " + tojson(metrics));
        throw e;
    }
};

// Returns metrics keyed database name.
const getDBMetrics = (adminDB) => {
    const cursor = adminDB.aggregate([{$operationMetrics: {}}]);
    let allMetrics = {};
    while (cursor.hasNext()) {
        let doc = cursor.next();
        // Remove localTime field as it stymies us from comparing objects since it always changes.
        delete doc.localTime;
        allMetrics[doc.db] = doc;
    }
    return allMetrics;
};

const setParams = {
    "aggregateOperationResourceConsumptionMetrics": true,
};

const replSet = new ReplSetTest({nodes: 1, nodeOptions: {setParameter: setParams}});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const adminDB = primary.getDB('admin');

const db1Name = "db1";
const db1 = primary.getDB(db1Name);
assert.commandWorked(db1.coll1.insert({a: 1}));
assert.commandWorked(db1.coll2.insert({a: 1}));

const db2Name = 'db2';
const db2 = primary.getDB(db2Name);
assert.commandWorked(db2.coll1.insert({a: 1}));
assert.commandWorked(db2.coll2.insert({a: 1}));

const metricsBefore = getDBMetrics(adminDB);
assertMetricsExist(metricsBefore[db1Name]);
assertMetricsExist(metricsBefore[db2Name]);

const newNode = replSet.add({setParameter: setParams});
replSet.reInitiate();
replSet.waitForState(newNode, ReplSetTest.State.SECONDARY);
replSet.awaitReplication();

// Ensure that the initial syncing node has not collected any metrics.
{
    const metrics = getDBMetrics(newNode.getDB('admin'));
    assert(!metrics.hasOwnProperty(db1Name), metrics);
    assert(!metrics.hasOwnProperty(db2Name), metrics);
}

// Ensure the initial syncing node did not accumulate metrics on the primary by reading.
{
    const metrics = getDBMetrics(adminDB);
    assert.eq(metricsBefore[db1Name], metrics[db1Name]);
    assert.eq(metricsBefore[db2Name], metrics[db2Name]);
}

replSet.stopSet();
})();
