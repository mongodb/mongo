'use strict';

/**
 * Test a snapshot read spanning a find and getmore that runs concurrently with killSessions,
 * killOp, killCursors, and txnNumber change.
 * @tags: [uses_transactions]
 */
load('jstests/concurrency/fsm_workload_helpers/snapshot_read_utils.js');
var $config = (function() {
    const data = {numIds: 100, batchSize: 50};
    const threadCount = 5;

    const states = {
        init: function init(db, collName) {
            let session = db.getMongo().startSession({causalConsistency: false});
            // Store the session ID in the database so any unterminated transactions can be aborted
            // at teardown.
            insertSessionDoc(db, collName, this, session);
            this.sessionDb = session.getDatabase(db.getName());
            this.txnNumber = 0;
            this.stmtId = 0;
        },

        snapshotFind: function snapshotFind(db, collName) {
            const sortByAscending = false;
            doSnapshotFind(sortByAscending, collName, this, [ErrorCodes.NoSuchTransaction]);
        },

        snapshotGetMore: function snapshotGetMore(db, collName) {
            doSnapshotGetMore(
                collName,
                this,
                [ErrorCodes.NoSuchTransaction, ErrorCodes.CursorNotFound, ErrorCodes.Interrupted],
                [ErrorCodes.NoSuchTransaction]);
        },

        incrementTxnNumber: function incrementTxnNumber(db, collName) {
            this.txnNumber++;
        },

        killSessions: function killSessions(db, collName) {
            // Kill a random active session.
            const idToKill = "sessionDoc" + Math.floor(Math.random() * threadCount);
            const sessionDocToKill = db[collName].find({"_id": idToKill});
            assert.commandWorked(
                this.sessionDb.runCommand({killSessions: [{id: sessionDocToKill.id}]}));
        },

        killOp: function killOp(db, collName) {
            // Find the object ID of the getMore in the snapshot read, if it is running, and attempt
            // to kill the operation.
            const res = assert.commandWorked(this.sessionDb.adminCommand(
                {currentOp: 1, ns: {$regex: db.getName() + "\." + collName}, op: "getmore"}));
            if (res.inprog.length) {
                const killOpCmd = {killOp: 1, op: res.inprog[0].opid};
                const killRes = this.sessionDb.adminCommand(killOpCmd);
                assert.commandWorked(killRes);
            }
        },

        killCursors: function killCursors(db, collName) {
            const killCursorCmd = {killCursors: collName, cursors: [this.cursorId]};
            const res = this.sessionDb.runCommand(killCursorCmd);
            assertWorkedOrFailed(killCursorCmd, res, [ErrorCodes.CursorNotFound]);
        }
    };

    const transitions = {
        init: {snapshotFind: 1.0},
        snapshotFind: {
            incrementTxnNumber: 0.20,
            killSessions: 0.20,
            killOp: 0.20,
            killCursors: 0.20,
            snapshotGetMore: 0.20
        },
        incrementTxnNumber: {snapshotGetMore: 1.0},
        killSessions: {snapshotGetMore: 1.0},
        killOp: {snapshotGetMore: 1.0},
        killCursors: {snapshotGetMore: 1.0},
        snapshotGetMore: {snapshotFind: 1.0}
    };

    function setup(db, collName, cluster) {
        assertWhenOwnColl.commandWorked(db.runCommand({create: collName}));
        for (let i = 0; i < this.numIds; ++i) {
            const res = db[collName].insert({_id: i, value: this.valueToBeIncremented});
            assert.writeOK(res);
            assert.eq(1, res.nInserted);
        }
    }

    function teardown(db, collName, cluster) {
        // Make sure any currently running transactions are aborted.
        killSessionsFromDocs(db, collName);
    }

    const skip = function skip(cluster) {
        // TODO(SERVER-34570) remove isSharded() check once transactions are supported in sharded
        // environments.
        if (cluster.isSharded() || cluster.isStandalone()) {
            return {skip: true, msg: 'only runs in a replica set.'};
        }
        return {skip: false};
    };

    return {
        threadCount: threadCount,
        iterations: 10,
        startState: 'init',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data,
        skip: skip
    };

})();
