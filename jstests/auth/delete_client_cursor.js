/**
 * Regression test for SERVER-4892.
 *
 * Verify that a client can delete cursors that it creates, when mongod is running with "auth"
 * enabled.
 *
 * This test requires users to persist across a restart.
 * @tags: [requires_persistence]
 */

let baseName = jsTestName();
let dbpath = MongoRunner.dataPath + baseName;
resetDbpath(dbpath);
let mongodCommonArgs = {
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
    let mongod = MongoRunner.runMongod(Object.merge(mongodCommonArgs, extraMongodArgs));

    try {
        operation(mongod);
    } finally {
        MongoRunner.stopMongod(mongod);
    }
}

/*
 * Fail an assertion if the given "mongod" instance does not have exactly expectNumLiveCursors live
 * cursors on the server.
 */
function expectNumLiveCursors(mongod, expectedNumLiveCursors) {
    let db = mongod.getDB("admin");
    db.auth("admin", "admin");
    let actualNumLiveCursors = db.serverStatus().metrics.cursor.open.total;
    assert(
        actualNumLiveCursors == expectedNumLiveCursors,
        "actual num live cursors (" + actualNumLiveCursors + ") != exptected (" + expectedNumLiveCursors + ")",
    );
}

withMongod({noauth: ""}, function setupTest(mongod) {
    let admin, somedb, conn;
    conn = new Mongo(mongod.host);
    admin = conn.getDB("admin");
    somedb = conn.getDB("somedb");
    admin.createUser({user: "admin", pwd: "admin", roles: jsTest.adminUserRoles});
    admin.auth("admin", "admin");
    somedb.createUser({user: "frim", pwd: "fram", roles: jsTest.basicUserRoles});
    somedb.data.drop();
    for (let i = 0; i < 10; ++i) {
        assert.commandWorked(somedb.data.insert({val: i}));
    }
    admin.logout();
});

withMongod({auth: ""}, function runTest(mongod) {
    let conn = new Mongo(mongod.host);
    let somedb = conn.getDB("somedb");
    somedb.auth("frim", "fram");

    expectNumLiveCursors(mongod, 0);

    let cursor = somedb.data.find({}, {"_id": 1}).batchSize(1);
    cursor.next();
    expectNumLiveCursors(mongod, 1);

    cursor.close();

    // NOTE(schwerin): dbKillCursors gets piggybacked on subsequent messages on the
    // connection, so we
    // have to force a message to the server.
    somedb.data.findOne();

    expectNumLiveCursors(mongod, 0);
});
