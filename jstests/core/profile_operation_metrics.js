/**
 * Tests that resource consumption metrics are reported in the profiler.
 *
 *  @tags: [
 *    does_not_support_stepdowns,
 *    requires_fcv_47,
 *    requires_getmore,
 *    requires_non_retryable_writes,
 *    requires_profiling,
 *    requires_wiredtiger,
 *    sbe_incompatible,
 *  ]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For isReplSet().

let res = assert.commandWorked(
    db.adminCommand({getParameter: 1, measureOperationResourceConsumption: 1}));
if (!res.measureOperationResourceConsumption.value) {
    jsTestLog("Skipping test because the 'measureOperationResourceConsumption' flag is disabled");
    return;
}

const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);
const collName = 'coll';
const debugBuild = db.adminCommand('buildInfo').debug;
const isReplSet = FixtureHelpers.isReplSet(db);

testDB.dropDatabase();

testDB.setProfilingLevel(2, 0);

const assertMetricsExist = (profilerEntry) => {
    let metrics = profilerEntry.operationMetrics;
    assert.neq(metrics, undefined);

    assert.gte(metrics.docBytesRead, 0);
    assert.gte(metrics.docUnitsRead, 0);
    assert.gte(metrics.idxEntryBytesRead, 0);
    assert.gte(metrics.idxEntryUnitsRead, 0);
    assert.gte(metrics.keysSorted, 0);
    assert.gte(metrics.docUnitsReturned, 0);

    assert.gte(metrics.cpuMillis, 0);
    assert.gte(metrics.docBytesWritten, 0);
    assert.gte(metrics.docUnitsWritten, 0);
    assert.gte(metrics.idxEntryBytesWritten, 0);
    assert.gte(metrics.idxEntryUnitsWritten, 0);
};

const runInLegacyQueryMode = (db, func) => {
    const readMode = db.getMongo().readMode();
    const writeMode = db.getMongo().writeMode();
    try {
        db.getMongo().forceReadMode("legacy");
        db.getMongo().forceWriteMode("legacy");
        func();
    } finally {
        db.getMongo().forceReadMode(readMode);
        db.getMongo().forceWriteMode(writeMode);
    }
};

const resetProfileColl = {
    name: 'resetProfileColl',
    command: (db) => {
        db.setProfilingLevel(0);
        assert(db.system.profile.drop());
        db.setProfilingLevel(2, 0);
    },
};

const resetTestColl = {
    name: 'resetTestColl',
    command: (db) => {
        assert(db[collName].drop());
        assert.commandWorked(db.createCollection(collName));
    },
};

const operations = [
    {
        name: 'create',
        command: (db) => {
            assert.commandWorked(db.createCollection(collName));
        },
        profileFilter: {op: 'command', 'command.create': collName},
        profileAssert: (profileDoc) => {
            // The size of the collection document in the _mdb_catalog may not be the same every
            // test run, so only assert this is non-zero.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.gt(profileDoc.docBytesWritten, 0);
            assert.gt(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    {
        name: 'createIndex',
        command: (db) => {
            assert.commandWorked(db[collName].createIndex({a: 1}));
        },
        profileFilter: {op: 'command', 'command.createIndexes': collName},
        profileAssert: (profileDoc) => {
            // The size of the collection document in the _mdb_catalog may not be the same every
            // test run, so only assert this is non-zero.
            // Index builds run on a separate thread and don't report their metrics with the
            // createIndex command, so we don't make any assertions.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.gt(profileDoc.docBytesWritten, 0);
            assert.gt(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    {
        name: 'insert',
        command: (db) => {
            assert.commandWorked(db[collName].insert({_id: 1, a: 0}));
        },
        profileFilter: {op: 'insert', 'command.insert': collName},
        profileAssert: (profileDoc) => {
            // Insert should not perform any reads.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            if (isReplSet) {
                // Ensure writes to the oplog are counted. Some oplog fields like UUID are
                // randomized between runs, but the types are fixed-length, so we can make strong
                // assertions about the sizes. If the oplog format changes for any reason, then so
                // will these assertions.
                assert.eq(profileDoc.docBytesWritten, 188);
                // Note that the oplog entry is greater than the default unit size of 128 bytes so
                // it counts for 2 document units.
                assert.eq(profileDoc.docUnitsWritten, 3);
            } else {
                assert.eq(profileDoc.docBytesWritten, 29);
                assert.eq(profileDoc.docUnitsWritten, 1);
            }
            assert.eq(profileDoc.idxEntryBytesWritten, 7);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
        }
    },
    {
        name: 'findIxScanAndFetch',
        command: (db) => {
            assert.eq(db[collName].find({_id: 1}).itcount(), 1);

            // Spot check that find is reporting operationMetrics in the slow query logs, as should
            // all operations.
            checkLog.containsJson(db.getMongo(), 51803, {
                'command': (obj) => {
                    return obj.find == collName;
                },
                'operationMetrics': (obj) => {
                    return obj.docBytesRead == 29;
                },
            });
        },
        profileFilter: {op: 'query', 'command.find': collName, 'command.filter': {_id: 1}},
        profileAssert: (profileDoc) => {
            // Should read exactly as many bytes are in the document.
            assert.eq(profileDoc.docBytesRead, 29);
            assert.eq(profileDoc.docUnitsRead, 1);
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    {
        name: 'findCollScan',
        command: (db) => {
            assert.eq(db[collName].find().itcount(), 1);
        },
        profileFilter: {op: 'query', 'command.find': collName, 'command.filter': {}},
        profileAssert: (profileDoc) => {
            // Should read exactly as many bytes are in the document.
            assert.eq(profileDoc.docBytesRead, 29);
            assert.eq(profileDoc.docUnitsRead, 1);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    {
        name: 'aggregate',
        command: (db) => {
            assert.eq(db[collName].aggregate([{$project: {_id: 1}}]).itcount(), 1);
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (profileDoc) => {
            // Should read exactly as many bytes are in the document.
            assert.eq(profileDoc.docBytesRead, 29);
            assert.eq(profileDoc.docUnitsRead, 1);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    {
        name: 'distinct',
        command: (db) => {
            assert.eq(db[collName].distinct("_id").length, 1);
        },
        profileFilter: {op: 'command', 'command.distinct': collName},
        profileAssert: (profileDoc) => {
            // Does not read from the collection.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    {
        name: 'findAndModify',
        command: (db) => {
            assert(db[collName].findAndModify({query: {_id: 1}, update: {$set: {a: 1}}}));
        },
        profileFilter: {op: 'command', 'command.findandmodify': collName},
        profileAssert: (profileDoc) => {
            // Should read exactly as many bytes are in the document. Debug builds may perform extra
            // reads of the _mdb_catalog.
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            if (isReplSet) {
                // Ensure writes to the oplog are counted.
                assert.eq(profileDoc.docBytesWritten, 224);
                assert.eq(profileDoc.docUnitsWritten, 3);
            } else {
                // This update will not be performed in-place because it is too small and affects an
                // index.
                assert.eq(profileDoc.docBytesWritten, 29);
                assert.eq(profileDoc.docUnitsWritten, 1);
            }
            // Deletes one index entry and writes another.
            assert.eq(profileDoc.idxEntryBytesWritten, 9);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
        }
    },
    {
        name: 'update',
        command: (db) => {
            assert.commandWorked(db[collName].update({_id: 1}, {$set: {a: 2}}));
        },
        profileFilter: {op: 'update', 'command.q': {_id: 1}},
        profileAssert: (profileDoc) => {
            // Should read exactly as many bytes are in the document. Debug builds may perform extra
            // reads of the _mdb_catalog.
            if (!debugBuild) {
                assert.eq(profileDoc.docUnitsRead, 1);
                assert.eq(profileDoc.docBytesRead, 29);
            } else {
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.docBytesRead, 29);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            if (isReplSet) {
                // Ensure writes to the oplog are counted.
                assert.eq(profileDoc.docBytesWritten, 224);
                assert.eq(profileDoc.docUnitsWritten, 3);
            } else {
                // This update will not be performed in-place because it is too small and affects an
                // index.
                assert.eq(profileDoc.docBytesWritten, 29);
                assert.eq(profileDoc.docUnitsWritten, 1);
            }
            // Deletes one index entry and writes another.
            assert.eq(profileDoc.idxEntryBytesWritten, 10);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
        }
    },
    {
        name: 'count',
        command: (db) => {
            assert.eq(1, db[collName].count());
        },
        profileFilter: {op: 'command', 'command.count': collName},
        profileAssert: (profileDoc) => {
            // Reads from the fast-count, not the collection.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    {
        name: 'explain',
        command: (db) => {
            assert.commandWorked(db[collName].find().explain());
        },
        profileFilter: {op: 'command', 'command.explain.find': collName},
        profileAssert: (profileDoc) => {
            // Should not read from the collection.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    // Clear the profile collection so we can easily identify new operations with similar filters as
    // past operations.
    resetProfileColl,
    {
        name: 'explainWithRead',
        command: (db) => {
            assert.commandWorked(db[collName].find().explain('allPlansExecution'));
        },
        profileFilter: {op: 'command', 'command.explain.find': collName},
        profileAssert: (profileDoc) => {
            // Should read from the collection.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    {
        name: 'listIndexes',
        command: (db) => {
            assert.eq(db[collName].getIndexes().length, 2);
        },
        profileFilter: {op: 'command', 'command.listIndexes': collName},
        profileAssert: (profileDoc) => {
            // This reads from the collection catalog.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    {
        name: 'dropIndex',
        command: (db) => {
            assert.commandWorked(db[collName].dropIndex({a: 1}));
        },
        profileFilter: {op: 'command', 'command.dropIndexes': collName},
        profileAssert: (profileDoc) => {
            // This reads from the collection catalog.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.gt(profileDoc.docBytesWritten, 0);
            assert.gt(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    resetProfileColl,
    {
        name: 'getMore',
        command: (db) => {
            db[collName].insert({_id: 2, a: 2});
            let cursor = db[collName].find().batchSize(1);
            cursor.next();
            assert.eq(cursor.objsLeftInBatch(), 0);
            // Trigger a getMore
            cursor.next();
        },
        profileFilter: {op: 'getmore', 'command.collection': collName},
        profileAssert: (profileDoc) => {
            // Debug builds may perform extra reads of the _mdb_catalog.
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    {
        name: 'deleteIxScan',
        command: (db) => {
            assert.commandWorked(db[collName].remove({_id: 1}));
        },
        profileFilter: {op: 'remove', 'command.q': {_id: 1}},
        profileAssert: (profileDoc) => {
            // Due to a deficiency in the delete path, we read the same document twice.
            // TODO (SERVER-51420): Deletes should not read documents twice before deleting.
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 58);
                assert.eq(profileDoc.docUnitsRead, 2);
            } else {
                assert.gte(profileDoc.docBytesRead, 58);
                assert.gte(profileDoc.docUnitsRead, 2);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            if (isReplSet) {
                // Ensure writes to the oplog are counted.
                assert.eq(profileDoc.docBytesWritten, 177);
                assert.eq(profileDoc.docUnitsWritten, 3);
            } else {
                // Deleted bytes are counted as 'written'.
                assert.eq(profileDoc.docBytesWritten, 29);
                assert.eq(profileDoc.docUnitsWritten, 1);
            }
            assert.eq(profileDoc.idxEntryBytesWritten, 3);
            assert.eq(profileDoc.idxEntryUnitsWritten, 1);
        }
    },
    {
        name: 'deleteCollScan',
        command: (db) => {
            assert.commandWorked(db[collName].remove({}));
        },
        profileFilter: {op: 'remove', 'command.q': {}},
        profileAssert: (profileDoc) => {
            // Due to a deficiency in the delete path, we read the same document twice.
            // TODO: SERVER-51420
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 58);
                assert.eq(profileDoc.docUnitsRead, 2);
            } else {
                assert.gte(profileDoc.docBytesRead, 58);
                assert.gte(profileDoc.docUnitsRead, 2);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            if (isReplSet) {
                // Ensure writes to the oplog are counted.
                assert.eq(profileDoc.docBytesWritten, 177);
                assert.eq(profileDoc.docUnitsWritten, 3);
            } else {
                // Deleted bytes are counted as 'written'.
                assert.eq(profileDoc.docBytesWritten, 29);
                assert.eq(profileDoc.docUnitsWritten, 1);
            }
            assert.eq(profileDoc.idxEntryBytesWritten, 3);
            assert.eq(profileDoc.idxEntryUnitsWritten, 1);
        }
    },
    {
        name: 'dropCollection',
        command: (db) => {
            assert(db[collName].drop());
        },
        profileFilter: {op: 'command', 'command.drop': collName},
        profileAssert: (profileDoc) => {
            // Reads from the collection catalog.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.gt(profileDoc.docBytesWritten, 0);
            assert.gt(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    resetProfileColl,
    {
        name: 'sample',
        command: (db) => {
            // For $sample to use a random cursor, we must have at least 100 documents and a sample
            // size less than 5%.
            for (let i = 0; i < 150; i++) {
                assert.commandWorked(db[collName].insert({_id: i, a: i}));
            }
            assert.eq(db[collName].aggregate([{$sample: {size: 5}}]).itcount(), 5);
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (profileDoc) => {
            // The exact amount of data read is not easily calculable.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    {
        name: 'createIndexUnique',
        command: (db) => {
            assert.commandWorked(db[collName].createIndex({a: 1}, {unique: true}));
        },
        profileFilter: {op: 'command', 'command.createIndexes': collName},
        profileAssert: (profileDoc) => {
            // The size of the collection document in the _mdb_catalog may not be the same every
            // test run, so only assert this is non-zero.
            // Index builds run on a separate thread and don't report their metrics with the
            // createIndex command, so we don't make any assertions.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    resetProfileColl,
    {
        name: 'insertUnique',
        command: (db) => {
            assert.commandWorked(db[collName].insert({_id: 150, a: 150}));
        },
        profileFilter: {op: 'insert', 'command.insert': collName},
        profileAssert: (profileDoc) => {
            // Insert should not perform any reads.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            // Reads the index entry for 'a' to determine uniqueness.
            assert.eq(profileDoc.idxEntryBytesRead, 6);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            if (isReplSet) {
                // Ensure writes to the oplog are counted. Some oplog fields like UUID are
                // randomized between runs, but the types are fixed-length, so we can make strong
                // assertions about the sizes. If the oplog format changes for any reason, then so
                // will these assertions.
                assert.eq(profileDoc.docBytesWritten, 188);
                assert.eq(profileDoc.docUnitsWritten, 3);
            } else {
                assert.eq(profileDoc.docBytesWritten, 29);
                assert.eq(profileDoc.docUnitsWritten, 1);
            }
            // Deletes one entry and writes another.
            assert.eq(profileDoc.idxEntryBytesWritten, 10);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
        }
    },
    resetProfileColl,
    {
        name: 'insertDup',
        command: (db) => {
            // Insert a duplicate key on 'a', not _id.
            assert.commandFailedWithCode(db[collName].insert({_id: 200, a: 0}),
                                         ErrorCodes.DuplicateKey);
        },
        profileFilter: {op: 'insert', 'command.insert': collName},
        profileAssert: (profileDoc) => {
            // Insert should not perform any reads.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            // Inserting into a unique index requires reading one key.
            assert.eq(profileDoc.idxEntryBytesRead, 4);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            // Despite failing to insert keys into the unique index, the operation first succeeded
            // in writing to the collection. Even though the operation was rolled-back, this counts
            // towards metrics.
            assert.eq(profileDoc.docBytesWritten, 29);
            assert.eq(profileDoc.docUnitsWritten, 1);
            assert.eq(profileDoc.idxEntryBytesWritten, 4);
            assert.eq(profileDoc.idxEntryUnitsWritten, 1);
        }
    },
    {
        name: 'updateWithoutModify',
        command: (db) => {
            assert.commandWorked(db[collName].update({_id: 1}, {$set: {a: 151}}));
        },
        profileFilter: {op: 'update', 'command.q': {_id: 1}},
        profileAssert: (profileDoc) => {
            // Should read exactly as many bytes are in the document. Debug builds may perform extra
            // reads of the _mdb_catalog.
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
            }
            // Reads index entries on '_id' for the lookup and 'a' to ensure uniqueness.
            assert.eq(profileDoc.idxEntryBytesRead, 9);
            assert.eq(profileDoc.idxEntryUnitsRead, 2);
            if (isReplSet) {
                // Ensure writes to the oplog are counted.
                assert.eq(profileDoc.docBytesWritten, 224);
                assert.eq(profileDoc.docUnitsWritten, 3);
            } else {
                // This out-of-place update should perform a direct insert because it is not large
                // enough to qualify for the in-place update path.
                assert.eq(profileDoc.docBytesWritten, 29);
                assert.eq(profileDoc.docUnitsWritten, 1);
            }
            // Removes one entry and inserts another.
            assert.eq(profileDoc.idxEntryBytesWritten, 11);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
        }
    },
    {
        name: 'updateWithModify',
        command: (db) => {
            // WT_MODIFY updates can be used to overwrite small regions of documents rather than
            // rewriting an entire document. They are only used under the following conditions:
            // * The collection is not journaled (i.e. it is a replicated user collection)
            // * The document is at least 1K bytes
            // * The updated document is no more than 10% larger than the original document
            assert.commandWorked(db[collName].insert({_id: 200, x: 'x'.repeat(1024)}));
            assert.commandWorked(db[collName].update({_id: 200}, {$set: {a: 200}}));
        },
        profileFilter: {op: 'update', 'command.q': {_id: 200}},
        profileAssert: (profileDoc) => {
            // Should read exactly as many bytes are in the document. Debug builds may perform extra
            // reads of the _mdb_catalog.
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 1050);
                assert.eq(profileDoc.docUnitsRead, 9);
            } else {
                assert.gte(profileDoc.docBytesRead, 1050);
                assert.gte(profileDoc.docUnitsRead, 9);
            }
            // Reads index entries on '_id' for the lookup and 'a' to ensure uniqueness.
            assert.eq(profileDoc.idxEntryBytesRead, 10);
            assert.eq(profileDoc.idxEntryUnitsRead, 2);
            if (isReplSet) {
                // When WT_MODIFY is used on a replicated collection, in addition to writing fewer
                // bytes per the comment about WT_MODIFY above, ensure it also inserts into the
                // oplog.
                assert.eq(profileDoc.docBytesWritten, 208);
                assert.eq(profileDoc.docUnitsWritten, 3);
            } else {
                assert.eq(profileDoc.docBytesWritten, 1061);
                assert.eq(profileDoc.docUnitsWritten, 9);
            }
            // Removes one entry and inserts another.
            assert.eq(profileDoc.idxEntryBytesWritten, 10);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
        }
    },
    {
        name: 'updateWithDamages',
        command: (db) => {
            // This update behaves differently from the 'updateWithModify' case above. It uses the
            // same WT_MODIFY update machinery and can be used on un-replicated collections, but is
            // limited instead to the following conditions:
            // * A field is replaced such that that the total size of the document remains unchanged
            // * No secondary indexes are affected (e.g. we are not updating 'a')
            assert.commandWorked(db[collName].insert({_id: 201, b: 0}));
            assert.commandWorked(db[collName].update({_id: 201}, {$set: {b: 1}}));
        },
        profileFilter: {op: 'update', 'command.q': {_id: 201}},
        profileAssert: (profileDoc) => {
            // Should read exactly as many bytes are in the document. Debug builds may perform extra
            // reads of the _mdb_catalog.
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 4);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            if (isReplSet) {
                // When WT_MODIFY is used on a replicated collection, in addition to writing fewer
                // bytes per the comment about WT_MODIFY above, ensure it also inserts into the
                // oplog.
                assert.eq(profileDoc.docBytesWritten, 211);
                assert.eq(profileDoc.docUnitsWritten, 3);
            } else {
                // This is calculated as the number of bytes overwritten + the number of bytes
                // written, and is still less than the full document size.
                assert.eq(profileDoc.docBytesWritten, 16);
                assert.eq(profileDoc.docUnitsWritten, 1);
            }
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    resetProfileColl,
    resetTestColl,
    {
        name: 'insertLegacy',
        command: (db) => {
            runInLegacyQueryMode(db, () => {
                db[collName].insert({_id: 1, a: 0});
            });
        },
        profileFilter: {op: 'insert'},
        profileAssert: (profileDoc) => {
            // Insert should not perform any reads.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            if (isReplSet) {
                assert.eq(profileDoc.docBytesWritten, 188);
                assert.eq(profileDoc.docUnitsWritten, 3);
            } else {
                assert.eq(profileDoc.docBytesWritten, 29);
                assert.eq(profileDoc.docUnitsWritten, 1);
            }
            assert.eq(profileDoc.idxEntryBytesWritten, 3);
            assert.eq(profileDoc.idxEntryUnitsWritten, 1);
        }
    },
    {
        name: 'findLegacy',
        command: (db) => {
            runInLegacyQueryMode(db, () => {
                assert.eq(db[collName].find({_id: 1}).itcount(), 1);
            });
        },
        profileFilter: {op: 'query', 'command.find': collName},
        profileAssert: (profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 29);
            assert.eq(profileDoc.docUnitsRead, 1);
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    resetProfileColl,
    {
        name: 'getMoreLegacy',
        command: (db) => {
            runInLegacyQueryMode(db, () => {
                db[collName].insert({_id: 2});
                db[collName].insert({_id: 3});
                // The value '1' is not a valid batch size for legacy queries, and will actually
                // return more than 1 document per batch.
                let cursor = db[collName].find().batchSize(2);
                cursor.next();
                cursor.next();
                assert.eq(cursor.objsLeftInBatch(), 0);
                // Trigger a getMore.
                cursor.next();
            });
        },
        profileFilter: {op: 'getmore'},
        profileAssert: (profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 18);
            assert.eq(profileDoc.docUnitsRead, 1);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    {
        name: 'updateLegacy',
        command: (db) => {
            runInLegacyQueryMode(db, () => {
                db[collName].update({_id: 1}, {$set: {a: 1}});
            });
        },
        profileFilter: {op: 'update'},
        profileAssert: (profileDoc) => {
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            if (isReplSet) {
                assert.eq(profileDoc.docBytesWritten, 211);
                assert.eq(profileDoc.docUnitsWritten, 3);
            } else {
                assert.eq(profileDoc.docBytesWritten, 16);
                assert.eq(profileDoc.docUnitsWritten, 1);
            }
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
        }
    },
    {
        name: 'deleteLegacy',
        command: (db) => {
            runInLegacyQueryMode(db, () => {
                db[collName].remove({_id: 1});
            });
        },
        profileFilter: {op: 'remove'},
        profileAssert: (profileDoc) => {
            // Due to a deficiency in the delete path, we read the same document twice.
            // TODO (SERVER-51420): Deletes should not read documents twice before deleting.
            if (!debugBuild) {
                assert.eq(profileDoc.docBytesRead, 58);
                assert.eq(profileDoc.docUnitsRead, 2);
            } else {
                assert.gte(profileDoc.docBytesRead, 58);
                assert.gte(profileDoc.docUnitsRead, 2);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            if (isReplSet) {
                assert.eq(profileDoc.docBytesWritten, 177);
                assert.eq(profileDoc.docUnitsWritten, 3);
            } else {
                assert.eq(profileDoc.docBytesWritten, 29);
                assert.eq(profileDoc.docUnitsWritten, 1);
            }
            assert.eq(profileDoc.idxEntryBytesWritten, 3);
            assert.eq(profileDoc.idxEntryUnitsWritten, 1);
        }
    },
    resetProfileColl,
    resetTestColl,
    {
        name: 'insertBulk',
        command: (db) => {
            let bulk = db[collName].initializeUnorderedBulkOp();
            for (let i = 0; i < 100; i++) {
                bulk.insert({_id: i, a: 0});
            }
            assert.commandWorked(bulk.execute());
        },
        profileFilter: {op: 'insert', 'command.insert': collName},
        profileAssert: (profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            if (isReplSet) {
                assert.eq(profileDoc.docBytesWritten, 18800);
                // Each inserted document counts for 1 document unit plus 2 document units for its
                // oplog entry (which is larger than the default unit size of 128 bytes).
                assert.eq(profileDoc.docUnitsWritten, 300);
            } else {
                assert.eq(profileDoc.docBytesWritten, 2900);
                assert.eq(profileDoc.docUnitsWritten, 100);
            }
            assert.eq(profileDoc.idxEntryBytesWritten, 299);
            assert.eq(profileDoc.idxEntryUnitsWritten, 100);
        }
    },

];

const profileColl = testDB.system.profile;
const testOperation = (operation) => {
    jsTestLog("Testing operation: " + operation.name);
    operation.command(testDB);
    if (!operation.profileFilter) {
        return;
    }

    const cursor = profileColl.find(operation.profileFilter);
    assert(cursor.hasNext(), () => {
        // Get the last operation that was not a find on the profile collection.
        const lastOp =
            profileColl.find({'command.find': {$ne: 'system.profile'}}).sort({$natural: -1}).next();
        return "Could not find operation in profiler with filter: " +
            tojson(operation.profileFilter) +
            ". Last operation in profile collection is: " + tojson(lastOp);
    });
    const entry = cursor.next();
    assert(!cursor.hasNext(), () => {
        return "Filter for profiler matched more than one entry: filter: " +
            tojson(operation.profileFilter) + ", first entry: " + tojson(entry) +
            ", second entry: " + tojson(cursor.next());
    });

    assertMetricsExist(entry);
    if (operation.profileAssert) {
        try {
            operation.profileAssert(entry.operationMetrics);
        } catch (e) {
            print("Caught exception while checking profile entry for '" + operation.name +
                  "' : " + tojson(entry));
            throw e;
        }
    }
};

operations.forEach((op) => {
    testOperation(op);
});
})();