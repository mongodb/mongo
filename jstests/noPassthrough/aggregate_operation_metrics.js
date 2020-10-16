/**
 * Tests command output from the $operationMetrics aggregation stage.
 * @tags: [
 *   requires_replication
 * ]
 */
(function() {
'use strict';

var rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            "measureOperationResourceConsumption": true,
            "aggregateOperationResourceConsumptionMetrics": true
        }
    }
});
rst.startSet();
rst.initiate();

let assertMetricsExist = function(metrics) {
    assert.neq(metrics, undefined);
    let primaryMetrics = metrics.primaryMetrics;
    let secondaryMetrics = metrics.secondaryMetrics;
    [primaryMetrics, secondaryMetrics].forEach((readMetrics) => {
        assert.gte(readMetrics.docBytesRead, 0);
        assert.gte(readMetrics.docUnitsRead, 0);
        assert.gte(readMetrics.idxEntriesRead, 0);
        assert.gte(readMetrics.keysSorted, 0);
    });

    assert.gte(metrics.cpuMillis, 0);
    assert.gte(metrics.docBytesWritten, 0);
    assert.gte(metrics.docUnitsWritten, 0);
    assert.gte(metrics.docUnitsReturned, 0);
};

let getDBMetrics = (adminDB) => {
    let cursor = adminDB.aggregate([{$operationMetrics: {}}]);

    // Merge all returned documents into a single object keyed by database name.
    let allMetrics = {};
    while (cursor.hasNext()) {
        let doc = cursor.next();
        allMetrics[doc.db] = doc;
    }

    return allMetrics;
};

// Perform very basic reads and writes on two different databases.
const db1Name = 'db1';
const primary = rst.getPrimary();
const db1 = primary.getDB(db1Name);
assert.commandWorked(db1.coll1.insert({a: 1}));
assert.commandWorked(db1.coll2.insert({a: 1}));

const db2Name = 'db2';
const db2 = primary.getDB(db2Name);
assert.commandWorked(db2.coll1.insert({a: 1}));
assert.commandWorked(db2.coll2.insert({a: 1}));

