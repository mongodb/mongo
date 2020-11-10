/**
 * Tests that resource consumption metrics are reported in the profiler.
 *
 *  @tags: [
 *    requires_capped,
 *    requires_fcv_47,
 *    requires_replication,
 *    requires_wiredtiger,
 *  ]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For isReplSet().

const dbName = jsTestName();
const collName = 'coll';

const isLinux = getBuildInfo().buildEnvironment.target_os == "linux";
const isDebugBuild = (db) => {
    return db.adminCommand('buildInfo').debug;
};

const assertMetricsExist = (profilerEntry) => {
    let metrics = profilerEntry.operationMetrics;
    assert.neq(metrics, undefined);

    assert.gte(metrics.docBytesRead, 0);
    assert.gte(metrics.docUnitsRead, 0);
    assert.gte(metrics.idxEntryBytesRead, 0);
    assert.gte(metrics.idxEntryUnitsRead, 0);
    assert.gte(metrics.keysSorted, 0);
    assert.gte(metrics.sorterSpills, 0);
    assert.gte(metrics.docUnitsReturned, 0);
    assert.gte(metrics.cursorSeeks, 0);

    // Every test should perform enough work to be measured as non-zero CPU activity in
    // nanoseconds.
    // The CPU time metrics are only collected on Linux.
    if (isLinux) {
        assert.gt(metrics.cpuNanos, 0);
    }
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
        profileAssert: (db, profileDoc) => {
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
            assert.gt(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'createIndex',
        command: (db) => {
            assert.commandWorked(db[collName].createIndex({a: 1}));
        },
        profileFilter: {op: 'command', 'command.createIndexes': collName},
        profileAssert: (db, profileDoc) => {
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
            assert.gt(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'findEmpty',
        command: (db) => {
            assert.eq(db[collName].find({a: 1}).itcount(), 0);
        },
        profileFilter: {op: 'query', 'command.find': collName, 'command.filter': {a: 1}},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            // This tests to make sure we only increment the cusorSeeks counter if the cursor seek
            // is successful. In this case, the seek is not successful because the index is empty.
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'insert',
        command: (db) => {
            assert.commandWorked(db[collName].insert({_id: 1, a: 0}));
        },
        profileFilter: {op: 'insert', 'command.insert': collName},
        profileAssert: (db, profileDoc) => {
            // Insert should not perform any reads.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 29);
            assert.eq(profileDoc.docUnitsWritten, 1);
            assert.eq(profileDoc.idxEntryBytesWritten, 7);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
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
        profileAssert: (db, profileDoc) => {
            // Should read exactly as many bytes are in the document.
            assert.eq(profileDoc.docBytesRead, 29);
            assert.eq(profileDoc.docUnitsRead, 1);
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.eq(profileDoc.cursorSeeks, 2);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'findCollScan',
        command: (db) => {
            assert.eq(db[collName].find().itcount(), 1);
        },
        profileFilter: {op: 'query', 'command.find': collName, 'command.filter': {}},
        profileAssert: (db, profileDoc) => {
            // Should read exactly as many bytes are in the document.
            assert.eq(profileDoc.docBytesRead, 29);
            assert.eq(profileDoc.docUnitsRead, 1);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'aggregate',
        command: (db) => {
            assert.eq(db[collName].aggregate([{$project: {_id: 1}}]).itcount(), 1);
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (db, profileDoc) => {
            // Should read exactly as many bytes are in the document.
            assert.eq(profileDoc.docBytesRead, 29);
            assert.eq(profileDoc.docUnitsRead, 1);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'distinct',
        command: (db) => {
            assert.eq(db[collName].distinct("_id").length, 1);
        },
        profileFilter: {op: 'command', 'command.distinct': collName},
        profileAssert: (db, profileDoc) => {
            // Does not read from the collection.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.eq(profileDoc.cursorSeeks, 2);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'findAndModify',
        command: (db) => {
            assert(db[collName].findAndModify({query: {_id: 1}, update: {$set: {a: 1}}}));
        },
        profileFilter: {op: 'command', 'command.findandmodify': collName},
        profileAssert: (db, profileDoc) => {
            // Should read exactly as many bytes are in the document. Debug builds may perform extra
            // reads of the _mdb_catalog.
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
                assert.eq(profileDoc.cursorSeeks, 3);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, 3);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            // This update will not be performed in-place because it is too small and affects an
            // index.
            assert.eq(profileDoc.docBytesWritten, 29);
            assert.eq(profileDoc.docUnitsWritten, 1);
            // Deletes one index entry and writes another.
            assert.eq(profileDoc.idxEntryBytesWritten, 9);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'update',
        command: (db) => {
            assert.commandWorked(db[collName].update({_id: 1}, {$set: {a: 2}}));
        },
        profileFilter: {op: 'update', 'command.q': {_id: 1}},
        profileAssert: (db, profileDoc) => {
            // Should read exactly as many bytes are in the document. Debug builds may perform extra
            // reads of the _mdb_catalog.
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docUnitsRead, 1);
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.cursorSeeks, 3);
            } else {
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.cursorSeeks, 3);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            // This update will not be performed in-place because it is too small and affects an
            // index.
            assert.eq(profileDoc.docBytesWritten, 29);
            assert.eq(profileDoc.docUnitsWritten, 1);
            // Deletes one index entry and writes another.
            assert.eq(profileDoc.idxEntryBytesWritten, 10);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'count',
        command: (db) => {
            assert.eq(1, db[collName].count());
        },
        profileFilter: {op: 'command', 'command.count': collName},
        profileAssert: (db, profileDoc) => {
            // Reads from the fast-count, not the collection.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
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
        profileAssert: (db, profileDoc) => {
            // Should not read from the collection.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
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
        profileAssert: (db, profileDoc) => {
            // Should read from the collection.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
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
        profileAssert: (db, profileDoc) => {
            // This reads from the collection catalog.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'dropIndex',
        command: (db) => {
            assert.commandWorked(db[collName].dropIndex({a: 1}));
        },
        profileFilter: {op: 'command', 'command.dropIndexes': collName},
        profileAssert: (db, profileDoc) => {
            // This reads from the collection catalog.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.gt(profileDoc.cursorSeeks, 0);
            assert.gt(profileDoc.docBytesWritten, 0);
            assert.gt(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
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
        profileAssert: (db, profileDoc) => {
            // Debug builds may perform extra reads of the _mdb_catalog.
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
                assert.eq(profileDoc.cursorSeeks, 0);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, 0);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'deleteIxScan',
        command: (db) => {
            assert.commandWorked(db[collName].remove({_id: 1}));
        },
        profileFilter: {op: 'remove', 'command.q': {_id: 1}},
        profileAssert: (db, profileDoc) => {
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
                assert.eq(profileDoc.cursorSeeks, 3);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, 3);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            // Deleted bytes are counted as 'written'.
            assert.eq(profileDoc.docBytesWritten, 29);
            assert.eq(profileDoc.docUnitsWritten, 1);
            assert.eq(profileDoc.idxEntryBytesWritten, 3);
            assert.eq(profileDoc.idxEntryUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'deleteCollScan',
        command: (db) => {
            assert.commandWorked(db[collName].remove({}));
        },
        profileFilter: {op: 'remove', 'command.q': {}},
        profileAssert: (db, profileDoc) => {
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
                assert.eq(profileDoc.cursorSeeks, 1);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, 1);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            // Deleted bytes are counted as 'written'.
            assert.eq(profileDoc.docBytesWritten, 29);
            assert.eq(profileDoc.docUnitsWritten, 1);
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
        profileAssert: (db, profileDoc) => {
            // Reads from the collection catalog.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.gt(profileDoc.cursorSeeks, 0);
            assert.gt(profileDoc.docBytesWritten, 0);
            assert.gt(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
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
        profileAssert: (db, profileDoc) => {
            // The exact amount of data read is not easily calculable.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    resetProfileColl,
    {
        name: 'sampleWithSort',
        command: (db) => {
            // For $sample to not use a random cursor and use sorting, we must use a sample size
            // larger than 5%.
            assert.eq(db[collName].aggregate([{$sample: {size: 10}}]).itcount(), 10);
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (db, profileDoc) => {
            // This operation will read all documents and sort a random sample of them.
            assert.eq(profileDoc.docBytesRead, 29 * 150);
            assert.eq(profileDoc.docUnitsRead, 150);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 150);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'createIndexUnique',
        command: (db) => {
            assert.commandWorked(db[collName].createIndex({a: 1}, {unique: true}));
        },
        profileFilter: {op: 'command', 'command.createIndexes': collName},
        profileAssert: (db, profileDoc) => {
            // The size of the collection document in the _mdb_catalog may not be the same every
            // test run, so only assert this is non-zero.
            // Index builds run on a separate thread and don't report their metrics with the
            // createIndex command, so we don't make any assertions.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    resetProfileColl,
    {
        name: 'insertUnique',
        command: (db) => {
            assert.commandWorked(db[collName].insert({_id: 150, a: 150}));
        },
        profileFilter: {op: 'insert', 'command.insert': collName},
        profileAssert: (db, profileDoc) => {
            // Insert should not perform any reads.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            // Reads the index entry for 'a' to determine uniqueness.
            assert.eq(profileDoc.idxEntryBytesRead, 6);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.eq(profileDoc.cursorSeeks, 1);
            assert.eq(profileDoc.docBytesWritten, 29);
            assert.eq(profileDoc.docUnitsWritten, 1);
            // Deletes one entry and writes another.
            assert.eq(profileDoc.idxEntryBytesWritten, 10);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
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
        profileAssert: (db, profileDoc) => {
            // Insert should not perform any reads.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            // Inserting into a unique index requires reading one key.
            assert.eq(profileDoc.idxEntryBytesRead, 4);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.eq(profileDoc.cursorSeeks, 1);
            // Despite failing to insert keys into the unique index, the operation first succeeded
            // in writing to the collection. Even though the operation was rolled-back, this counts
            // towards metrics.
            assert.eq(profileDoc.docBytesWritten, 29);
            assert.eq(profileDoc.docUnitsWritten, 1);
            assert.eq(profileDoc.idxEntryBytesWritten, 4);
            assert.eq(profileDoc.idxEntryUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'updateWithoutModify',
        command: (db) => {
            assert.commandWorked(db[collName].update({_id: 1}, {$set: {a: 151}}));
        },
        profileFilter: {op: 'update', 'command.q': {_id: 1}},
        profileAssert: (db, profileDoc) => {
            // Should read exactly as many bytes are in the document. Debug builds may perform extra
            // reads of the _mdb_catalog.
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
                // There are 4 seeks:
                // 1) Reading the _id index.
                // 2) Reading the document on the collection.
                // 3) Reading the document again before updating.
                // 4) Seeking on the _id index to check for uniqueness.
                assert.eq(profileDoc.cursorSeeks, 4);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, 4);
            }
            // Reads index entries on '_id' for the lookup and 'a' to ensure uniqueness.
            assert.eq(profileDoc.idxEntryBytesRead, 9);
            assert.eq(profileDoc.idxEntryUnitsRead, 2);
            // This out-of-place update should perform a direct insert because it is not large
            // enough to qualify for the in-place update path.
            assert.eq(profileDoc.docBytesWritten, 29);
            assert.eq(profileDoc.docUnitsWritten, 1);
            // Removes one entry and inserts another.
            assert.eq(profileDoc.idxEntryBytesWritten, 11);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
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
        profileAssert: (db, profileDoc) => {
            // Should read exactly as many bytes are in the document. Debug builds may perform extra
            // reads of the _mdb_catalog.
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, 1050);
                assert.eq(profileDoc.docUnitsRead, 9);
                assert.eq(profileDoc.cursorSeeks, 4);
            } else {
                assert.gte(profileDoc.docBytesRead, 1050);
                assert.gte(profileDoc.docUnitsRead, 9);
                assert.gte(profileDoc.cursorSeeks, 4);
            }
            // Reads index entries on '_id' for the lookup and 'a' to ensure uniqueness.
            assert.eq(profileDoc.idxEntryBytesRead, 10);
            assert.eq(profileDoc.idxEntryUnitsRead, 2);
            if (FixtureHelpers.isReplSet(db)) {
                // When WT_MODIFY is used on a replicated collection fewer bytes are written per the
                // comment about WT_MODIFY above.
                assert.eq(profileDoc.docBytesWritten, 13);
                assert.eq(profileDoc.docUnitsWritten, 1);
            } else {
                assert.eq(profileDoc.docBytesWritten, 1061);
                assert.eq(profileDoc.docUnitsWritten, 9);
            }
            // Removes one entry and inserts another.
            assert.eq(profileDoc.idxEntryBytesWritten, 10);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
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
        profileAssert: (db, profileDoc) => {
            // Should read exactly as many bytes are in the document. Debug builds may perform extra
            // reads of the _mdb_catalog.
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
                assert.eq(profileDoc.cursorSeeks, 2);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, 2);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 4);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            // This is calculated as the number of bytes overwritten + the number of bytes
            // written, and is still less than the full document size.
            assert.eq(profileDoc.docBytesWritten, 16);
            assert.eq(profileDoc.docUnitsWritten, 1);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
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
        profileAssert: (db, profileDoc) => {
            // Insert should not perform any reads.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 29);
            assert.eq(profileDoc.docUnitsWritten, 1);
            assert.eq(profileDoc.idxEntryBytesWritten, 3);
            assert.eq(profileDoc.idxEntryUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
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
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 29);
            assert.eq(profileDoc.docUnitsRead, 1);
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.eq(profileDoc.cursorSeeks, 2);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
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
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 18);
            assert.eq(profileDoc.docUnitsRead, 1);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
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
        profileAssert: (db, profileDoc) => {
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
                assert.eq(profileDoc.cursorSeeks, 2);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, 2);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.eq(profileDoc.docBytesWritten, 16);
            assert.eq(profileDoc.docUnitsWritten, 1);
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
        profileAssert: (db, profileDoc) => {
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
                assert.eq(profileDoc.cursorSeeks, 3);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, 3);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 3);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.eq(profileDoc.docBytesWritten, 29);
            assert.eq(profileDoc.docUnitsWritten, 1);
            assert.eq(profileDoc.idxEntryBytesWritten, 3);
            assert.eq(profileDoc.idxEntryUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    resetProfileColl,
    resetTestColl,
    {
        name: 'insertBulk',
        command: (db) => {
            let bulk = db[collName].initializeUnorderedBulkOp();
            for (let i = 0; i < 100; i++) {
                // There should be 10 distinct values of 'a' from 0 to 9.
                bulk.insert({_id: i, a: Math.floor(i / 10)});
            }
            assert.commandWorked(bulk.execute());
        },
        profileFilter: {op: 'insert', 'command.insert': collName},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 2900);
            assert.eq(profileDoc.docUnitsWritten, 100);
            assert.eq(profileDoc.idxEntryBytesWritten, 299);
            assert.eq(profileDoc.idxEntryUnitsWritten, 100);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'sortSimple',
        command: (db) => {
            // This uses a sort plan for queries that only need to sort keys that are returned, and
            // no additional metadata. This is achieved by projecting and sorting on the same field.
            let cur = db[collName].find({}, {a: 1}).sort({a: 1});
            assert.eq(100, cur.itcount());
        },
        profileFilter: {op: 'query', 'command.find': collName},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 29 * 100);
            assert.eq(profileDoc.docUnitsRead, 100);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 0);
        },
    },
    resetProfileColl,
    {
        name: 'sortDefault',
        command: (db) => {
            // This uses a sort plan for queries that sort on one field but return full documents.
            let cur = db[collName].find({}).sort({a: 1});
            assert.eq(100, cur.itcount());
        },
        profileFilter: {op: 'query', 'command.find': collName},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 29 * 100);
            assert.eq(profileDoc.docUnitsRead, 100);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 0);
        },
    },
    {
        name: 'sortStage',
        command: (db) => {
            let cur = db[collName].aggregate([{$sort: {a: 1}}]);
            assert.eq(100, cur.itcount());
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 29 * 100);
            assert.eq(profileDoc.docUnitsRead, 100);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 0);
        },
    },
    resetProfileColl,
    {
        name: 'sortLimitOne',
        command: (db) => {
            let cur = db[collName].aggregate([{$sort: {a: 1}}, {$limit: 1}]);
            assert.eq(1, cur.itcount());
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 29 * 100);
            assert.eq(profileDoc.docUnitsRead, 100);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 0);
        },
    },
    resetProfileColl,
    {
        name: 'sortTopK',
        command: (db) => {
            let cur = db[collName].aggregate([{$sort: {a: 1}}, {$limit: 5}]);
            assert.eq(5, cur.itcount());
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 29 * 100);
            assert.eq(profileDoc.docUnitsRead, 100);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 0);
        },
    },
    resetProfileColl,
    {
        name: 'sortSpills',
        command: (db) => {
            // Force the sorter to spill for every document by lowering the memory usage limit.
            const originalSortBytes =
                assert
                    .commandWorked(db.adminCommand(
                        {getParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 1}))
                    .internalQueryMaxBlockingSortMemoryUsageBytes;
            assert.commandWorked(db.adminCommand(
                {setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 1}));
            let cur = db[collName].aggregate([{$sort: {a: 1}}], {allowDiskUse: true});
            assert.eq(100, cur.itcount());
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryMaxBlockingSortMemoryUsageBytes: originalSortBytes
            }));
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 29 * 100);
            assert.eq(profileDoc.docUnitsRead, 100);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 101);
        },
    },
    resetProfileColl,
    {
        name: 'groupStage',
        command: (db) => {
            // There should be 10 distinct values for 'a'.
            let cur = db[collName].aggregate([{$group: {_id: "$a", count: {$sum: 1}}}]);
            assert.eq(cur.itcount(), 10);
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 29 * 100);
            assert.eq(profileDoc.docUnitsRead, 100);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            if (isDebugBuild(db)) {
                // In debug builds we sort and spill for each of the first 20 documents. Once we
                // reach that limit, we stop spilling as often. This 26 is the sum of 20 debug sorts
                // and spills of documents in groups 0 through 3 plus 6 debug spills and sorts for
                // groups 4 through 10.
                assert.eq(profileDoc.keysSorted, 26);
                // This 21 is the sum of 20 debug spills plus 1 final debug spill
                assert.eq(profileDoc.sorterSpills, 21);
            } else {
                // No sorts required.
                assert.eq(profileDoc.keysSorted, 0);
                assert.eq(profileDoc.sorterSpills, 0);
            }
        },
    },
    resetProfileColl,
    {
        name: 'bucketAuto',
        command: (db) => {
            // This uses the aggregation pipeline sort stage.
            let cur = db[collName].aggregate([{$bucketAuto: {groupBy: "$a", buckets: 10}}]);
            assert.eq(cur.next().count, 10);
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 29 * 100);
            assert.eq(profileDoc.docUnitsRead, 100);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 0);
        },
    },
    {
        name: 'cappedInitialSetup',
        command: (db) => {
            db.capped.drop();
            assert.commandWorked(
                db.createCollection("capped", {capped: true, size: 4096, max: 10}));
            db.capped.insert({_id: 0, a: 0});
        },
        profileFilter: {op: 'insert', 'command.insert': 'capped'},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 29);
            assert.eq(profileDoc.docUnitsWritten, 1);
            assert.eq(profileDoc.idxEntryBytesWritten, 2);
            assert.eq(profileDoc.idxEntryUnitsWritten, 1);
        }
    },
    resetProfileColl,
    {
        name: 'cappedFillWithNineDocs',
        command: (db) => {
            let docs = [];
            for (let i = 1; i < 10; i++) {
                docs.push({_id: i, a: i});
            }
            db.capped.insertMany(docs);
            assert.eq(db.capped.find({_id: 0}).itcount(), 1);
        },
        profileFilter: {op: 'insert', 'command.insert': 'capped'},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 261);
            assert.eq(profileDoc.docUnitsWritten, 9);
            assert.eq(profileDoc.idxEntryBytesWritten, 27);
            assert.eq(profileDoc.idxEntryUnitsWritten, 9);
        }
    },
    resetProfileColl,
    {
        name: 'cappedExtraOne',
        command: (db) => {
            db.capped.insert({_id: 10, a: 10});
            assert.eq(db.capped.find({a: 0}).itcount(), 0);
        },
        profileFilter: {op: 'insert', 'command.insert': 'capped'},
        profileAssert: (db, profileDoc) => {
            if (!isDebugBuild(db)) {
                // Should read exactly as many bytes as are in the document that is being deleted.
                // Debug builds may perform extra reads of the _mdb_catalog.
                assert.eq(profileDoc.docBytesRead, 29);
                assert.eq(profileDoc.docUnitsRead, 1);
                // The first time doing a capped delete, we don't know what the _cappedFirstRecord
                // should be so we cannot seek to it.
                assert.eq(profileDoc.cursorSeeks, 0);
            } else {
                assert.gte(profileDoc.docBytesRead, 29);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, 0);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 58);
            assert.eq(profileDoc.docUnitsWritten, 2);
            assert.eq(profileDoc.idxEntryBytesWritten, 5);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
        }
    },
    resetProfileColl,
    {
        name: 'cappedExtraNine',
        command: (db) => {
            let docs = [];
            for (let i = 11; i < 20; i++) {
                docs.push({_id: i, a: i});
            }
            db.capped.insertMany(docs);
            assert.eq(db.capped.find({a: 9}).itcount(), 0);
            assert.eq(db.capped.find({a: 10}).itcount(), 1);
        },
        profileFilter: {op: 'insert', 'command.insert': 'capped'},
        profileAssert: (db, profileDoc) => {
            if (!isDebugBuild(db)) {
                // Should read exactly as many bytes as are in the documents that are being deleted.
                // Debug builds may perform extra reads of the _mdb_catalog.
                assert.eq(profileDoc.docBytesRead, 261);
                assert.eq(profileDoc.docUnitsRead, 9);
                // Each capped delete seeks twice if it knows what record to start at. Once at the
                // start of the delete process and then once again to reposition itself at the first
                // record to delete after it figures out how many documents must be deleted.
                assert.eq(profileDoc.cursorSeeks, 18);
            } else {
                assert.gte(profileDoc.docBytesRead, 261);
                assert.gte(profileDoc.docUnitsRead, 9);
                assert.gte(profileDoc.cursorSeeks, 18);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 522);
            assert.eq(profileDoc.docUnitsWritten, 18);
            assert.eq(profileDoc.idxEntryBytesWritten, 54);
            assert.eq(profileDoc.idxEntryUnitsWritten, 18);
        }
    }
];

const testOperation = (db, operation) => {
    jsTestLog("Testing operation: " + operation.name);
    operation.command(db);
    if (!operation.profileFilter) {
        return;
    }

    const profileColl = db.system.profile;
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
            operation.profileAssert(db, entry.operationMetrics);
        } catch (e) {
            print("Caught exception while checking profile entry for '" + operation.name +
                  "' : " + tojson(entry));
            throw e;
        }
    }
};

const setParams = {
    measureOperationResourceConsumption: true
};

const runTest = (db) => {
    db.setProfilingLevel(2, 0);
    operations.forEach((op) => {
        testOperation(db, op);
    });
};

jsTestLog("Testing standalone");
(function testStandalone() {
    const conn = MongoRunner.runMongod({setParameter: setParams});
    const db = conn.getDB(dbName);
    runTest(db);
    MongoRunner.stopMongod(conn);
})();

jsTestLog("Testing replica set");
(function testReplicaSet() {
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: setParams}});
    rst.startSet();
    rst.initiate();

    const db = rst.getPrimary().getDB(dbName);
    runTest(db);
    rst.stopSet();
})();
})();
