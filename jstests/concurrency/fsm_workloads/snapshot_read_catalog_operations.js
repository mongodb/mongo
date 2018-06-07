'use strict';

/**
 * Perform snapshot reads that span a find and a getmore concurrently with CRUD operations. The
 * snapshot reads and CRUD operations will all contend for locks on db and collName. Since the
 * snapshot read does not release its locks until the transaction is committed, it is expected that
 * once the read has begun, catalog operations with conflicting locks will block until the read is
 * finished. Additionally, index operations running concurrently with the snapshot read may cause
 * the read to fail with a SnapshotUnavailable error.
 *
 * @tags: [uses_transactions]
 */

load('jstests/concurrency/fsm_workload_helpers/snapshot_read_utils.js');
var $config = (function() {
    const data = {numIds: 100, numDocsToInsertPerThread: 5, valueToBeInserted: 1, batchSize: 50};

    const states = {
        init: function init(db, collName) {
            this.session = db.getMongo().startSession({causalConsistency: false});
            this.sessionDb = this.session.getDatabase(db.getName());
            this.txnNumber = 0;
            this.stmtId = 0;
        },

        snapshotFind: function snapshotFind(db, collName) {
            // The ascending snapshot find order is more likely to include documents read, updated,
            // and deleted by readDocs, updateDocs, and deleteDocs.
            // The descending snapshot find order is more likely to include documents inserted by
            // insertDocs.
            const sortOptions = [true, false];
            const sortByAscending = sortOptions[Random.randInt(2)];
            const readErrorCodes = [
                ErrorCodes.NoSuchTransaction,
                ErrorCodes.SnapshotUnavailable,
                ErrorCodes.LockTimeout
            ];
            const commitTransactionErrorCodes = readErrorCodes;
            doSnapshotFind(sortByAscending, collName, this, readErrorCodes);
            if (this.cursorId) {
                doSnapshotGetMore(collName, this, readErrorCodes, commitTransactionErrorCodes);
            }
        },

        insertDocs: function insertDocs(db, collName) {
            for (let i = 0; i < this.numDocsToInsertPerThread; ++i) {
                const res = db[collName].insert({value: this.valueToBeInserted});
                assertWhenOwnColl.writeOK(res);
                assertWhenOwnColl.eq(1, res.nInserted);
            }
        },

        updateDocs: function updateDocs(db, collName) {
            for (let i = 0; i < this.numIds; ++i) {
                try {
                    db[collName].update({_id: i}, {$inc: {value: 1}});
                } catch (e) {
                    // dropIndex can cause queries to throw if these queries yield.
                    assertAlways.contains(e.code,
                                          [ErrorCodes.QueryPlanKilled, ErrorCodes.OperationFailed],
                                          'unexpected error code: ' + e.code + ': ' + e.message);
                }
            }
        },

        readDocs: function readDocs(db, collName) {
            for (let i = 0; i < this.numIds; ++i) {
                try {
                    db[collName].findOne({_id: i});
                } catch (e) {
                    // dropIndex can cause queries to throw if these queries yield.
                    assertAlways.contains(e.code,
                                          [ErrorCodes.QueryPlanKilled, ErrorCodes.OperationFailed],
                                          'unexpected error code: ' + e.code + ': ' + e.message);
                }
            }
        },

        deleteDocs: function deleteDocs(db, collName) {
            let indexToDelete = Math.floor(Math.random() * this.numIds);
            try {
                db[collName].deleteOne({_id: indexToDelete});
            } catch (e) {
                // dropIndex can cause queries to throw if these queries yield.
                assertAlways.contains(e.code,
                                      [ErrorCodes.QueryPlanKilled, ErrorCodes.OperationFailed],
                                      'unexpected error code: ' + e.code + ': ' + e.message);
            }
        },

        createIndex: function createIndex(db, collName) {
            db[collName].createIndex({value: 1}, {background: true});
        },

        dropIndex: function dropIndex(db, collName) {
            db[collName].dropIndex({value: 1});
        }
    };

    const transitions = {
        init: {
            snapshotFind: 0.15,
            insertDocs: 0.14,
            updateDocs: 0.14,
            deleteDocs: 0.14,
            readDocs: 0.14,
            createIndex: 0.15,
            dropIndex: 0.14,
        },
        snapshotFind: {
            insertDocs: 0.17,
            updateDocs: 0.16,
            deleteDocs: 0.17,
            readDocs: 0.16,
            createIndex: 0.17,
            dropIndex: 0.17,
        },
        insertDocs: {snapshotFind: 1.0},
        updateDocs: {snapshotFind: 1.0},
        readDocs: {snapshotFind: 1.0},
        deleteDocs: {snapshotFind: 1.0},
        createIndex: {snapshotFind: 1.0},
        dropIndex: {snapshotFind: 1.0},
    };

    function setup(db, collName, cluster) {
        assertWhenOwnColl.commandWorked(db.runCommand({create: collName}));
        for (let i = 0; i < this.numIds; ++i) {
            const res = db[collName].insert({_id: i, value: this.valueToBeInserted});
            assert.writeOK(res);
            assert.eq(1, res.nInserted);
        }
    }

    return {
        threadCount: 5,
        iterations: 10,
        startState: 'init',
        states: states,
        transitions: transitions,
        setup: setup,
        data: data,
    };

})();
