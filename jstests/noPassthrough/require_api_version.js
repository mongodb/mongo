/**
 * Tests the "requireApiVersion" mongod/mongos parameter.
 *
 * This test is incompatible with parallel and passthrough suites; concurrent jobs fail while
 * requireApiVersion is true.
 *
 * @tags: [requires_journaling, requires_replication, requires_transactions]
 */

(function() {
"use strict";

function runTest(db, isMongos) {
    assert.commandWorked(db.runCommand({setParameter: 1, requireApiVersion: true}));
    assert.commandFailedWithCode(db.runCommand({ping: 1}), 498870, "command without apiVersion");
    assert.commandWorked(db.runCommand({ping: 1, apiVersion: "1"}));
    assert.commandFailed(db.runCommand({ping: 1, apiVersion: "not a real API version"}));

    /*
     * "getMore" never accepts or requires apiVersion.
     */
    assert.commandWorked(
        db.runCommand({insert: "collection", documents: [{}, {}, {}], apiVersion: "1"}));
    let reply = db.runCommand({find: "collection", batchSize: 1, apiVersion: "1"});
    assert.commandWorked(reply);
    assert.commandWorked(db.runCommand({getMore: reply.cursor.id, collection: "collection"}));

    /*
     * Transaction-starting commands must have apiVersion, transaction-continuing commands must not.
     */
    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(db.getName());
    reply = sessionDb.runCommand({
        find: "collection",
        batchSize: 1,
        apiVersion: "1",
        txnNumber: NumberLong(0),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    });
    assert.commandWorked(reply);
    assert.commandWorked(sessionDb.runCommand({
        getMore: reply.cursor.id,
        collection: "collection",
        txnNumber: NumberLong(0),
        stmtId: NumberInt(1),
        autocommit: false
    }));
    assert.commandWorked(sessionDb.runCommand({
        find: "collection",
        batchSize: 1,
        txnNumber: NumberLong(0),
        stmtId: NumberInt(2),
        autocommit: false
    }));
    const commitTxnWithApiVersionErrorCode = isMongos ? 4937702 : 4937700;
    assert.commandFailedWithCode(
        sessionDb.runCommand(
            {commitTransaction: 1, apiVersion: "1", txnNumber: NumberLong(0), autocommit: false}),
        commitTxnWithApiVersionErrorCode);
    assert.commandWorked(
        sessionDb.runCommand({commitTransaction: 1, txnNumber: NumberLong(0), autocommit: false}));

    // Start a new txn so we can test abortTransaction.
    reply = sessionDb.runCommand({
        find: "collection",
        apiVersion: "1",
        txnNumber: NumberLong(1),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    });
    assert.commandWorked(reply);
    const abortTxnWithApiVersionErrorCode = isMongos ? 4937701 : 4937700;
    assert.commandFailedWithCode(
        sessionDb.runCommand(
            {abortTransaction: 1, apiVersion: "1", txnNumber: NumberLong(1), autocommit: false}),
        abortTxnWithApiVersionErrorCode);
    assert.commandWorked(
        sessionDb.runCommand({abortTransaction: 1, txnNumber: NumberLong(1), autocommit: false}));

    assert.commandWorked(
        db.runCommand({setParameter: 1, requireApiVersion: false, apiVersion: "1"}));
    assert.commandWorked(db.runCommand({ping: 1}));
}

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
runTest(rst.getPrimary().getDB("admin"), false /* isMongos */);
rst.stopSet();

const st = new ShardingTest({});
runTest(st.s0.getDB("admin"), true /* isMongos */);
st.stop();
}());
