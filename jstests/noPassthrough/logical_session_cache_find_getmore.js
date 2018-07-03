(function() {
    'use strict';

    TestData.disableImplicitSessions = true;

    var conn = MongoRunner.runMongod({setParameter: {maxSessions: 2}});
    var testDB = conn.getDB("test");

    assert.writeOK(testDB.foo.insert({data: 1}));
    assert.writeOK(testDB.foo.insert({data: 2}));

    for (var i = 0; i < 2; i++) {
        var session = conn.startSession();
        var db = session.getDatabase("test");
        var res = assert.commandWorked(db.runCommand({find: "foo", batchSize: 1}),
                                       "unable to run find when the cache is not full");
        var cursorId = res.cursor.id;
        assert.commandWorked(db.runCommand({getMore: cursorId, collection: "foo"}),
                             "unable to run getMore when the cache is not full");
    }

    var session3 = conn.startSession();
    var db = session3.getDatabase("test");
    assert.commandFailed(db.runCommand({find: "foo", batchSize: 1}),
                         "able to run find when the cache is full");

    MongoRunner.stopMongod(conn);
})();
