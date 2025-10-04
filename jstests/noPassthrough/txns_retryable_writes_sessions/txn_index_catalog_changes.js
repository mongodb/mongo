/**
 * Verifies that a multi-document transaction aborts with WriteConflictError if an index build has
 * committed since the transaction's read snapshot.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const db = primary.getDB("test");

// Transaction inserting an index key.
{
    assert.commandWorked(db["c"].insertOne({_id: 0, num: 0}));

    const s0 = db.getMongo().startSession();
    s0.startTransaction();
    assert.commandWorked(s0.getDatabase("test")["c"].deleteOne({_id: 0}));
    s0.commitTransaction();

    const clusterTime = s0.getClusterTime().clusterTime;

    assert.commandWorked(db["c"].createIndex({num: 1}));

    // Start a transaction whose snapshot predates the completion of the index build, and which
    // reserves an oplog entry after the index build commits.
    const s1 = db.getMongo().startSession();
    s1.startTransaction({readConcern: {level: "snapshot", atClusterTime: clusterTime}});

    const res = assert.commandFailedWithCode(
        s1.getDatabase("test").c.insert({_id: 1, num: 1}),
        ErrorCodes.WriteConflict,
    );
    assert(res.hasOwnProperty("errorLabels"), tojson(res));
    assert.contains("TransientTransactionError", res.errorLabels, tojson(res));

    assert.commandFailedWithCode(s1.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
}

db.c.drop();

// Transaction deleting an index key.
{
    assert.commandWorked(db.createCollection("c"));

    const s0 = db.getMongo().startSession();
    s0.startTransaction();
    assert.commandWorked(s0.getDatabase("test")["c"].insertOne({_id: 0, num: 0}));
    s0.commitTransaction();

    const clusterTime = s0.getClusterTime().clusterTime;

    assert.commandWorked(db["c"].createIndex({num: 1}));

    // Start a transaction whose snapshot predates the completion of the index build, and which
    // reserves an oplog entry after the index build commits.
    const s1 = db.getMongo().startSession();
    s1.startTransaction({readConcern: {level: "snapshot", atClusterTime: clusterTime}});

    const res = assert.commandFailedWithCode(s1.getDatabase("test").c.remove({_id: 0}), ErrorCodes.WriteConflict);
    assert(res.hasOwnProperty("errorLabels"), tojson(res));
    assert.contains("TransientTransactionError", res.errorLabels, tojson(res));

    assert.commandFailedWithCode(s1.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
}

replTest.stopSet();
