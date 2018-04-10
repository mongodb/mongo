// @tags: [uses_transactions]
(function() {
    "use strict";
    var dbName = "list_collections_not_blocked";
    var mydb = db.getSiblingDB(dbName);
    var session = db.getMongo().startSession({causalConsistency: false});
    var sessionDb = session.getDatabase(dbName);

    mydb.foo.drop();

    mydb.createCollection("foo");
    session.startTransaction({readConcern: {level: "snapshot"}});
    sessionDb.foo.insert({x: 1});

    assert.soon(function() {
        let res = mydb.runCommand({listCollections: 1, nameOnly: true});
        assert.commandWorked(res);
        let collObj = res.cursor.firstBatch[0];
        // collObj should only have name and type fields.
        assert.eq('foo', collObj.name);
        assert.eq('collection', collObj.type);
        assert(!collObj.hasOwnProperty("idIndex"), tojson(collObj));
        assert(!collObj.hasOwnProperty("options"), tojson(collObj));
        assert(!collObj.hasOwnProperty("info"), tojson(collObj));
        return true;
    }, 'listCollections gets blocked by txn', 60 * 1000);

    session.commitTransaction();
    session.endSession();

}());
