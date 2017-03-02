/**
 * Regression test for SERVER-4892.
 *
 * Verify that a client can delete cursors that it creates, when bongod is running with "auth"
 * enabled.
 *
 * This test requires users to persist across a restart.
 * @tags: [requires_persistence]
 */

var baseName = 'jstests_auth_server4892';
var dbpath = BongoRunner.dataPath + baseName;
resetDbpath(dbpath);
var bongodCommonArgs = {
    dbpath: dbpath,
    noCleanData: true,
};

/*
 * Start an instance of bongod, pass it as a parameter to operation(), then stop the instance of
 * bongod before unwinding or returning out of with_bongod().
 *
 * 'extraBongodArgs' are extra arguments to pass on the bongod command line, as an object.
 */
function withBongod(extraBongodArgs, operation) {
    var bongod = BongoRunner.runBongod(Object.merge(bongodCommonArgs, extraBongodArgs));

    try {
        operation(bongod);
    } finally {
        BongoRunner.stopBongod(bongod.port);
    }
}

/*
 * Fail an assertion if the given "bongod" instance does not have exactly expectNumLiveCursors live
 * cursors on the server.
 */
function expectNumLiveCursors(bongod, expectedNumLiveCursors) {
    var conn = new Bongo(bongod.host);
    var db = bongod.getDB('admin');
    db.auth('admin', 'admin');
    var actualNumLiveCursors = db.serverStatus().metrics.cursor.open.total;
    assert(actualNumLiveCursors == expectedNumLiveCursors,
           "actual num live cursors (" + actualNumLiveCursors + ") != exptected (" +
               expectedNumLiveCursors + ")");
}

withBongod({noauth: ""}, function setupTest(bongod) {
    var admin, somedb, conn;
    conn = new Bongo(bongod.host);
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

withBongod({auth: ""}, function runTest(bongod) {
    var conn = new Bongo(bongod.host);
    var somedb = conn.getDB('somedb');
    somedb.auth('frim', 'fram');

    expectNumLiveCursors(bongod, 0);

    var cursor = somedb.data.find({}, {'_id': 1}).batchSize(1);
    cursor.next();
    expectNumLiveCursors(bongod, 1);

    cursor.close();

    // NOTE(schwerin): dbKillCursors gets piggybacked on subsequent messages on the
    // connection, so we
    // have to force a message to the server.
    somedb.data.findOne();

    expectNumLiveCursors(bongod, 0);
});
