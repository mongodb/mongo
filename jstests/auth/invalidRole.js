// Test that authenticating as a user with an invalid role doesn't prevent acquiriing privileges
// from other, valid, roles.
var conn = MongoRunner.runMongod({auth : ""});

var adminDB = conn.getDB("admin");
var testDB = conn.getDB("testdb");

testDB.foo.insert({a:1});

testDB.addUser({user:'spencer',
                pwd:'password',
                roles:['invalidRole', 'readWrite']});

adminDB.addUser({user:'admin',
                 pwd:'password',
                 roles:['userAdminAnyDatabase']});

assert.throws(function() { testDB.foo.findOne(); });
testDB.auth('spencer', 'password');
assert.eq(1, testDB.foo.findOne().a);
