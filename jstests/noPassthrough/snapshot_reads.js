// Tests snapshot isolation on readConcern level snapshot read.
// @tags: [requires_replication]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();

    function runTest({useCausalConsistency}) {
        const primaryDB = rst.getPrimary().getDB(dbName);
        primaryDB.coll.drop();

        const session =
            primaryDB.getMongo().startSession({causalConsistency: useCausalConsistency});
        const sessionDb = session.getDatabase(dbName);

        if (!primaryDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
            rst.stopSet();
            return;
        }

        const bulk = primaryDB.coll.initializeUnorderedBulkOp();
        for (let x = 0; x < 10; ++x) {
            bulk.insert({_id: x});
        }
        assert.commandWorked(bulk.execute({w: "majority"}));

        let txnNumber = 0;

        // Establish a snapshot cursor, fetching the first 5 documents.
        let res = assert.commandWorked(sessionDb.runCommand({
            find: collName,
            sort: {_id: 1},
            batchSize: 5,
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(txnNumber)
        }));

        assert(res.hasOwnProperty("cursor"));
        assert(res.cursor.hasOwnProperty("id"));
        const cursorId = res.cursor.id;

        // Insert an 11th document which should not be visible to the snapshot cursor. This write is
        // performed outside of the session.
        assert.writeOK(primaryDB.coll.insert({_id: 10}, {writeConcern: {w: "majority"}}));

        // Fetch the 6th document. This confirms that the transaction stash is preserved across
        // multiple getMore invocations.
        res = assert.commandWorked(sessionDb.runCommand({
            getMore: cursorId,
            collection: collName,
            batchSize: 1,
            txnNumber: NumberLong(txnNumber)
        }));
        assert(res.hasOwnProperty("cursor"));
        assert(res.cursor.hasOwnProperty("id"));
        assert.neq(0, res.cursor.id);

        // Exhaust the cursor, retrieving the remainder of the result set.
        res = assert.commandWorked(sessionDb.runCommand({
            getMore: cursorId,
            collection: collName,
            batchSize: 10,
            txnNumber: NumberLong(txnNumber++)
        }));

        // The cursor has been exhausted.
        assert(res.hasOwnProperty("cursor"));
        assert(res.cursor.hasOwnProperty("id"));
        assert.eq(0, res.cursor.id);

        // Only the remaining 4 of the initial 10 documents are returned. The 11th document is not
        // part of the result set.
        assert(res.cursor.hasOwnProperty("nextBatch"));
        assert.eq(4, res.cursor.nextBatch.length);

        // Perform a second snapshot read under a new transaction.
        res = assert.commandWorked(sessionDb.runCommand({
            find: collName,
            sort: {_id: 1},
            batchSize: 20,
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(txnNumber++)
        }));

        // The cursor has been exhausted.
        assert(res.hasOwnProperty("cursor"));
        assert(res.cursor.hasOwnProperty("id"));
        assert.eq(0, res.cursor.id);

        // All 11 documents are returned.
        assert(res.cursor.hasOwnProperty("firstBatch"));
        assert.eq(11, res.cursor.firstBatch.length);

        // Reject snapshot reads without txnNumber.
        assert.commandFailed(sessionDb.runCommand(
            {find: collName, sort: {_id: 1}, batchSize: 20, readConcern: {level: "snapshot"}}));

        // Reject snapshot reads without session.
        assert.commandFailed(primaryDB.runCommand({
            find: collName,
            sort: {_id: 1},
            batchSize: 20,
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(txnNumber++)
        }));

        session.endSession();
    }

    runTest({useCausalConsistency: false});
    runTest({useCausalConsistency: true});

    rst.stopSet();
})();
