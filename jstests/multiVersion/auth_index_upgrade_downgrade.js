//
// Upgrade and downgrade a MongoD node with existing system.users collections.
//
// Between 2.2 and 2.4, the schema of system.users documents expanded, and the uniqueness
// constraints changed.  As a result, the indexes on system.users collections must be replaced.
//
// Theory of operation:
//
// Running version 2.2:
//   * Construct a database, "old", and insert elements into old.system.users.
//   * Construct { user: 1 } unique index on old.system.users.
// Restart the node running version "latest":
//   * Construct a database, "new", and insert elements into new.system.users.
//   * Verify the presence of the { user: 1, userSource: 1 } unique index on new.system.users.
//   * Verify the presence of the { user: 1, userSource: 1 } unique index on old.system.users.
//   * Verify the absence of the { user: 1 } unique index on old.system.users.
//   * Verify the absence of the { user: 1 } unique index on new.system.users.
//   * Verify can insert privilege documents that would have conflicted in 2.2 into the database.
//   * Verify that the authenticate command works.
//   * Remove the conflicting entries.
// Restart the node running version 2.2:
//   * Verify that it didn't crash.
//   * Verify that the authenticate command works.
// Restart the node running version "latest":
//   * Verify that it didn't crash.
//   * Verify that the authenticate command works.
//   * Verify that the desired indexes are present.
//   * Verify can insert privilege documents that would have conflicted in 2.2 into the database.

var oldVersion = "2.2";
var newVersion = "2.4";

// Raises an exception if "status" is not a GetLastError object indicating success.
function assertGLEOK(status) {
    assert(status.ok && status.err === null,
           "Expected OK status object; found " + tojson(status));
}

// Raises an exception if "status" is not a GetLastError object indicating failure.
function assertGLENotOK(status) {
    assert(status.ok && status.err !== null,
           "Expected not-OK status object; found " + tojson(status));
}

// Finds and returns a cursor over all indexes on "collectionName" in database object "db" with
// the given "keyPattern".
function findIndex(db, collectionName, keyPattern) {
    return db.system.indexes.find({ key: keyPattern, ns: db.getName() + '.' + collectionName });
}

// Asserts that an index matching "keyPattern" is present for "collectionName" in "db".
function assertIndexExists(db, collectionName, keyPattern) {
    assert.eq(1, findIndex(db, collectionName, keyPattern).itcount());
}

// Asserts that an index matching "keyPattern" is absent for "collectionName" in "db".
function assertIndexDoesNotExist(db, collectionName, keyPattern) {
    assert.eq(0, findIndex(db, collectionName, keyPattern).itcount());
}

// Asserts that inserting "obj" into "collection" succeeds.
function assertInsertSucceeds(collection, obj) {
    collection.insert(obj);
    assertGLEOK(collection.getDB().getLastErrorObj());
}

// Asserts that inserting "obj" into "collection" fails.
function assertInsertFails(collection, obj) {
    collection.insert(obj);
    assertGLENotOK(collection.getDB().getLastErrorObj());
}

// Runs the function "action" with database objects for every database named in "dbNames", using
// "conn" as the connection object.
function withDbs(conn, dbNames, action) {
    var dbs = [];
    var i;
    for (i = 0; i < dbNames.length; ++i) {
        dbs.push(conn.getDB(dbNames[i]));
    }
    action.apply(null, dbs);
}

//
// With oldVersion
//
var conn = MongoRunner.runMongod({ remember: true, binVersion: oldVersion, smallfiles: "" });

withDbs(conn, ["old"], function (dbOld) {
    dbOld.system.users.ensureIndex({ user: 1 }, { unique: 1 });
    assertGLEOK(dbOld.getLastErrorObj());

    assertInsertSucceeds(dbOld.system.users, {user: 'andy', pwd: hex_md5('andy:mongo:a')});
    assertInsertSucceeds(dbOld.system.users, {user: 'spencer', pwd: hex_md5('spencer:mongo:a')});
    assertInsertFails(dbOld.system.users, {user: 'spencer', pwd: hex_md5('spencer:mongo:b')});
    assert(dbOld.auth('andy', 'a'));
});

//
// With newVersion
//
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod({ restart: conn, binVersion: newVersion });
withDbs(conn, ["old", "new"], function (dbOld, dbNew) {

    assertInsertSucceeds(dbNew.system.users, {user: 'andy', pwd: hex_md5('andy:mongo:a')});
    assertInsertSucceeds(dbNew.system.users, {user: 'andy', userSource: 'old', roles: ["read"]});

    assertIndexExists(dbOld, 'system.users', { user: 1, userSource: 1 });
    assertIndexExists(dbNew, 'system.users', { user: 1, userSource: 1 });
    assertIndexDoesNotExist(dbOld, 'system.users', { user: 1 });
    assertIndexDoesNotExist(dbNew, 'system.users', { user: 1 });

    dbNew.system.users.remove({user: 'andy', userSource: 'old'});
    assert(dbNew.auth('andy', 'a'));
    assert(dbOld.auth('andy', 'a'));
});

//
// Again with oldVersion
//
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod({ restart: conn, binVersion: oldVersion });
withDbs(conn, ["old", "new"], function (dbOld, dbNew) {
    assert.eq(1, dbNew.system.users.find({user: 'andy'}).itcount());
    assert.eq(1, dbOld.system.users.find({user: 'andy'}).itcount());
    assert(dbNew.auth('andy', 'a'));
    assert(dbOld.auth('andy', 'a'));
});


//
// Again with newVersion
//
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod({ restart: conn, binVersion: newVersion });
withDbs(conn, ["old", "new"], function (dbOld, dbNew) {
    assert(dbNew.auth('andy', 'a'));
    assert(dbOld.auth('andy', 'a'));
    assertIndexExists(dbOld, 'system.users', { user: 1, userSource: 1 });
    assertIndexExists(dbNew, 'system.users', { user: 1, userSource: 1 });
    assertIndexDoesNotExist(dbOld, 'system.users', { user: 1 });
    assertIndexDoesNotExist(dbNew, 'system.users', { user: 1 });
    assertInsertSucceeds(dbNew.system.users, {user: 'andy', userSource: 'old', roles: ["read"]});
});
