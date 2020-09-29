/**
 * Tests that resource consumption metrics are reported in the profiler.
 *
 *  @tags: [
 *    does_not_support_stepdowns,
 *    requires_fcv_47,
 *    requires_getmore,
 *    requires_non_retryable_writes,
 *    requires_profiling,
 *    sbe_incompatible,
 *  ]
 */
(function() {
"use strict";

let res = assert.commandWorked(
    db.adminCommand({getParameter: 1, measureOperationResourceConsumption: 1}));
if (!res.measureOperationResourceConsumption) {
    jsTestLog("Skipping test because the 'measureOperationResourceConsumption' flag is disabled");
    return;
}

const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);
const collName = 'coll';

testDB.dropDatabase();

testDB.setProfilingLevel(2);

let assertMetricsExist = (profilerEntry) => {
    let metrics = profilerEntry.operationMetrics;
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

const resetProfileColl = {
    name: 'resetProfileColl',
    command: (db) => {
        db.setProfilingLevel(0);
        assert(db.system.profile.drop());
        db.setProfilingLevel(2);
    },
};
const operations = [
    {
        name: 'create',
        command: (db) => {
            assert.commandWorked(db.createCollection(collName));
        },
        profileFilter: {op: 'command', 'command.create': collName}
    },
    {
        name: 'createIndex',
        command: (db) => {
            assert.commandWorked(db[collName].createIndex({a: 1}));
        },
        profileFilter: {op: 'command', 'command.createIndexes': collName}
    },
    {
        name: 'insert',
        command: (db) => {
            assert.commandWorked(db[collName].insert({_id: 1}));
        },
        profileFilter: {op: 'insert', 'command.insert': collName}
    },
    {
        name: 'find',
        command: (db) => {
            assert.eq(db[collName].find({_id: 1}).itcount(), 1);
        },
        profileFilter: {op: 'query', 'command.find': collName}
    },
    {
        name: 'aggregate',
        command: (db) => {
            assert.eq(db[collName].aggregate([{$project: {_id: 1}}]).itcount(), 1);
        },
        profileFilter: {op: 'command', 'command.aggregate': collName}
    },
    {
        name: 'distinct',
        command: (db) => {
            assert.eq(db[collName].distinct("_id").length, 1);
        },
        profileFilter: {op: 'command', 'command.distinct': collName}
    },
    {
        name: 'findAndModify',
        command: (db) => {
            assert(db[collName].findAndModify({query: {_id: 1}, update: {$set: {a: 1}}}));
        },
        profileFilter: {op: 'command', 'command.findandmodify': collName}
    },
    {
        name: 'update',
        command: (db) => {
            assert.commandWorked(db[collName].update({_id: 1}, {$set: {a: 2}}));
        },
        profileFilter: {op: 'update', 'command.q': {_id: 1}}
    },
    {
        name: 'count',
        command: (db) => {
            assert.eq(1, db[collName].count());
        },
        profileFilter: {op: 'command', 'command.count': collName}
    },
    {
        name: 'explain',
        command: (db) => {
            assert.commandWorked(db[collName].find().explain());
        },
        profileFilter: {op: 'command', 'command.explain.find': collName}
    },
    {
        name: 'listIndexes',
        command: (db) => {
            assert.eq(db[collName].getIndexes().length, 2);
        },
        profileFilter: {op: 'command', 'command.listIndexes': collName}
    },
    {
        name: 'dropIndex',
        command: (db) => {
            assert.commandWorked(db[collName].dropIndex({a: 1}));
        },
        profileFilter: {op: 'command', 'command.dropIndexes': collName}
    },
    // Clear the profile collection so we can easily identify new operations with similar filters as
    // past operations.
    resetProfileColl,
    {
        name: 'getMore',
        command: (db) => {
            db[collName].insert({_id: 2});
            let cursor = db[collName].find().batchSize(1);
            cursor.next();
            assert.eq(cursor.objsLeftInBatch(), 0);
            // Trigger a getMore
            cursor.next();
        },
        profileFilter: {op: 'getmore', 'command.collection': collName}
    },
    {
        name: 'delete',
        command: (db) => {
            assert.commandWorked(db[collName].remove({_id: 1}));
        },
        profileFilter: {op: 'remove', 'command.q': {_id: 1}}
    },
    {
        name: 'dropCollection',
        command: (db) => {
            assert(db[collName].drop());
        },
        profileFilter: {op: 'command', 'command.drop': collName}
    },
];

let profileColl = testDB.system.profile;
let testOperation = (operation) => {
    jsTestLog("Testing operation: " + operation.name);
    operation.command(testDB);
    if (!operation.profileFilter) {
        return;
    }

    let cursor = profileColl.find(operation.profileFilter);
    let entry = cursor.next();
    assert(!cursor.hasNext());

    assertMetricsExist(entry);
};

operations.forEach((op) => {
    testOperation(op);
});
})();