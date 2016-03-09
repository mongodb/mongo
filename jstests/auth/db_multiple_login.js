// Test the behavior when two users from the same database are authenticated serially on a single
// connection.  Expected behavior is that the first user is implicitly logged out by the second
// authentication.
//
// Regression test for SERVER-8144.
var conn = MongoRunner.runMongod({auth: "", smallfiles: ""});
var admin = conn.getDB("admin");
var test = conn.getDB("test");

admin.createUser({user: 'admin', pwd: 'a', roles: jsTest.adminUserRoles});
assert(admin.auth('admin', 'a'));
test.createUser({user: 'reader', pwd: 'a', roles: ["read"]});
test.createUser({user: 'writer', pwd: 'a', roles: ["readWrite"]});
admin.logout();

// Nothing logged in, can neither read nor write.
assert.writeError(test.docs.insert({value: 0}));
assert.throws(function() {
    test.foo.findOne();
});

// Writer logged in, can read and write.
test.auth('writer', 'a');
assert.writeOK(test.docs.insert({value: 1}));
test.foo.findOne();

// Reader logged in, replacing writer, can only read.
test.auth('reader', 'a');
assert.writeError(test.docs.insert({value: 2}));
test.foo.findOne();
