// Test the awaitData flag for the find/getMore commands.
//
// @tags: [
//   # This test attempts to perform a getMore command and find it using the currentOp command. The
//   # former operation may be routed to a secondary in the replica set, whereas the latter must be
//   # routed to the primary.
//   assumes_read_preference_unchanged,
//   requires_getmore,
// ]

(function() {
    'use strict';

    load("jstests/libs/fixture_helpers.js");

    var cmdRes;
    var cursorId;
    var defaultBatchSize = 101;
    var collName = 'await_data';
    var coll = db[collName];

    // Create a non-capped collection with 10 documents.
    coll.drop();
    for (var i = 0; i < 10; i++) {
        assert.writeOK(coll.insert({a: i}));
    }

    // Find with tailable flag set should fail for a non-capped collection.
    cmdRes = db.runCommand({find: collName, tailable: true});
    assert.commandFailed(cmdRes);

    // Should also fail in the non-capped case if both the tailable and awaitData flags are set.
    cmdRes = db.runCommand({find: collName, tailable: true, awaitData: true});
    assert.commandFailed(cmdRes);

    // With a non-existent collection, should succeed but return no data and a closed cursor.
    coll.drop();
    cmdRes = assert.commandWorked(db.runCommand({find: collName, tailable: true}));
    assert.eq(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.firstBatch.length, 0);

    // Create a capped collection with 10 documents.
    assert.commandWorked(db.createCollection(collName, {capped: true, size: 2048}));
    for (var i = 0; i < 10; i++) {
        assert.writeOK(coll.insert({a: i}));
    }

    // GetMore should succeed if query has awaitData but no maxTimeMS is supplied.
    cmdRes = db.runCommand({find: collName, batchSize: 2, awaitData: true, tailable: true});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 2);
    cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());

    // Should also succeed if maxTimeMS is supplied on the original find.
    cmdRes = db.runCommand(
        {find: collName, batchSize: 2, awaitData: true, tailable: true, maxTimeMS: 2000});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 2);
    cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());

    // Check that we can set up a tailable cursor over the capped collection.
    cmdRes = db.runCommand({find: collName, batchSize: 5, awaitData: true, tailable: true});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 5);

    // Check that tailing the capped collection with awaitData eventually ends up returning an empty
    // batch after hitting the timeout.
    cmdRes = db.runCommand({find: collName, batchSize: 2, awaitData: true, tailable: true});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 2);

    // Issue getMore until we get an empty batch of results.
    cmdRes = db.runCommand({
        getMore: cmdRes.cursor.id,
        collection: coll.getName(),
        batchSize: NumberInt(2),
        maxTimeMS: 4000
    });
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());

    // Keep issuing getMore until we get an empty batch after the timeout expires.
    while (cmdRes.cursor.nextBatch.length > 0) {
        var now = new Date();
        cmdRes = db.runCommand({
            getMore: cmdRes.cursor.id,
            collection: coll.getName(),
            batchSize: NumberInt(2),
            maxTimeMS: 4000
        });
        assert.commandWorked(cmdRes);
        assert.gt(cmdRes.cursor.id, NumberLong(0));
        assert.eq(cmdRes.cursor.ns, coll.getFullName());
    }
    assert.gte((new Date()) - now, 2000);

    // Repeat the test, this time tailing the oplog rather than a user-created capped collection.
    // The oplog tailing in not possible on mongos.
    if (FixtureHelpers.isReplSet(db)) {
        var localDB = db.getSiblingDB("local");
        var oplogColl = localDB.oplog.$main;

        cmdRes = localDB.runCommand(
            {find: oplogColl.getName(), batchSize: 2, awaitData: true, tailable: true});
        assert.commandWorked(cmdRes);
        if (cmdRes.cursor.id > NumberLong(0)) {
            assert.eq(cmdRes.cursor.ns, oplogColl.getFullName());
            assert.eq(cmdRes.cursor.firstBatch.length, 2);

            cmdRes = localDB.runCommand(
                {getMore: cmdRes.cursor.id, collection: oplogColl.getName(), maxTimeMS: 1000});
            assert.commandWorked(cmdRes);
            assert.gt(cmdRes.cursor.id, NumberLong(0));
            assert.eq(cmdRes.cursor.ns, oplogColl.getFullName());

            while (cmdRes.cursor.nextBatch.length > 0) {
                now = new Date();
                cmdRes = localDB.runCommand(
                    {getMore: cmdRes.cursor.id, collection: oplogColl.getName(), maxTimeMS: 4000});
                assert.commandWorked(cmdRes);
                assert.gt(cmdRes.cursor.id, NumberLong(0));
                assert.eq(cmdRes.cursor.ns, oplogColl.getFullName());
            }
            assert.gte((new Date()) - now, 2000);
        }
    }

    // Test filtered inserts while writing to a capped collection.
    // Find with a filter which doesn't match any documents in the collection.
    cmdRes = assert.commandWorked(db.runCommand({
        find: collName,
        batchSize: 2,
        filter: {x: 1},
        awaitData: true,
        tailable: true,
        comment: "uniquifier_comment"
    }));
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 0);

    // getMore should time out if we insert a non-matching document.
    let insertshell = startParallelShell(function() {
        assert.soon(
            function() {
                return db.currentOp({
                             op: "getmore",
                             "command.collection": "await_data",
                             "originatingCommand.comment": "uniquifier_comment"
                         }).inprog.length == 1;
            },
            function() {
                return tojson(db.currentOp().inprog);
            });
        assert.writeOK(db.await_data.insert({x: 0}));
    });

    now = new Date();
    cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName, maxTimeMS: 4000});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.nextBatch.length, 0);
    assert.gte((new Date()) - now,
               // SERVER-31502 Add some leniency here since our server-side wait may be woken up
               // spuriously.
               3900,
               "Insert not matching filter caused awaitData getMore to return prematurely.");
    insertshell();

    // getMore should succeed if we insert a non-matching document followed by a matching one.
    insertshell = startParallelShell(function() {
        assert.writeOK(db.await_data.insert({x: 0}));
        assert.writeOK(db.await_data.insert({_id: "match", x: 1}));
        jsTestLog("Written");
    });

    cmdRes =
        db.runCommand({getMore: cmdRes.cursor.id, collection: collName, maxTimeMS: 5 * 60 * 1000});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.nextBatch.length, 1);
    assert.docEq(cmdRes.cursor.nextBatch[0], {_id: "match", x: 1});
    insertshell();
})();
