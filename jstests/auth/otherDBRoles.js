var conn = MongoRunner.runMongod({auth : ""});

function assertGLENotOK(status) {
    assert(status.ok && status.err !== null,
           "Expected not-OK status object; found " + tojson(status));
}

function assertGLEOK(status) {
    assert(status.ok && status.err === null,
           "Expected OK status object; found " + tojson(status));
}

var adminDB = conn.getDB("admin");
var testDB = conn.getDB("test");
var test2DB = conn.getDB("test2");

// Can't use otherDBRoles outside of admin DB
assert.throws(function() {
                  testDB.addUser({user:'spencer',
                                  pwd:'x',
                                  roles:[],
                                  otherDBRoles: {test2: ['readWrite']}});
              });

testDB.addUser({user: 'spencer', pwd: 'x', roles: ['readWrite']});

adminDB.addUser({user:'spencer',
                 userSource: 'test',
                 roles:[],
                 otherDBRoles: {test: ['dbAdmin'], test2: ['readWrite']}});

testDB.auth('spencer', 'x');

testDB.foo.insert({a:1});
assertGLEOK(testDB.getLastErrorObj());
assert.eq(1, testDB.foo.findOne().a);

// Make sure user got the dbAdmin role
assert.commandWorked(testDB.foo.runCommand("compact"));

// Make sure the user got privileges on the test2 database.
test2DB.foo.insert({a:1});
assertGLEOK(test2DB.getLastErrorObj());
assert.eq(1, test2DB.foo.findOne().a);

assert.commandFailed(test2DB.foo.runCommand("compact"));