// Tests snapshot isolation on readConcern level snapshot read.
// @tags: [uses_transactions]
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

    function parseCursor(cmdResult) {
        if (cmdResult.hasOwnProperty("cursor")) {
            assert(cmdResult.cursor.hasOwnProperty("id"));
            return cmdResult.cursor;
        } else if (cmdResult.hasOwnProperty("cursors") && cmdResult.cursors.length === 1 &&
                   cmdResult.cursors[0].hasOwnProperty("cursor")) {
            assert(cmdResult.cursors[0].cursor.hasOwnProperty("id"));
            return cmdResult.cursors[0].cursor;
        }

        throw Error("parseCursor failed to find cursor object. Command Result: " +
                    tojson(cmdResult));
    }

    function runTest({useCausalConsistency, establishCursorCmd}) {
        primaryDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

        const session =
            primaryDB.getMongo().startSession({causalConsistency: useCausalConsistency});
        const sessionDb = session.getDatabase(dbName);

        const bulk = primaryDB.coll.initializeUnorderedBulkOp();
        for (let x = 0; x < 10; ++x) {
            bulk.insert({_id: x});
        }
        assert.commandWorked(bulk.execute({w: "majority"}));

        session.startTransaction({readConcern: {level: "snapshot"}});

        // Establish a snapshot batchSize:0 cursor.
        let res = assert.commandWorked(sessionDb.runCommand(establishCursorCmd));
        let cursor = parseCursor(res);

        assert(cursor.hasOwnProperty("firstBatch"), tojson(res));
        assert.eq(0, cursor.firstBatch.length, tojson(res));
        assert.neq(cursor.id, 0);

        // Insert an 11th document which should not be visible to the snapshot cursor. This write is
        // performed outside of the session.
        assert.writeOK(primaryDB.coll.insert({_id: 10}, {writeConcern: {w: "majority"}}));

        // Fetch the first 5 documents.
        res = assert.commandWorked(
            sessionDb.runCommand({getMore: cursor.id, collection: collName, batchSize: 5}));
        cursor = parseCursor(res);
        assert.neq(0, cursor.id, tojson(res));
        assert(cursor.hasOwnProperty("nextBatch"), tojson(res));
        assert.eq(5, cursor.nextBatch.length, tojson(res));

        // Exhaust the cursor, retrieving the remainder of the result set. Performing a second
        // getMore tests snapshot isolation across multiple getMore invocations.
        res = assert.commandWorked(
            sessionDb.runCommand({getMore: cursor.id, collection: collName, batchSize: 20}));
        session.commitTransaction();

        // The cursor has been exhausted.
        cursor = parseCursor(res);
        assert.eq(0, cursor.id, tojson(res));

        // Only the remaining 5 of the initial 10 documents are returned. The 11th document is not
        // part of the result set.
        assert(cursor.hasOwnProperty("nextBatch"), tojson(res));
        assert.eq(5, cursor.nextBatch.length, tojson(res));

        // Perform a second snapshot read under a new transaction.
        session.startTransaction({readConcern: {level: "snapshot"}});
        res = assert.commandWorked(
            sessionDb.runCommand({find: collName, sort: {_id: 1}, batchSize: 20}));
        session.commitTransaction();

        // The cursor has been exhausted.
        cursor = parseCursor(res);
        assert.eq(0, cursor.id, tojson(res));

        // All 11 documents are returned.
        assert(cursor.hasOwnProperty("firstBatch"), tojson(res));
        assert.eq(11, cursor.firstBatch.length, tojson(res));

        session.endSession();
    }

    // Test snapshot reads using find.
    let findCmd = {find: collName, sort: {_id: 1}, batchSize: 0};
    runTest({useCausalConsistency: false, establishCursorCmd: findCmd});
    runTest({useCausalConsistency: true, establishCursorCmd: findCmd});

    // Test snapshot reads using aggregate.
    let aggCmd = {aggregate: collName, pipeline: [{$sort: {_id: 1}}], cursor: {batchSize: 0}};
    runTest({useCausalConsistency: false, establishCursorCmd: aggCmd});
    runTest({useCausalConsistency: true, establishCursorCmd: aggCmd});

    rst.stopSet();
})();
