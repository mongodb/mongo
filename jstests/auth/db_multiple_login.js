// Test the behavior when two users from the same database are authenticated serially on a single
// connection.  Expected behavior is that the first user is implicitly logged out by the second
// authentication.
//
// Regression test for SERVER-8144.

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


var conn = MongoRunner.runMongod({ auth: "", smallfiles: "" });
var admin = conn.getDB("admin");
var test = conn.getDB("test");

admin.addUser('admin', 'a', jsTest.adminUserRoles);
assert(admin.auth('admin', 'a'));
test.addUser({user: 'reader', pwd: 'a', roles: [ "read" ]});
test.addUser({user: 'writer', pwd: 'a', roles: [ "readWrite" ]});
admin.logout();

// Nothing logged in, can neither read nor write.
assertInsertFails(test.docs, { value: 0 });
assert.throws(function() { test.foo.findOne() });

// Writer logged in, can read and write.
test.auth('writer', 'a');
assertInsertSucceeds(test.docs, { value: 1 });
test.foo.findOne();

// Reader logged in, replacing writer, can only read.
test.auth('reader', 'a');
assertInsertFails(test.docs, { value: 2 });
test.foo.findOne();
