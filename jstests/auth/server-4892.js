/**
 * Regression test for SERVER-4892.
 *
 * Verify that a client can delete cursors that it creates, when mongod is running with "auth"
 * enabled.
 *
 * This test requires users to persist across a restart.
 * @tags: [requires_persistence]
 */

var baseName = 'jstests_auth_server4892';
var dbpath = MongoRunner.dataPath + baseName;
resetDbpath(dbpath);
var mongodCommonArgs = {
    dbpath: dbpath,
    noCleanData: true,
};

/*
 * Start an instance of mongod, pass it as a parameter to operation(), then stop the instance of
 * mongod before unwinding or returning out of with_mongod().
 *
 * 'extraMongodArgs' are extra arguments to pass on the mongod command line, as an object.
 */
function withMongod(extraMongodArgs, operation) {
    var mongod = MongoRunner.runMongod(Object.merge(mongodCommonArgs, extraMongodArgs));

    try {
        operation(mongod);
    } finally {
        MongoRunner.stopMongod(mongod.port);
    }
}

/*
 * Fail an assertion if the given "mongod" instance does not have exactly expectNumLiveCursors live
 * cursors on the server.
 */
function expectNumLiveCursors(mongod, expectedNumLiveCursors) {
    var conn = new Mongo(mongod.host);
    var db = mongod.getDB('admin');
    db.auth('admin', 'admin');
    var actualNumLiveCursors = db.serverStatus().metrics.cursor.open.total;
    assert(actualNumLiveCursors == expectedNumLiveCursors,
           "actual num live cursors (" + actualNumLiveCursors + ") != exptected (" +
               expectedNumLiveCursors + ")");
}

withMongod({noauth: ""}, function setupTest(mongod) {
    var admin, somedb, conn;
    conn = new Mongo(mongod.host);
    admin = conn.getDB('admin');
    somedb = conn.getDB('somedb');
    admin.createUser({user: 'admin', pwd: 'admin', roles: jsTest.adminUserRoles});
    admin.auth('admin', 'admin');
    somedb.createUser({user: 'frim', pwd: 'fram', roles: jsTest.basicUserRoles});
    somedb.data.drop();
    for (var i = 0; i < 10; ++i) {
        assert.writeOK(somedb.data.insert({val: i}));
    }
    admin.logout();
});

withMongod({auth: ""}, function runTest(mongod) {
    var conn = new Mongo(mongod.host);
    var somedb = conn.getDB('somedb');
    somedb.auth('frim', 'fram');

    expectNumLiveCursors(mongod, 0);

    var cursor = somedb.data.find({}, {'_id': 1}).batchSize(1);
    cursor.next();
    expectNumLiveCursors(mongod, 1);

    cursor = null;
    // NOTE(schwerin): We assume that after setting cursor = null, there are no remaining
    // references
    // to the cursor, and that gc() will deterministically garbage collect it.
    gc();

    // NOTE(schwerin): dbKillCursors gets piggybacked on subsequent messages on the
    // connection, so we
    // have to force a message to the server.
    somedb.data.findOne();

    expectNumLiveCursors(mongod, 0);
});
