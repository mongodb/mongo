// Test the awaitData flag for the find/getMore commands.
(function() {
    'use strict';

    var mongo = MongoRunner.runMongod({master: ""});

    var cmdRes;
    var cursorId;
    var defaultBatchSize = 101;
    var collName = 'await_data';
    var db = mongo.getDB("test");
    var coll = db[collName];

    var localDB = db.getSiblingDB("local");
    var oplogColl = localDB.oplog.$main;

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

    // Create a capped collection with 10 documents.
    coll.drop();
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
    cmdRes = localDB.runCommand(
        {find: oplogColl.getName(), batchSize: 2, awaitData: true, tailable: true});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
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

})();
