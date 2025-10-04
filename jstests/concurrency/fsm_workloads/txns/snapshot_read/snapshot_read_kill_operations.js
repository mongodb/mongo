/**
 * Test a snapshot read spanning a find and getmore that runs concurrently with killSessions,
 * killOp, killCursors, and txnNumber change.
 *
 * @tags: [
 *   uses_transactions,
 *   state_functions_share_transaction,
 *   requires_getmore,
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */

import {interruptedQueryErrors} from "jstests/concurrency/fsm_libs/assert.js";
import {abortTransaction, cleanupOnLastIteration} from "jstests/concurrency/fsm_workload_helpers/cleanup_txns.js";
import {
    doSnapshotFind,
    doSnapshotGetMore,
    insertSessionDoc,
    killSessionsFromDocs,
} from "jstests/concurrency/fsm_workload_helpers/snapshot_read_utils.js";

export const $config = (function () {
    const data = {numIds: 100, batchSize: 50};

    const states = {
        init: function init(db, collName) {
            let session = db.getMongo().startSession({causalConsistency: true});
            // Store the session ID in the database so any unterminated transactions can be aborted
            // at teardown.
            insertSessionDoc(db, collName, this.tid, session.getSessionId().id);
            this.sessionDb = session.getDatabase(db.getName());
            this.txnNumber = 0;
            this.stmtId = 0;
            this.iteration = 1;
        },

        snapshotFind: function snapshotFind(db, collName) {
            const sortByAscending = false;
            doSnapshotFind(sortByAscending, collName, this, [
                ErrorCodes.NoSuchTransaction,
                ErrorCodes.LockTimeout,
                ErrorCodes.Interrupted,
                ErrorCodes.SnapshotTooOld,
            ]);
        },

        snapshotGetMore: function snapshotGetMore(db, collName) {
            doSnapshotGetMore(
                collName,
                this,
                [...interruptedQueryErrors, ErrorCodes.LockTimeout, ErrorCodes.NoSuchTransaction],
                [
                    ErrorCodes.NoSuchTransaction,
                    ErrorCodes.Interrupted,
                    // Anonymous code for when user tries to send commit as the first
                    // operation in a transaction without sending a recovery token
                    50940,
                ],
            );
        },

        incrementTxnNumber: function incrementTxnNumber(db, collName) {
            abortTransaction(this.sessionDb, this.txnNumber);
            this.txnNumber++;
        },

        killSessions: function killSessions(db, collName) {
            // Kill a random active session.
            let idToKill = "sessionDoc" + Math.floor(Math.random() * this.threadCount);
            let sessionDocToKill = db[collName].findOne({"_id": idToKill});

            // Retry the find on idToKill in case the ID corresponds to a thread that has not
            // inserted its sessionDoc yet, and make sure we don't kill our own thread.
            while (!sessionDocToKill || idToKill == "sessionDoc" + this.tid) {
                idToKill = "sessionDoc" + Math.floor(Math.random() * this.threadCount);
                sessionDocToKill = db[collName].findOne({"_id": idToKill});
            }

            // This command may get interrupted by another thread's killSessions.
            assert.commandWorkedOrFailedWithCode(
                this.sessionDb.runCommand({killSessions: [{id: sessionDocToKill.id}]}),
                ErrorCodes.Interrupted,
            );
        },

        killOp: function killOp(db, collName) {
            // Find the object ID of the getMore in the snapshot read, if it is running, and attempt
            // to kill the operation. This command may get interrupted by another thread's
            // killSessions.
            const res = assert.commandWorkedOrFailedWithCode(
                this.sessionDb.adminCommand({
                    currentOp: 1,
                    ns: {$regex: db.getName() + "\." + collName},
                    op: "getmore",
                }),
                [ErrorCodes.CursorKilled, ErrorCodes.CursorNotFound, ErrorCodes.Interrupted],
            );
            if (res.hasOwnProperty("inprog") && res.inprog.length) {
                const killOpCmd = {killOp: 1, op: res.inprog[0].opid};
                const killRes = this.sessionDb.adminCommand(killOpCmd);
                assert.commandWorkedOrFailedWithCode(killRes, ErrorCodes.Interrupted);
            }
        },

        killCursors: function killCursors(db, collName) {
            const killCursorCmd = {killCursors: collName, cursors: [this.cursorId]};
            const res = this.sessionDb.runCommand(killCursorCmd);
            // This command may get interrupted by another thread's killSessions.
            assert.commandWorkedOrFailedWithCode(
                res,
                [ErrorCodes.CursorNotFound, ErrorCodes.Interrupted],
                () => `cmd: ${tojson(killCursorCmd)}`,
            );
        },
    };

    // Wrap each state in a cleanupOnLastIteration() invocation.
    for (let stateName of Object.keys(states)) {
        const stateFn = states[stateName];
        states[stateName] = function (db, collName) {
            cleanupOnLastIteration(this, () => stateFn.apply(this, arguments));
        };
    }

    const transitions = {
        init: {snapshotFind: 1.0},
        snapshotFind: {
            incrementTxnNumber: 0.2,
            killSessions: 0.2,
            killOp: 0.2,
            killCursors: 0.2,
            snapshotGetMore: 0.2,
        },
        incrementTxnNumber: {snapshotGetMore: 1.0},
        killSessions: {snapshotGetMore: 1.0},
        killOp: {snapshotGetMore: 1.0},
        killCursors: {snapshotGetMore: 1.0},
        snapshotGetMore: {snapshotFind: 1.0},
    };

    function setup(db, collName, cluster) {
        // The default WC is majority and this workload may not be able to satisfy majority writes.
        if (cluster.isSharded()) {
            cluster.executeOnMongosNodes(function (db) {
                assert.commandWorked(
                    db.adminCommand({
                        setDefaultRWConcern: 1,
                        defaultWriteConcern: {w: 1},
                        writeConcern: {w: "majority"},
                    }),
                );
            });
        } else if (cluster.isReplication()) {
            assert.commandWorked(
                db.adminCommand({
                    setDefaultRWConcern: 1,
                    defaultWriteConcern: {w: 1},
                    writeConcern: {w: "majority"},
                }),
            );
        }

        assert.commandWorked(db.runCommand({create: collName}));
        for (let i = 0; i < this.numIds; ++i) {
            const res = db[collName].insert({_id: i, value: i});
            assert.commandWorked(res);
            assert.eq(1, res.nInserted);
        }
    }

    function teardown(db, collName, cluster) {
        // Make sure any currently running transactions are aborted.
        killSessionsFromDocs(db, collName);

        // Unsetting CWWC is not allowed, so explicitly restore the default write concern to be
        // majority by setting CWWC to {w: majority}.
        if (cluster.isSharded()) {
            cluster.executeOnMongosNodes(function (db) {
                assert.commandWorked(
                    db.adminCommand({
                        setDefaultRWConcern: 1,
                        defaultWriteConcern: {w: "majority"},
                        writeConcern: {w: "majority"},
                    }),
                );
            });
        } else if (cluster.isReplication()) {
            assert.commandWorked(
                db.adminCommand({
                    setDefaultRWConcern: 1,
                    defaultWriteConcern: {w: "majority"},
                    writeConcern: {w: "majority"},
                }),
            );
        }
    }

    return {
        threadCount: 5,
        iterations: 10,
        startState: "init",
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data,
    };
})();