const secondary = rst.getSecondary();
[primary, secondary].forEach(function(node) {
    jsTestLog("Testing node: " + node);

    // Clear metrics after waiting for replication to ensure we are not observing metrics from
    // a previous loop iteration.
    rst.awaitReplication();
    const adminDB = node.getDB('admin');
    adminDB.aggregate([{$operationMetrics: {clearMetrics: true}}]);

    assert.eq(node.getDB(db1Name).coll1.find({a: 1}).itcount(), 1);
    assert.eq(node.getDB(db1Name).coll2.find({a: 1}).itcount(), 1);
    assert.eq(node.getDB(db2Name).coll1.find({a: 1}).itcount(), 1);
    assert.eq(node.getDB(db2Name).coll2.find({a: 1}).itcount(), 1);

    // Run an aggregation with a batch size of 1.
    let cursor = adminDB.aggregate([{$operationMetrics: {}}], {cursor: {batchSize: 1}});
    assert(cursor.hasNext());

    // Merge all returned documents into a single object keyed by database name.
    let allMetrics = {};
    let doc = cursor.next();
    allMetrics[doc.db] = doc;
    assert.eq(cursor.objsLeftInBatch(), 0);

    // Trigger a getMore to retrieve metrics for the other database.
    assert(cursor.hasNext());
    doc = cursor.next();
    allMetrics[doc.db] = doc;
    assert(!cursor.hasNext());

    // Ensure the two user database have present metrics.
    assertMetricsExist(allMetrics[db1Name]);
    assertMetricsExist(allMetrics[db2Name]);

    // Ensure read metrics are attributed to the correct replication state.
    let lastDocBytesRead;
    if (node === primary) {
        [db1Name, db2Name].forEach((db) => {
            assert.gt(allMetrics[db].primaryMetrics.docBytesRead, 0);
            assert.gt(allMetrics[db].primaryMetrics.docUnitsRead, 0);
            assert.eq(allMetrics[db].secondaryMetrics.docBytesRead, 0);
            assert.eq(allMetrics[db].secondaryMetrics.docUnitsRead, 0);
        });
        assert.eq(allMetrics[db1Name].primaryMetrics.docBytesRead,
                  allMetrics[db2Name].primaryMetrics.docBytesRead);
        lastDocBytesRead = allMetrics[db1Name].primaryMetrics.docBytesRead;
    } else {
        [db1Name, db2Name].forEach((db) => {
            assert.gt(allMetrics[db].secondaryMetrics.docBytesRead, 0);
            assert.gt(allMetrics[db].secondaryMetrics.docUnitsRead, 0);
            assert.eq(allMetrics[db].primaryMetrics.docBytesRead, 0);
            assert.eq(allMetrics[db].primaryMetrics.docUnitsRead, 0);
        });
        assert.eq(allMetrics[db1Name].secondaryMetrics.docBytesRead,
                  allMetrics[db2Name].secondaryMetrics.docBytesRead);
        lastDocBytesRead = allMetrics[db1Name].secondaryMetrics.docBytesRead;
    }

    // Metrics for these databases should not be collected or reported.
    assert.eq(allMetrics['admin'], undefined);
    assert.eq(allMetrics['local'], undefined);
    assert.eq(allMetrics['config'], undefined);

    // Ensure this stage can be composed with other pipeline stages.
    const newDbName = "newDB";
    const newCollName = "metrics_out";
    cursor = adminDB.aggregate([
        {$operationMetrics: {}},
        {$project: {db: 1}},
        {$out: {db: newDbName, coll: newCollName}},
    ]);

    // No results from the aggregation because of the $out.
    assert.eq(cursor.itcount(), 0);

    // TODO (SERVER-51176): Ensure metrics are properly recorded for $out.
    // This new database should appear with metrics, but it does not.
    cursor = adminDB.aggregate([{$operationMetrics: {}}]);
    assert.eq(cursor.itcount(), 2);

    // Metrics should not have changed.
    allMetrics = getDBMetrics(adminDB);
    if (node === primary) {
        assert.eq(allMetrics[db1Name].primaryMetrics.docBytesRead, lastDocBytesRead);
        assert.eq(allMetrics[db2Name].primaryMetrics.docBytesRead, lastDocBytesRead);
    } else {
        assert.eq(allMetrics[db1Name].secondaryMetrics.docBytesRead, lastDocBytesRead);
        assert.eq(allMetrics[db2Name].secondaryMetrics.docBytesRead, lastDocBytesRead);
    }

    // Ensure the output collection has the 2 databases that existed at the start of the operation.
    rst.awaitReplication();
    cursor = node.getDB(newDbName)[newCollName].find({});
    assert.eq(cursor.itcount(), 2);

    primary.getDB(newDbName).dropDatabase();

    // Fetch and don't clear metrics.
    cursor = adminDB.aggregate([{$operationMetrics: {clearMetrics: false}}]);
    assert.eq(cursor.itcount(), 3);

    // Fetch and clear metrics.
    cursor = adminDB.aggregate([{$operationMetrics: {clearMetrics: true}}]);
    assert.eq(cursor.itcount(), 3);

    // Ensure no metrics are reported.
    cursor = adminDB.aggregate([{$operationMetrics: {}}]);
    assert.eq(cursor.itcount(), 0);

    // Insert something and ensure metrics are still reporting.
    assert.commandWorked(db1.coll3.insert({a: 1}));
    rst.awaitReplication();

    // On the primary, this insert's metrics should be recorded, but not on the secondary. Since it
    // is applied by the batch applier on the secondary, it is not a user operation and should not
    // count toward any metrics.
    cursor = adminDB.aggregate([{$operationMetrics: {}}]);
    if (node === primary) {
        assert.eq(cursor.itcount(), 1);
    } else {
        assert.eq(cursor.itcount(), 0);
    }
    db1.coll3.drop();
});

rst.stopSet();
}());