/**
 * Tests that resource consumption metrics are reported in the profiler.
 *
 * @tags: [
 *   requires_capped,
 *   requires_fcv_63,
 *   requires_replication,
 *   requires_wiredtiger,
 *   # TODO SERVER-71170: docBytesRead for read operations using cqf are reported are higher than
 *   # tests expect.
 *   cqf_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.
load("jstests/libs/fixture_helpers.js");             // For isReplSet().
load("jstests/libs/os_helpers.js");                  // For isLinux().
load("jstests/libs/sbe_util.js");                    // For checkSBEEnabled().

const dbName = jsTestName();
const collName = 'coll';

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

    // Even though every test should perform enough work to be measured as non-zero CPU activity in
    // nanoseconds, the OS is only required to return monotonically-increasing values. That means
    // the OS may occasionally return the same CPU time between two different reads of the timer,
    // resulting in the server calculating zero elapsed time.
    // The CPU time metrics are only collected on Linux.
    if (isLinux()) {
        assert.gte(metrics.cpuNanos, 0);
    }
    assert.gte(metrics.docBytesWritten, 0);
    assert.gte(metrics.docUnitsWritten, 0);
    assert.gte(metrics.idxEntryBytesWritten, 0);
    assert.gte(metrics.idxEntryUnitsWritten, 0);
    assert.gte(metrics.totalUnitsWritten, 0);
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
        db[collName].drop();
        assert.commandWorked(db.createCollection(collName));
    },
};

// The value below is empirical and is size in bytes of the KeyString representation of the index
// item in the _id index, when _id contains a double from the set {1, ..., 9}.
const idxEntrySize = 3;

// The sizes below are empirical. Set them to the expected values before running the tests that use
// the same document/index data.
let singleDocSize = 0;
let secondaryIndexEntrySize = 0;

// For point-queries on _id field, we currently report 2 cursor seeks.
const nSeeksForIdxHackPlans = 2;

// NB: The order of operations is important as the later ones might rely on the state of the target
// collection, created by the previous operations.
const operations = [
    //
    // Test profiling of collection's create, findEmpty, drop
    //
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
            assert.gt(profileDoc.totalUnitsWritten, 0);
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
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
        }
    },
    {
        name: 'dropCollection',
        command: (db) => {
            assert.commandWorked(db[collName].insert({_id: 1, a: 0}));
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
            assert.gt(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
        }
    },

    //
    // Test profiling of index DDL.
    //
    resetTestColl,
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
            assert.gt(profileDoc.totalUnitsWritten, 0);
            assert.gt(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
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
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
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
            assert.gt(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
        }
    },

    //
    // Test profiling of basic read operations.
    //
    // The tests in the following group assume that the collection has a single document
    // {_id: 1, a: <value>} and only the default index on the '_id' field. Within the group the
    // tests should be runnable in any order as they don't modify the collection state.
    //
    {
        name: 'setupForReadTests',
        command: (db) => {
            db[collName].drop();
            assert.commandWorked(db[collName].insert({_id: 1, a: 0}));
            singleDocSize = 29;  // empirical size for the document above
        },
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
            assert.eq(profileDoc.docBytesRead, singleDocSize);
            assert.eq(profileDoc.docUnitsRead, 1);
            assert.eq(profileDoc.idxEntryBytesRead, idxEntrySize);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.eq(profileDoc.cursorSeeks, nSeeksForIdxHackPlans);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 1);
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
            assert.eq(profileDoc.docBytesRead, singleDocSize);
            assert.eq(profileDoc.docUnitsRead, 1);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 1);
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
            assert.eq(profileDoc.docBytesRead, singleDocSize);
            assert.eq(profileDoc.docUnitsRead, 1);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 1);
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
            assert.eq(profileDoc.idxEntryBytesRead, idxEntrySize);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.eq(profileDoc.cursorSeeks, 2);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
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
            assert.eq(profileDoc.docUnitsReturned, 0);
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
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
        }
    },
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
            assert.eq(profileDoc.docUnitsReturned, 0);
        }
    },
    {
        name: 'getMore',
        command: (db) => {
            db[collName].insert({_id: 2, a: 2});
            let cursor = db[collName].find().batchSize(1);
            cursor.next();
            assert.eq(cursor.objsLeftInBatch(), 0);
            // Trigger a getMore
            cursor.next();
            // Restore the collection state.
            assert.commandWorked(db[collName].remove({_id: 2}));
        },
        profileFilter: {op: 'getmore', 'command.collection': collName},
        profileAssert: (db, profileDoc) => {
            // Debug builds may perform extra reads of the _mdb_catalog.
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, singleDocSize);
                assert.eq(profileDoc.docUnitsRead, 1);
                assert.eq(profileDoc.cursorSeeks, 0);
            } else {
                assert.gte(profileDoc.docBytesRead, singleDocSize);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, 0);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 1);
        }
    },

    //
    // Test profiling of update operations, including their effects on indexes.
    //
    // To ensure an easy to reproduce state, each of the tests re-creates the data and indexes
    // it needs, so the state is not affected by the previously run tests.
    //
    {
        name: 'setupForUpdatesWithIndexes',
        command: (db) => {
            // Empirical value for the doc like {_id: <double>, a: <double>, b: <double>}. All tests
            // in this group operate only with docs with schema like this.
            singleDocSize = 40;

            // The value below is empirical for indexes like {a: 1}, where 'a' contains a double
            // from the set {1, ..., 9} (other values might produces different index entry size).
            secondaryIndexEntrySize = 5;
        }
    },
    {
        name: 'insert',
        command: (db) => {
            db[collName].drop();
            assert.commandWorked(db.createCollection(collName));

            assert.commandWorked(db[collName].insert({_id: 1, a: 1, b: 1}));
        },
        profileFilter: {op: 'insert', 'command.insert': collName},
        profileAssert: (db, profileDoc) => {
            // Insert should not perform any reads.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);

            // The insert updates the _id index and in other situations, updates on unique indexes
            // cause seeks into them... why not here?
            assert.eq(profileDoc.cursorSeeks, 0);

            assert.eq(profileDoc.docBytesWritten, singleDocSize);
            assert.eq(profileDoc.docUnitsWritten, 1);
            assert.eq(profileDoc.idxEntryBytesWritten, idxEntrySize);
            assert.eq(profileDoc.idxEntryUnitsWritten, 1);
            assert.eq(profileDoc.totalUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
        }
    },
    {
        name: 'insert-withSingleSecondaryIndex',
        command: (db) => {
            db[collName].drop();
            assert.commandWorked(db.createCollection(collName));
            assert.commandWorked(db[collName].createIndex({a: 1}));
            assert.commandWorked(db[collName].insert({_id: 1, a: 1, b: 1}));

            assert.commandWorked(db[collName].insert({_id: 2, a: 2, b: 2}));
        },
        profileFilter: {op: 'insert', 'command.insert': collName},
        profileAssert: (db, profileDoc) => {
            // Insert should not perform any reads.
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);

            // The insert updates the _id index and in other situations, updates on unique indexes
            // cause seeks into them... why not here?
            assert.eq(profileDoc.cursorSeeks, 0);

            assert.eq(profileDoc.docBytesWritten, singleDocSize);
            assert.eq(profileDoc.docUnitsWritten, 1);
            assert.eq(profileDoc.idxEntryBytesWritten, idxEntrySize + secondaryIndexEntrySize);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
            assert.eq(profileDoc.totalUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
        }
    },
    {
        name: 'update-noSecondaryIndexes',
        command: (db) => {
            db[collName].drop();
            assert.commandWorked(db[collName].insert({_id: 1, a: 0, b: 0}));

            // Adding a sibling cannot be done in-place.
            assert.commandWorked(db[collName].update({_id: 1}, {$unset: {b: ""}, $set: {c: 1}}));
        },
        profileFilter: {op: 'update', 'command.q': {_id: 1}},
        profileAssert: (db, profileDoc) => {
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, singleDocSize);
                assert.eq(profileDoc.docUnitsRead, 1);
                // The additional seek is to ensure uniqueness of the _id index.
                assert.eq(profileDoc.cursorSeeks, nSeeksForIdxHackPlans + 1);
            } else {
                assert.gte(profileDoc.docBytesRead, singleDocSize);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, nSeeksForIdxHackPlans + 1);
            }
            // This query does ixscan of the primary index.
            assert.eq(profileDoc.idxEntryBytesRead, idxEntrySize);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);

            // This is not an in-place update (but the updated doc has the same size).
            assert.eq(profileDoc.docBytesWritten, singleDocSize);
            assert.eq(profileDoc.docUnitsWritten, 1);

            // No indexes should be updated.
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);

            assert.eq(profileDoc.totalUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
        }
    },
    {
        name: 'update-inplace-noSecondaryIndexes',
        command: (db) => {
            db[collName].drop();
            assert.commandWorked(db[collName].insert({_id: 1, a: 0, b: 0}));

            assert.commandWorked(db[collName].update({_id: 1}, {$inc: {a: 1}}));
        },
        profileFilter: {op: 'update', 'command.q': {_id: 1}},
        profileAssert: (db, profileDoc) => {
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, singleDocSize);
                assert.eq(profileDoc.docUnitsRead, 1);
                // For in-place update uniqueness of _id index isn't checked so no extra seeks.
                assert.eq(profileDoc.cursorSeeks, nSeeksForIdxHackPlans);
            } else {
                assert.gte(profileDoc.docBytesRead, singleDocSize);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, nSeeksForIdxHackPlans);
            }
            // This query does ixscan of the primary index.
            assert.eq(profileDoc.idxEntryBytesRead, idxEntrySize);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);

            // In-place update of a single field.
            assert.lt(profileDoc.docBytesWritten, singleDocSize / 2);
            assert.eq(profileDoc.docUnitsWritten, 1);

            // No indexes should be updated.
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);

            assert.eq(profileDoc.totalUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
        }
    },
    {
        name: 'update-inplace-singleIndexAffected',
        command: (db) => {
            db[collName].drop();
            assert.commandWorked(db[collName].insert({_id: 1, a: 1, b: 1}));
            assert.commandWorked(db[collName].createIndex({a: 1}));
            assert.commandWorked(db[collName].createIndex({b: 1}));

            assert.commandWorked(db[collName].update({_id: 1}, {$inc: {a: 1}}));
        },
        profileFilter: {op: 'update', 'command.q': {_id: 1}},
        profileAssert: (db, profileDoc) => {
            // In-place update of a single field even when an index has to be updated.
            assert.lt(profileDoc.docBytesWritten, singleDocSize / 2);
            assert.eq(profileDoc.docUnitsWritten, 1);

            // Only the index on "a" should be updated.
            assert.eq(profileDoc.idxEntryBytesWritten, 2 * secondaryIndexEntrySize);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
        }
    },
    {
        name: 'update-inplace-twoIndexesAffected',
        command: (db) => {
            db[collName].drop();
            assert.commandWorked(db[collName].insert({_id: 1, a: 1, b: 1}));
            assert.commandWorked(db[collName].createIndex({a: 1}, {unique: true}));
            assert.commandWorked(db[collName].createIndex({b: 1}));

            assert.commandWorked(db[collName].update({_id: 1}, {$inc: {a: 1, b: 1}}));
        },
        profileFilter: {op: 'update', 'command.q': {_id: 1}},
        profileAssert: (db, profileDoc) => {
            if (!isDebugBuild(db)) {
                // For in-place updates the uniqueness of _id index doesn't need to be checked, but
                // checking the unique index on 'a' adds one more seek.
                assert.eq(profileDoc.cursorSeeks, nSeeksForIdxHackPlans + 1);
            } else {
                assert.gte(profileDoc.cursorSeeks, nSeeksForIdxHackPlans + 1);
            }

            // In-place update even when an index has to be updated.
            assert.lt(profileDoc.docBytesWritten, singleDocSize);
            assert.eq(profileDoc.docUnitsWritten, 1);

            // Both indexes should be updated so x2 compared with a single index test.
            assert.eq(profileDoc.idxEntryBytesWritten, 4 * secondaryIndexEntrySize);
            assert.eq(profileDoc.idxEntryUnitsWritten, 4);
        }
    },
    {
        name: 'findAndModifyUpdate-inplace',
        command: (db) => {
            db[collName].drop();
            assert.commandWorked(db[collName].insert({_id: 1, a: 1, b: 1}));
            assert.commandWorked(db[collName].createIndex({a: 1}));
            assert.commandWorked(db[collName].createIndex({b: 1}));

            assert(db[collName].findAndModify({query: {_id: 1}, update: {$inc: {a: 1}}}));
        },
        profileFilter: {op: 'command', 'command.findandmodify': collName},
        profileAssert: (db, profileDoc) => {
            // Should be the same as the corresponding "update" test with exception of
            // 'docUnitsReturned' field.
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, singleDocSize);
                assert.eq(profileDoc.docUnitsRead, 1);
                assert.eq(profileDoc.cursorSeeks, nSeeksForIdxHackPlans);
            } else {
                assert.gte(profileDoc.docBytesRead, singleDocSize);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, nSeeksForIdxHackPlans);
            }
            assert.eq(profileDoc.idxEntryBytesRead, idxEntrySize);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.lt(profileDoc.docBytesWritten, singleDocSize / 2);
            assert.eq(profileDoc.docUnitsWritten, 1);
            assert.eq(profileDoc.idxEntryBytesWritten, 2 * secondaryIndexEntrySize);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
            assert.eq(profileDoc.totalUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);

            assert.eq(profileDoc.docUnitsReturned, 1);
        }
    },
    {
        name: 'deleteIxScan-noSecondaryIndexes',
        command: (db) => {
            db[collName].drop();
            assert.commandWorked(db[collName].insert({_id: 1, a: 1, b: 1}));
            assert.commandWorked(db[collName].insert({_id: 2, a: 2, b: 2}));

            assert.commandWorked(db[collName].remove({_id: 1}));
        },
        profileFilter: {op: 'remove', 'command.q': {_id: 1}},
        profileAssert: (db, profileDoc) => {
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, singleDocSize);
                assert.eq(profileDoc.docUnitsRead, 1);
                // Not sure what the extra seek is from. The test below shows that, unlike update,
                // the unique secondary indexes don't generate additional seeks.
                assert.eq(profileDoc.cursorSeeks, nSeeksForIdxHackPlans + 1);
            } else {
                assert.gte(profileDoc.docBytesRead, singleDocSize);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, nSeeksForIdxHackPlans + 1);
            }
            assert.eq(profileDoc.idxEntryBytesRead, idxEntrySize);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);

            // Deleted bytes are counted as 'written'.
            assert.eq(profileDoc.docBytesWritten, singleDocSize);
            assert.eq(profileDoc.docUnitsWritten, 1);

            // Update the index on '_id'.
            assert.eq(profileDoc.idxEntryBytesWritten, idxEntrySize);
            assert.eq(profileDoc.idxEntryUnitsWritten, 1);

            assert.eq(profileDoc.totalUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
        }
    },
    {
        name: 'deleteIxScan-singleSecondaryIndex',
        command: (db) => {
            db[collName].drop();
            assert.commandWorked(db[collName].insert({_id: 1, a: 1, b: 1}));
            assert.commandWorked(db[collName].insert({_id: 2, a: 2, b: 2}));
            assert.commandWorked(db[collName].createIndex({a: 1}));

            assert.commandWorked(db[collName].remove({_id: 1}));
        },
        profileFilter: {op: 'remove', 'command.q': {_id: 1}},
        profileAssert: (db, profileDoc) => {
            // Updated the indexes on '_id' and 'a'
            assert.eq(profileDoc.idxEntryBytesWritten, idxEntrySize + secondaryIndexEntrySize);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
        }
    },
    {
        name: 'deleteIxScan-twoSecondaryIndexes',
        command: (db) => {
            db[collName].drop();
            assert.commandWorked(db[collName].insert({_id: 1, a: 1, b: 1}));
            assert.commandWorked(db[collName].insert({_id: 2, a: 2, b: 2}));
            assert.commandWorked(db[collName].createIndex({a: 1}, {unique: true}));
            assert.commandWorked(db[collName].createIndex({b: 1}));

            assert.commandWorked(db[collName].remove({_id: 1}));
        },
        profileFilter: {op: 'remove', 'command.q': {_id: 1}},
        profileAssert: (db, profileDoc) => {
            if (!isDebugBuild(db)) {
                // A unique secondary index doesn't generate an extra seek so it's still "+1".
                assert.eq(profileDoc.cursorSeeks, nSeeksForIdxHackPlans + 1);
            } else {
                assert.gte(profileDoc.cursorSeeks, nSeeksForIdxHackPlans + 1);
            }

            // Updated the indexes on '_id', 'a' and 'b'
            assert.eq(profileDoc.idxEntryBytesWritten, idxEntrySize + 2 * secondaryIndexEntrySize);
            assert.eq(profileDoc.idxEntryUnitsWritten, 3);
        }
    },
    {
        name: 'findAndModifyRemove',
        command: (db) => {
            db[collName].drop();
            assert.commandWorked(db[collName].insert({_id: 1, a: 1, b: 1}));
            assert.commandWorked(db[collName].insert({_id: 2, a: 2, b: 2}));
            assert.commandWorked(db[collName].createIndex({a: 1}));

            assert(db[collName].findAndModify({query: {_id: 1}, remove: true}));
        },
        profileFilter: {op: 'command', 'command.findandmodify': collName},
        profileAssert: (db, profileDoc) => {
            // Should be the same as the corresponding "delete" test with exception of
            // 'docUnitsReturned' field.
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, singleDocSize);
                assert.eq(profileDoc.docUnitsRead, 1);
                assert.eq(profileDoc.cursorSeeks, nSeeksForIdxHackPlans + 1);
            } else {
                assert.gte(profileDoc.docBytesRead, singleDocSize);
                assert.gte(profileDoc.docUnitsRead, 1);
                assert.gte(profileDoc.cursorSeeks, nSeeksForIdxHackPlans + 1);
            }
            assert.eq(profileDoc.idxEntryBytesRead, idxEntrySize);
            assert.eq(profileDoc.idxEntryUnitsRead, 1);
            assert.eq(profileDoc.docBytesWritten, singleDocSize);
            assert.eq(profileDoc.docUnitsWritten, 1);
            assert.eq(profileDoc.idxEntryBytesWritten, idxEntrySize + secondaryIndexEntrySize);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
            assert.eq(profileDoc.totalUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);

            assert.eq(profileDoc.docUnitsReturned, 1);
        }
    },
    {
        name: 'deleteCollScan',
        command: (db) => {
            db[collName].drop();
            assert.commandWorked(db[collName].insert({_id: 1, a: 1, b: 1}));
            assert.commandWorked(db[collName].insert({_id: 2, a: 2, b: 2}));

            assert.commandWorked(db[collName].remove({a: 2}));
        },
        profileFilter: {op: 'remove', 'command.q': {a: 2}},
        profileAssert: (db, profileDoc) => {
            // Should be the same as the corresponding 'delete' test with exception of idx and doc
            // reads.
            if (!isDebugBuild(db)) {
                assert.eq(profileDoc.docBytesRead, singleDocSize * 2);  // the target doc is second
                assert.eq(profileDoc.docUnitsRead, 2);
                assert.eq(profileDoc.cursorSeeks,
                          1);  // the same "mystery" seek as in idxhack tests
            } else {
                assert.gte(profileDoc.docBytesRead, singleDocSize * 2);
                assert.gte(profileDoc.docUnitsRead, 2);
                assert.gte(profileDoc.cursorSeeks, 1);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);

            // Deleted bytes are counted as 'written'.
            assert.eq(profileDoc.docBytesWritten, singleDocSize);
            assert.eq(profileDoc.docUnitsWritten, 1);

            // Updated the index on '_id'.
            assert.eq(profileDoc.idxEntryBytesWritten, idxEntrySize);
            assert.eq(profileDoc.idxEntryUnitsWritten, 1);

            assert.eq(profileDoc.totalUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
        }
    },

    //
    // Other tests. The order of operations is probably important.
    //
    resetProfileColl,
    resetTestColl,
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
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 5);
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
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 150);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 10);
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
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
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
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 1);
            assert.eq(profileDoc.docBytesWritten, 29);
            assert.eq(profileDoc.docUnitsWritten, 1);
            // Deletes one entry and writes another.
            assert.eq(profileDoc.idxEntryBytesWritten, 10);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
            assert.eq(profileDoc.totalUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
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
            assert.eq(profileDoc.totalUnitsWritten, 1);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
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
            // This is 102 instead of 100 because all of the index bytes for the batch insert are
            // lumped together and associated with the last document written in the batch, instead
            // of being associated with each document written.  This causes the last document+index
            // bytes to exceed the unit size.
            assert.eq(profileDoc.totalUnitsWritten, 102);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
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
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 100);
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
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 100);
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
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 100);
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
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 1);
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
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 5);
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
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 201);
            assert.eq(profileDoc.docUnitsReturned, 100);
        },
    },
    resetProfileColl,
    {
        name: 'groupStageAllowDiskUse',
        command: (db) => {
            // There should be 10 distinct values for 'a'.
            let cur = db[collName].aggregate([{$group: {_id: "$a", count: {$sum: 1}}}],
                                             {allowDiskUse: true});
            assert.eq(cur.itcount(), 10);
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (db, profileDoc) => {
            // In debug builds we spill artificially in order to exercise the query execution
            // engine's spilling logic. For $group, we incorporate the number of items spilled into
            // "keysSorted" and the number of individual spill events into "sorterSpills".
            if (isDebugBuild(db)) {
                assert.gt(profileDoc.keysSorted, 0);
                assert.gt(profileDoc.sorterSpills, 0);
            } else {
                assert.eq(profileDoc.keysSorted, 0);
                assert.eq(profileDoc.sorterSpills, 0);
            }

            // TODO SERVER-71684: We currently erroneously account for reads from and writes to
            // temporary record stores used as spill tables. This test accommodates the erroneous
            // behavior. Such accommodation is only necessary for debug builds (where we spill
            // artificially for test purposes), and when SBE is used. The classic engine spills to
            // files outside the storage engine rather than to a temporary record store, so it is
            // not subject to SERVER-71684.
            if (isDebugBuild(db) && checkSBEEnabled(db)) {
                assert.gt(profileDoc.docBytesWritten, 0);
                assert.gt(profileDoc.docUnitsWritten, 0);
                assert.gt(profileDoc.totalUnitsWritten, 0);
                assert.eq(profileDoc.totalUnitsWritten, profileDoc.docUnitsWritten);
                assert.eq(profileDoc.docBytesRead, 29 * 100 + profileDoc.docBytesWritten);
                assert.eq(profileDoc.docUnitsRead, 100 + profileDoc.docUnitsWritten);
            } else {
                assert.eq(profileDoc.docBytesRead, 29 * 100);
                assert.eq(profileDoc.docUnitsRead, 100);
                assert.eq(profileDoc.docBytesWritten, 0);
                assert.eq(profileDoc.docUnitsWritten, 0);
                assert.eq(profileDoc.totalUnitsWritten, 0);
            }

            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.docUnitsReturned, 10);
        },
    },
    resetProfileColl,
    {
        name: 'groupStageDisallowDiskUse',
        command: (db) => {
            // There should be 10 distinct values for 'a'.
            let cur = db[collName].aggregate([{$group: {_id: "$a", count: {$sum: 1}}}],
                                             {allowDiskUse: false});
            assert.eq(cur.itcount(), 10);
        },
        profileFilter: {op: 'command', 'command.aggregate': collName},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docBytesRead, 29 * 100);
            assert.eq(profileDoc.docUnitsRead, 100);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.docUnitsReturned, 10);
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
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.keysSorted, 100);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 10);
        },
    },
    resetProfileColl,
    {
        name: '$out',
        command: (db) => {
            db[collName].aggregate([{$out: {db: db.getName(), coll: 'outColl'}}]);
        },
        profileFilter: {'command.aggregate': collName},
        profileAssert: (db, profileDoc) => {
            // Creating a new collection writes to the durable catalog
            assert.gte(profileDoc.docBytesRead, 29 * 100);
            assert.gte(profileDoc.docUnitsRead, 100);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.gte(profileDoc.cursorSeeks, 0);
            assert.gte(profileDoc.docBytesWritten, 29 * 100);
            assert.gte(profileDoc.docUnitsWritten, 100);
            // The key size varies from 2 to 3 bytes.
            assert.gte(profileDoc.idxEntryBytesWritten, 2 * 100);
            assert.eq(profileDoc.idxEntryUnitsWritten, 100);
            assert.gte(profileDoc.totalUnitsWritten, 100);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
        },
    },
    resetProfileColl,
    {
        name: '$merge',
        command: (db) => {
            db['outColl'].drop();
            db[collName].aggregate([{$merge: {into: 'outColl'}}]);
        },
        profileFilter: {'command.aggregate': collName},
        profileAssert: (db, profileDoc) => {
            // Creating a new collection writes to the durable catalog
            assert.gte(profileDoc.docBytesRead, 29 * 100);
            assert.gte(profileDoc.docUnitsRead, 100);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.gte(profileDoc.cursorSeeks, 0);
            assert.gte(profileDoc.docBytesWritten, 29 * 100);
            assert.gte(profileDoc.docUnitsWritten, 100);
            // The key size varies from 2 to 3 bytes.
            assert.gte(profileDoc.idxEntryBytesWritten, 2 * 100);
            assert.eq(profileDoc.idxEntryUnitsWritten, 100);
            assert.gte(profileDoc.totalUnitsWritten, 100);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
            assert.eq(profileDoc.docUnitsReturned, 0);
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
            assert.eq(profileDoc.totalUnitsWritten, 1);
            assert.eq(profileDoc.docUnitsReturned, 0);
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
            db.capped.insertMany(docs, {ordered: false});
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
            assert.eq(profileDoc.totalUnitsWritten, 9);
            assert.eq(profileDoc.docUnitsReturned, 0);
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
                // Capped deletes will read two documents. The first is the document to be deleted
                // and the next is to cache the RecordId of the next document.
                // Debug builds may perform extra reads of the _mdb_catalog.
                assert.eq(profileDoc.docBytesRead, 58);
                assert.eq(profileDoc.docUnitsRead, 2);
                assert.eq(profileDoc.cursorSeeks, 1);
            } else {
                assert.gte(profileDoc.docBytesRead, 58);
                assert.gte(profileDoc.docUnitsRead, 2);
                assert.gte(profileDoc.cursorSeeks, 1);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 58);
            assert.eq(profileDoc.docUnitsWritten, 2);
            assert.eq(profileDoc.idxEntryBytesWritten, 5);
            assert.eq(profileDoc.idxEntryUnitsWritten, 2);
            assert.eq(profileDoc.totalUnitsWritten, 2);
            assert.eq(profileDoc.docUnitsReturned, 0);
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
            db.capped.insertMany(docs, {ordered: false});
            assert.eq(db.capped.find({a: 9}).itcount(), 0);
            assert.eq(db.capped.find({a: 10}).itcount(), 1);
        },
        profileFilter: {op: 'insert', 'command.insert': 'capped'},
        profileAssert: (db, profileDoc) => {
            if (!isDebugBuild(db)) {
                // Capped deletes will read two documents. The first is the document to be deleted
                // and the next is to cache the RecordId of the next document.
                // Debug builds may perform extra reads of the _mdb_catalog.
                assert.eq(profileDoc.docBytesRead, 522);
                assert.eq(profileDoc.docUnitsRead, 18);
                assert.eq(profileDoc.cursorSeeks, 18);
            } else {
                assert.gte(profileDoc.docBytesRead, 522);
                assert.gte(profileDoc.docUnitsRead, 18);
                assert.gte(profileDoc.cursorSeeks, 18);
            }
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 522);
            assert.eq(profileDoc.docUnitsWritten, 18);
            assert.eq(profileDoc.idxEntryBytesWritten, 54);
            assert.eq(profileDoc.idxEntryUnitsWritten, 18);
            assert.eq(profileDoc.totalUnitsWritten, 18);
            assert.eq(profileDoc.docUnitsReturned, 0);
        }
    },
    resetProfileColl,
    {
        name: 'createTimeseries',
        command: (db) => {
            assert.commandWorked(
                db.createCollection('ts', {timeseries: {timeField: 't', metaField: 'host'}}));
        },
        profileFilter: {op: 'command', 'command.create': 'ts'},
        profileAssert: (db, profileDoc) => {
            // The size of the collection document in the _mdb_catalog may not be the same every
            // test run, so only assert this is non-zero.
            assert.gt(profileDoc.docBytesRead, 0);
            assert.gt(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.gte(profileDoc.docBytesWritten, 0);
            assert.gte(profileDoc.docUnitsWritten, 0);
            assert.gte(profileDoc.idxEntryBytesWritten, 0);
            assert.gte(profileDoc.idxEntryUnitsWritten, 0);
            assert.gte(profileDoc.totalUnitsWritten, 0);
            assert.gt(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'insertTimeseriesNewBucketOrdered',
        command: (db) => {
            // Inserts a document that creates a new bucket.
            assert.commandWorked(db.ts.insert({t: new Date(), host: 0}, {ordered: true}));
        },
        profileFilter: {op: 'insert', 'command.insert': 'ts', 'command.ordered': true},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 207);
            assert.eq(profileDoc.docUnitsWritten, 2);
            if (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db)) {
                assert.eq(profileDoc.idxEntryBytesWritten, 34);
                assert.eq(profileDoc.idxEntryUnitsWritten, 3);
            } else {
                assert.eq(profileDoc.idxEntryBytesWritten, 0);
                assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            }
            assert.eq(profileDoc.totalUnitsWritten, 2);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'insertTimeseriesNewBucketUnordered',
        command: (db) => {
            // Inserts a document that creates a new bucket.
            assert.commandWorked(db.ts.insert({t: new Date(), host: 1}, {ordered: false}));
        },
        profileFilter: {op: 'insert', 'command.insert': 'ts', 'command.ordered': false},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 0);
            assert.eq(profileDoc.docUnitsRead, 0);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 207);
            assert.eq(profileDoc.docUnitsWritten, 2);
            if (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db)) {
                assert.eq(profileDoc.idxEntryBytesWritten, 35);
                assert.eq(profileDoc.idxEntryUnitsWritten, 3);
            } else {
                assert.eq(profileDoc.idxEntryBytesWritten, 0);
                assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            }
            assert.eq(profileDoc.cursorSeeks,
                      (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db)) ? 1 : 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    resetProfileColl,
    {
        name: 'timeseriesUpdateBucketOrdered',
        command: (db) => {
            // Inserts a document by updating an existing bucket.
            assert.commandWorked(db.ts.insert({t: new Date(), host: 0}, {ordered: true}));
        },
        profileFilter: {op: 'insert', 'command.insert': 'ts', 'command.ordered': true},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 207);
            assert.eq(profileDoc.docUnitsRead, 2);
            assert.eq(profileDoc.cursorSeeks, 2);
            assert.eq(profileDoc.docBytesWritten, 233);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docUnitsWritten, 2);
            if (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db)) {
                assert.eq(profileDoc.idxEntryBytesWritten, 68);
                assert.eq(profileDoc.idxEntryUnitsWritten, 6);
                assert.eq(profileDoc.totalUnitsWritten, 3);
            } else {
                assert.eq(profileDoc.idxEntryBytesWritten, 0);
                assert.eq(profileDoc.idxEntryUnitsWritten, 0);
                assert.eq(profileDoc.totalUnitsWritten, 2);
            }
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'timeseriesUpdateBucketUnordered',
        command: (db) => {
            // Inserts a document by updating an existing bucket.
            assert.commandWorked(db.ts.insert({t: new Date(), host: 1}, {ordered: false}));
        },
        profileFilter: {op: 'insert', 'command.insert': 'ts', 'command.ordered': false},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 207);
            assert.eq(profileDoc.docUnitsRead, 2);
            assert.eq(profileDoc.cursorSeeks, 2);
            assert.eq(profileDoc.docBytesWritten, 233);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docUnitsWritten, 2);
            if (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db)) {
                assert.eq(profileDoc.idxEntryBytesWritten, 70);
                assert.eq(profileDoc.idxEntryUnitsWritten, 6);
            } else {
                assert.eq(profileDoc.idxEntryBytesWritten, 0);
                assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            }
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
    {
        name: 'timeseriesQuery',
        command: (db) => {
            assert.eq(4, db.ts.find().itcount());
        },
        profileFilter: {op: 'query', 'command.find': 'ts'},
        profileAssert: (db, profileDoc) => {
            assert.eq(profileDoc.docBytesRead, 466);
            assert.eq(profileDoc.docUnitsRead, 4);
            assert.eq(profileDoc.idxEntryBytesRead, 0);
            assert.eq(profileDoc.idxEntryUnitsRead, 0);
            assert.eq(profileDoc.docBytesWritten, 0);
            assert.eq(profileDoc.docUnitsWritten, 0);
            assert.eq(profileDoc.idxEntryBytesWritten, 0);
            assert.eq(profileDoc.idxEntryUnitsWritten, 0);
            assert.eq(profileDoc.totalUnitsWritten, 0);
            assert.eq(profileDoc.cursorSeeks, 0);
            assert.eq(profileDoc.keysSorted, 0);
            assert.eq(profileDoc.sorterSpills, 0);
        }
    },
];

const testOperation = (db, operation) => {
    if (operation.skipTest && operation.skipTest(db)) {
        jsTestLog("skipping test case: " + operation.name);
        return;
    }

    jsTestLog("Testing operation: " + operation.name);
    operation.command(db);
    if (!operation.profileFilter) {
        return;
    }

    const profileColl = db.system.profile;
    const cursor = profileColl.find(operation.profileFilter).sort({$natural: -1}).limit(1);
    assert(cursor.hasNext(), () => {
        // Get the last operation that was not a find on the profile collection.
        const lastOp =
            profileColl.find({'command.find': {$ne: 'system.profile'}}).sort({$natural: -1}).next();
        return "Could not find operation in profiler with filter: " +
            tojson(operation.profileFilter) +
            ". Last operation in profile collection is: " + tojson(lastOp);
    });
    const entry = cursor.next();

    if (operation.profileAssert) {
        try {
            assertMetricsExist(entry);
            operation.profileAssert(db, entry.operationMetrics);
        } catch (e) {
            print("Caught exception while checking profile entry for '" + operation.name +
                  "' : " + tojson(entry));
            throw e;
        }
    }
};

const setParams = {
    profileOperationResourceConsumptionMetrics: true,
    internalQueryExecYieldPeriodMS: 5000
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
