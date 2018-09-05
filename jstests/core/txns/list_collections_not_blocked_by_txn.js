// @tags: [uses_transactions]
// This test ensures that listCollections does not conflict with multi-statement transactions
// as a result of taking MODE_S locks that are incompatible with MODE_IX needed for writes.
(function() {
    "use strict";
    var dbName = 'list_collections_not_blocked';
    var mydb = db.getSiblingDB(dbName);
    var session = db.getMongo().startSession({causalConsistency: false});
    var sessionDb = session.getDatabase(dbName);

    mydb.foo.drop({writeConcern: {w: "majority"}});

    assert.commandWorked(mydb.createCollection("foo", {writeConcern: {w: "majority"}}));
    session.startTransaction({readConcern: {level: "snapshot"}});

    const isMongos = assert.commandWorked(db.runCommand("ismaster")).msg === "isdbgrid";
    if (isMongos) {
        // Force the shard to refresh its database version, because this requires a database
        // exclusive lock, which will block behind the transaction.
        sessionDb.foo.distinct("x");
    }

    assert.commandWorked(sessionDb.foo.insert({x: 1}));

    for (let nameOnly of[false, true]) {
        // Check that both the nameOnly and full versions of listCollections don't block.
        let res = mydb.runCommand({listCollections: 1, nameOnly, maxTimeMS: 20 * 1000});
        assert.commandWorked(res, "listCollections should have succeeded and not timed out");
        let collObj = res.cursor.firstBatch[0];
        // collObj should only have name and type fields.
        assert.eq('foo', collObj.name);
        assert.eq('collection', collObj.type);
        assert(collObj.hasOwnProperty("idIndex") == !nameOnly, tojson(collObj));
        assert(collObj.hasOwnProperty("options") == !nameOnly, tojson(collObj));
        assert(collObj.hasOwnProperty("info") == !nameOnly, tojson(collObj));
    }

    session.commitTransaction();
    session.endSession();
}());
