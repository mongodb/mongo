// Tests snapshot isolation on readConcern level snapshot read.
// @tags: [requires_replication]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    let conf = rst.getReplSetConfig();
    conf.members[1].votes = 0;
    conf.members[1].priority = 0;
    rst.initiate(conf);

    const primaryDB = rst.getPrimary().getDB(dbName);
    if (!primaryDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }
    const secondaryDB = rst.getSecondary().getDB(dbName);

    function runTest({useCausalConsistency, readFromSecondary, establishCursorCmd}) {
        primaryDB.coll.drop();

        let readDB = primaryDB;
        if (readFromSecondary) {
            readDB = secondaryDB;
        }

        const session = readDB.getMongo().startSession({causalConsistency: useCausalConsistency});
        const sessionDb = session.getDatabase(dbName);

        const bulk = primaryDB.coll.initializeUnorderedBulkOp();
        for (let x = 0; x < 10; ++x) {
            bulk.insert({_id: x});
        }
        assert.commandWorked(bulk.execute({w: "majority"}));

        if (readFromSecondary) {
            rst.awaitLastOpCommitted();
        }

        let txnNumber = 0;

        // Augment the cursor-establishing command with the proper readConcern and transaction
        // number.
        let cursorCmd = Object.extend({}, establishCursorCmd);
        cursorCmd.readConcern = {level: "snapshot"};
        cursorCmd.txnNumber = NumberLong(txnNumber);

        // Establish a snapshot batchSize:0 cursor.
        let res = assert.commandWorked(sessionDb.runCommand(cursorCmd));

        assert(res.hasOwnProperty("cursor"), tojson(res));
        assert(res.cursor.hasOwnProperty("firstBatch"), tojson(res));
        assert.eq(0, res.cursor.firstBatch.length, tojson(res));

        assert(res.cursor.hasOwnProperty("id"), tojson(res));
        const cursorId = res.cursor.id;
        assert.neq(cursorId, 0);

        // Insert an 11th document which should not be visible to the snapshot cursor. This write is
        // performed outside of the session.
        assert.writeOK(primaryDB.coll.insert({_id: 10}, {writeConcern: {w: "majority"}}));

        // Fetch the first 5 documents.
        res = assert.commandWorked(sessionDb.runCommand({
            getMore: cursorId,
            collection: collName,
            batchSize: 5,
            txnNumber: NumberLong(txnNumber)
        }));
        assert(res.hasOwnProperty("cursor"), tojson(res));
        assert(res.cursor.hasOwnProperty("id"), tojson(res));
        assert.neq(0, res.cursor.id, tojson(res));
        assert(res.cursor.hasOwnProperty("nextBatch"), tojson(res));
        assert.eq(5, res.cursor.nextBatch.length, tojson(res));

        // Exhaust the cursor, retrieving the remainder of the result set. Performing a second
        // getMore tests snapshot isolation across multiple getMore invocations.
        res = assert.commandWorked(sessionDb.runCommand({
            getMore: cursorId,
            collection: collName,
            batchSize: 20,
            txnNumber: NumberLong(txnNumber++)
        }));

        // The cursor has been exhausted.
        assert(res.hasOwnProperty("cursor"), tojson(res));
        assert(res.cursor.hasOwnProperty("id"), tojson(res));
        assert.eq(0, res.cursor.id, tojson(res));

        // Only the remaining 5 of the initial 10 documents are returned. The 11th document is not
        // part of the result set.
        assert(res.cursor.hasOwnProperty("nextBatch"), tojson(res));
        assert.eq(5, res.cursor.nextBatch.length, tojson(res));

        if (readFromSecondary) {
            rst.awaitLastOpCommitted();
        }

        // Perform a second snapshot read under a new transaction.
        res = assert.commandWorked(sessionDb.runCommand({
            find: collName,
            sort: {_id: 1},
            batchSize: 20,
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(txnNumber++)
        }));

        // The cursor has been exhausted.
        assert(res.hasOwnProperty("cursor"), tojson(res));
        assert(res.cursor.hasOwnProperty("id"), tojson(res));
        assert.eq(0, res.cursor.id, tojson(res));

        // All 11 documents are returned.
        assert(res.cursor.hasOwnProperty("firstBatch"), tojson(res));
        assert.eq(11, res.cursor.firstBatch.length, tojson(res));

        // Reject snapshot reads without txnNumber.
        assert.commandFailed(sessionDb.runCommand(
            {find: collName, sort: {_id: 1}, batchSize: 20, readConcern: {level: "snapshot"}}));

        // Reject snapshot reads without session.
        assert.commandFailed(readDB.runCommand({
            find: collName,
            sort: {_id: 1},
            batchSize: 20,
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(txnNumber++)
        }));

        session.endSession();
    }

    // Test snapshot reads using find.
    let findCmd = {find: collName, sort: {_id: 1}, batchSize: 0};
    runTest({useCausalConsistency: false, readFromSecondary: false, establishCursorCmd: findCmd});
    runTest({useCausalConsistency: true, readFromSecondary: false, establishCursorCmd: findCmd});
    runTest({useCausalConsistency: false, readFromSecondary: true, establishCursorCmd: findCmd});
    runTest({useCausalConsistency: true, readFromSecondary: true, establishCursorCmd: findCmd});

    // Test snapshot reads using aggregate.
    let aggCmd = {aggregate: collName, pipeline: [{$sort: {_id: 1}}], cursor: {batchSize: 0}};
    runTest({useCausalConsistency: false, readFromSecondary: false, establishCursorCmd: aggCmd});
    runTest({useCausalConsistency: true, readFromSecondary: false, establishCursorCmd: aggCmd});
    runTest({useCausalConsistency: false, readFromSecondary: true, establishCursorCmd: aggCmd});
    runTest({useCausalConsistency: true, readFromSecondary: true, establishCursorCmd: aggCmd});

    rst.stopSet();
})();
