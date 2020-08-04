'use strict';

/**
 * Perform point-in-time snapshot reads that span a 'find' and multiple 'getmore's concurrently with
 * CRUD operations, after initial insert operations. This tests that the effects of concurrent CRUD
 * operations are not visible to the point-in-time snapshot reads. The initial inserted documents
 * (prior to the atClusterTime timestamp) are of the pattern:
 * {_id: (0-99), x:1}. The subsequent inserted documents have a generated ObjectId as _id. Document
 * updates increment the value of x. We test that the snapshot read only returns documents where _id
 * is between 0-99, and the value of x is always 1.
 *
 * @tags: [requires_fcv_47, requires_replication, does_not_support_causal_consistency,
 * requires_majority_read_concern]
 */

load('jstests/concurrency/fsm_workload_helpers/snapshot_read_utils.js');
var $config = (function() {
    const data = {numIds: 100, numDocsToInsertPerThread: 5, batchSize: 10};

    const states = {
        init: function init(db, collName) {
            this.atClusterTime = new Timestamp(this.clusterTime.t, this.clusterTime.i);
            jsTestLog("atClusterTime Timestamp: " + this.atClusterTime.toString());
            this.numDocScanned = 0;
        },

        snapshotScan: function snapshotScan(db, collName) {
            if (!this.cursorId || this.cursorId == 0) {
                doSnapshotFindAtClusterTime(
                    db, collName, this, [ErrorCodes.ShutdownInProgress], {_id: 1}, (res) => {
                        let expectedDocs =
                            [...Array(this.batchSize).keys()].map((i) => ({_id: i, x: 1}));
                        assert.eq(res.cursor.firstBatch, expectedDocs, () => tojson(res));
                        this.numDocScanned = this.batchSize;
                    });
            } else {
                doSnapshotGetMoreAtClusterTime(
                    db,
                    collName,
                    this,
                    [ErrorCodes.ShutdownInProgress, ErrorCodes.Interrupted],
                    (res) => {
                        let expectedDocs = [...Array(this.batchSize).keys()].map(
                            (i) => ({_id: i + this.numDocScanned, x: 1}));
                        assert.eq(res.cursor.nextBatch, expectedDocs, () => tojson(res));
                        this.numDocScanned = this.numDocScanned + this.batchSize;
                    });
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
                assert.commandWorked(db[collName].update({_id: i}, {$inc: {x: 1}}));
            }
        },

        readDocs: function readDocs(db, collName) {
            for (let i = 0; i < this.numIds; ++i) {
                db[collName].findOne({_id: i});
            }
        },

        deleteDocs: function deleteDocs(db, collName) {
            let indexToDelete = Math.floor(Math.random() * this.numIds);
            assert.commandWorked(db[collName].deleteOne({_id: indexToDelete}));
        },

        killOp: function killOp(db, collName) {
            // Find the object ID of the getMore in the snapshot read, if it is running, and attempt
            // to kill the operation.
            const res = assert.commandWorkedOrFailedWithCode(
                db.adminCommand(
                    {currentOp: 1, ns: {$regex: db.getName() + "\." + collName}, op: "getmore"}),
                [ErrorCodes.Interrupted]);
            if (res.hasOwnProperty("inprog") && res.inprog.length) {
                const killOpCmd = {killOp: 1, op: res.inprog[0].opid};
                const killRes = db.adminCommand(killOpCmd);
                assert.commandWorkedOrFailedWithCode(killRes, ErrorCodes.Interrupted);
            }
        },
    };

    const transitions = {
        init: {
            snapshotScan: 0.2,
            insertDocs: 0.2,
            updateDocs: 0.2,
            deleteDocs: 0.2,
            readDocs: 0.2,
        },
        snapshotScan:
            {insertDocs: 0.2, updateDocs: 0.2, deleteDocs: 0.2, readDocs: 0.2, killOp: 0.2},
        insertDocs: {snapshotScan: 1.0},
        updateDocs: {snapshotScan: 1.0},
        readDocs: {snapshotScan: 1.0},
        deleteDocs: {snapshotScan: 1.0},
        killOp: {snapshotScan: 1.0}
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
        const docs = [...Array(this.numIds).keys()].map((i) => ({_id: i, x: 1}));
        this.clusterTime =
            assert.commandWorked(db.runCommand({insert: collName, documents: docs})).operationTime;
    }

    function teardown(db, collName, cluster) {
        assertWhenOwnColl.commandWorked(db.runCommand({drop: collName}));
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
        startState: 'init',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data,
    };
})();
