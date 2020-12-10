/**
 * Tests command output from the $operationMetrics aggregation stage.
 * @tags: [
 *   requires_replication,
 *   requires_wiredtiger,
 *   sbe_incompatible,
 * ]
 */
(function() {
'use strict';

var rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {setParameter: {"aggregateOperationResourceConsumptionMetrics": true}}
});
rst.startSet();
rst.initiate();

const isLinux = getBuildInfo().buildEnvironment.target_os == "linux";

let assertMetricsExist = function(metrics) {
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
            assert.gte(readMetrics.sorterSpills, 0);
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

let getServerStatusMetrics = (db) => {
    let ss = db.serverStatus();
    assert(ss.hasOwnProperty('resourceConsumption'), ss);
    return ss.resourceConsumption;
};

const primary = rst.getPrimary();

// $operationMetrics may only be run against the admin database and in a 'collectionless' form.
assert.commandFailedWithCode(primary.getDB('invalid').runCommand({
    aggregate: 1,
    pipeline: [{$operationMetrics: {}}],
    cursor: {},
}),
                             ErrorCodes.InvalidNamespace);
assert.commandFailedWithCode(primary.getDB('admin').runCommand({
    aggregate: 'test',
    pipeline: [{$operationMetrics: {}}],
    cursor: {},
}),
                             ErrorCodes.InvalidNamespace);

// Perform very basic reads and writes on two different databases.
const db1Name = 'db1';
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

    let initialCpuTime = getServerStatusMetrics(adminDB).cpuNanos;

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

    let ssMetrics = getServerStatusMetrics(adminDB);
    assert.eq(ssMetrics.numMetrics, 2);
    assert.gt(ssMetrics.memUsage, 0);

    // Ensure read metrics are attributed to the correct replication state.
    let lastDocBytesRead;
    if (node === primary) {
        [db1Name, db2Name].forEach((db) => {
            assert.gt(allMetrics[db].primaryMetrics.docBytesRead, 0);
            assert.gt(allMetrics[db].primaryMetrics.docUnitsRead, 0);
            assert.eq(allMetrics[db].primaryMetrics.cursorSeeks, 0);
            assert.eq(allMetrics[db].secondaryMetrics.docBytesRead, 0);
            assert.eq(allMetrics[db].secondaryMetrics.docUnitsRead, 0);
            assert.eq(allMetrics[db].secondaryMetrics.cursorSeeks, 0);
        });
        assert.eq(allMetrics[db1Name].primaryMetrics.docBytesRead,
                  allMetrics[db2Name].primaryMetrics.docBytesRead);
        lastDocBytesRead = allMetrics[db1Name].primaryMetrics.docBytesRead;
    } else {
        [db1Name, db2Name].forEach((db) => {
            assert.gt(allMetrics[db].secondaryMetrics.docBytesRead, 0);
            assert.gt(allMetrics[db].secondaryMetrics.docUnitsRead, 0);
            assert.eq(allMetrics[db].secondaryMetrics.cursorSeeks, 0);
            assert.eq(allMetrics[db].primaryMetrics.docBytesRead, 0);
            assert.eq(allMetrics[db].primaryMetrics.docUnitsRead, 0);
            assert.eq(allMetrics[db].primaryMetrics.cursorSeeks, 0);
        });
        assert.eq(allMetrics[db1Name].secondaryMetrics.docBytesRead,
                  allMetrics[db2Name].secondaryMetrics.docBytesRead);
        lastDocBytesRead = allMetrics[db1Name].secondaryMetrics.docBytesRead;
    }

    // CPU time aggregation is only supported on Linux.
    if (isLinux) {
        // Ensure the CPU time is increasing.
        let lastCpuTime = getServerStatusMetrics(adminDB).cpuNanos;
        assert.gt(lastCpuTime, initialCpuTime);

        // Ensure the global CPU time matches the aggregated time for both databases.
        assert.eq(lastCpuTime - initialCpuTime,
                  allMetrics[db1Name].cpuNanos + allMetrics[db2Name].cpuNanos);
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

    // There are no additional metrics for the new database because the command was run on the
    // 'admin' database and it does not collect metrics.
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

    // Ensure the serverStatus metrics are cleared except for cpuNanos.
    ssMetrics = getServerStatusMetrics(adminDB);
    assert.eq(0, ssMetrics.numMetrics);
    assert.eq(0, ssMetrics.memUsage);
    if (isLinux) {
        assert.neq(0, ssMetrics.cpuNanos);
    } else {
        assert.eq(0, ssMetrics.cpuNanos);
    }

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