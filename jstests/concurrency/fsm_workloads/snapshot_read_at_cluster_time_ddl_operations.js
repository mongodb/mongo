'use strict';

/**
 * Perform point-in-time snapshot reads that span a 'find' and multiple 'getmore's concurrently with
 * CRUD operations. Index operations running concurrently with the snapshot read may cause
 * the read to fail with a SnapshotUnavailable error.
 *
 * @tags: [creates_background_indexes, requires_fcv_47, requires_replication,
 * does_not_support_causal_consistency, requires_majority_read_concern]
 */

load('jstests/concurrency/fsm_workload_helpers/snapshot_read_utils.js');
var $config = (function() {
    const data = {numIds: 100, numDocsToInsertPerThread: 5, batchSize: 10};

    const states = {

        snapshotScan: function snapshotScan(db, collName) {
            const readErrorCodes = [
                ErrorCodes.SnapshotUnavailable,
                ErrorCodes.ShutdownInProgress,
                ErrorCodes.CursorNotFound,
                ErrorCodes.QueryPlanKilled,
            ];
            if (!this.cursorId || this.cursorId == 0) {
                doSnapshotFindAtClusterTime(db, collName, this, readErrorCodes, {a: 1});
            } else {
                doSnapshotGetMoreAtClusterTime(db, collName, this, readErrorCodes);
            }
        },

        insertDocs: function insertDocs(db, collName) {
            for (let i = 0; i < this.numDocsToInsertPerThread; ++i) {
                const res = db[collName].insert({x: 1});
                assertWhenOwnColl.commandWorked(res);
                assertWhenOwnColl.eq(1, res.nInserted);
            }
        },

        updateDocs: function updateDocs(db, collName) {
            for (let i = 0; i < this.numIds; ++i) {
                try {
                    db[collName].update({a: i}, {$inc: {x: 1}});
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
                    db[collName].findOne({a: i});
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
                db[collName].deleteOne({a: indexToDelete});
            } catch (e) {
                // dropIndex can cause queries to throw if these queries yield.
                assertAlways.contains(e.code,
                                      [ErrorCodes.QueryPlanKilled, ErrorCodes.OperationFailed],
                                      'unexpected error code: ' + e.code + ': ' + e.message);
            }
        },

        createIndex: function createIndex(db, collName) {
            db[collName].createIndex({a: 1}, {background: true});
        },

        dropIndex: function dropIndex(db, collName) {
            db[collName].dropIndex({a: 1});
        },

    };

    const transitions = {
        snapshotScan: {
            insertDocs: 0.17,
            updateDocs: 0.16,
            deleteDocs: 0.17,
            readDocs: 0.16,
            createIndex: 0.17,
            dropIndex: 0.17,
        },
        insertDocs: {snapshotScan: 1.0},
        updateDocs: {snapshotScan: 1.0},
        readDocs: {snapshotScan: 1.0},
        deleteDocs: {snapshotScan: 1.0},
        createIndex: {snapshotScan: 1.0},
        dropIndex: {snapshotScan: 1.0},
    };

    let minSnapshotHistoryWindowInSecondsDefault;

    function setup(db, collName, cluster) {
        // We temporarily increase the minimum snapshot history window to ensure point-in-time reads
        // at the initial insert timestamp are valid throughout the duration of this test.
        cluster.executeOnMongodNodes((db) => {
            const res = db.adminCommand({setParameter: 1, minSnapshotHistoryWindowInSeconds: 3600});
            assert.commandWorked(res);
            minSnapshotHistoryWindowInSecondsDefault = res.was;
        });
        // We modify chunk history to be larger on config nodes to ensure snapshot reads succeed for
        // sharded clusters.
        if (cluster.isSharded()) {
            cluster.executeOnConfigNodes((db) => {
                assert.commandWorked(
                    db.adminCommand({setParameter: 1, minSnapshotHistoryWindowInSeconds: 3600}));
            });
        }
        assertWhenOwnColl.commandWorked(db.runCommand({create: collName}));
        const docs = [...Array(this.numIds).keys()].map((i) => ({a: i, x: 1}));
        assert.commandWorked(db.runCommand({insert: collName, documents: docs}));
        assert.commandWorked(
            db.runCommand({createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}]}));
    }

    function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes(function(db) {
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                minSnapshotHistoryWindowInSeconds: minSnapshotHistoryWindowInSecondsDefault
            }));
        });
        if (cluster.isSharded()) {
            cluster.executeOnConfigNodes((db) => {
                assert.commandWorked(db.adminCommand({
                    setParameter: 1,
                    minSnapshotHistoryWindowInSeconds: minSnapshotHistoryWindowInSecondsDefault
                }));
            });
        }
    }

    return {
        threadCount: 5,
        iterations: 50,
        startState: 'snapshotScan',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data,
    };
})();
