/**
 * Verifies that a multi-document transaction aborts with WriteConflictError if an index build has
 * committed since the transaction's read snapshot.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const db = primary.getDB('test');

// Transaction inserting an index key.
{
    assert.commandWorked(db['c'].insertOne({_id: 0, num: 0}));

    const s0 = db.getMongo().startSession();
    s0.startTransaction();
    assert.commandWorked(s0.getDatabase('test')['c'].deleteOne({_id: 0}));
    s0.commitTransaction();

    const clusterTime = s0.getClusterTime().clusterTime;

    assert.commandWorked(db['c'].createIndex({num: 1}));

    // Start a transaction whose snapshot predates the completion of the index build, and which
    // reserves an oplog entry after the index build commits.
    try {
        const s1 = db.getMongo().startSession();
        s1.startTransaction({readConcern: {level: "snapshot", atClusterTime: clusterTime}});
        s1.getDatabase('test').c.insertOne({_id: 1, num: 1});

        // Transaction should have failed.
        assert(0);
    } catch (e) {
        assert(e.hasOwnProperty("errorLabels"), tojson(e));
        assert.contains("TransientTransactionError", e.errorLabels, tojson(e));
        assert.eq(e["code"], ErrorCodes.WriteConflict, tojson(e));
    }
}

db.c.drop();

// Transaction deleting an index key.
{
    assert.commandWorked(db.createCollection('c'));

    const s0 = db.getMongo().startSession();
    s0.startTransaction();
    assert.commandWorked(s0.getDatabase('test')['c'].insertOne({_id: 0, num: 0}));
    s0.commitTransaction();

    const clusterTime = s0.getClusterTime().clusterTime;

    assert.commandWorked(db['c'].createIndex({num: 1}));

    // Start a transaction whose snapshot predates the completion of the index build, and which
    // reserves an oplog entry after the index build commits.
    try {
        const s1 = db.getMongo().startSession();
        s1.startTransaction({readConcern: {level: "snapshot", atClusterTime: clusterTime}});
        s1.getDatabase('test').c.deleteOne({_id: 0});

        // Transaction should have failed.
        assert(0);
    } catch (e) {
        assert(e.hasOwnProperty("errorLabels"), tojson(e));
        assert.contains("TransientTransactionError", e.errorLabels, tojson(e));
        assert.eq(e["code"], ErrorCodes.WriteConflict, tojson(e));
    }
}

replTest.stopSet();
})();
