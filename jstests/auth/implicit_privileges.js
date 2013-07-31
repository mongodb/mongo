// Test implicit privilege acquisition.
//
// TODO: Rewrite user document creation portion of test when addUser shell helper is updated.

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

var conn = MongoRunner.runMongod({ auth: "", smallfiles: "" });
var admin = conn.getDB("admin");
var test = conn.getDB("test");
var test2 = conn.getDB("test2");

assert.commandWorked(admin.runCommand(
                     { createUser: 1,
                       user: 'root',
                       pwd: 'a',
                       roles: ["clusterAdmin",
                               "readWriteAnyDatabase",
                               "dbAdminAnyDatabase",
                               "userAdminAnyDatabase"]
                     }));

var andyAddUserCommandTestDb = {
    createUser: 1,
    user: "andy",
    pwd: "a",
    roles: [ "readWrite" ]
};

var andyAddUserCommandTest2Db = {
    createUser: 1,
    user: "andy",
    userSource: "test",
    roles: [ "read" ]
};

assertInsertFails(test.foo, {});
assert.commandFailed(test.runCommand(andyAddUserCommandTestDb));
assert.throws(function() { test.foo.findOne(); });
assert.throws(function() { test2.foo.findOne(); } );

assert(admin.auth('root', 'a'));
assert.commandWorked(test.runCommand(andyAddUserCommandTestDb));
assert.commandWorked(test2.runCommand(andyAddUserCommandTest2Db));
assertInsertSucceeds(test.foo, {_id: 0});
assertInsertSucceeds(test2.foo, {_id: 0});

admin.logout();

assert(test.auth('andy', 'a'));
assertInsertSucceeds(test.foo, {_id: 1});
assertInsertFails(test2.foo, {_id: 1});
assert.eq(test.foo.findOne({_id: 1})._id, 1);
assert.eq(test2.foo.findOne({_id: 0})._id, 0);
assert(test.logout());
assertInsertFails(test.foo, {});
assert.commandFailed(test.runCommand(andyAddUserCommandTestDb));
assert.throws(function() { test.foo.findOne(); });
assert.throws(function() { test2.foo.findOne(); } );

