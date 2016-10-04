var conn = MongoRunner.runMongod({auth: ""});

var adminDb = conn.getDB("admin");
var testDb = conn.getDB("testdb");

adminDb.createUser({
    user: 'admin',
    pwd: 'password',
    roles: ['userAdminAnyDatabase', 'dbAdminAnyDatabase', 'readWriteAnyDatabase']
});

adminDb.auth('admin', 'password');
testDb.createUser({user: 'readUser', pwd: 'password', roles: ['read']});
testDb.createUser({user: 'dbAdminUser', pwd: 'password', roles: ['dbAdmin']});
testDb.createUser({
    user: 'dbAdminAnyDBUser',
    pwd: 'password',
    roles: [{role: 'dbAdminAnyDatabase', db: 'admin'}]
});
testDb.setProfilingLevel(2);
testDb.foo.findOne();
adminDb.logout();
testDb.auth('readUser', 'password');
assert.throws(function() {
    testDb.system.profile.findOne();
});
testDb.logout();

// SERVER-14355
testDb.auth('dbAdminUser', 'password');
testDb.setProfilingLevel(0);
testDb.system.profile.drop();
assert.commandWorked(testDb.createCollection("system.profile", {capped: true, size: 1024}));
testDb.logout();

// SERVER-16944
testDb.auth('dbAdminAnyDBUser', 'password');
testDb.setProfilingLevel(0);
testDb.system.profile.drop();
assert.commandWorked(testDb.createCollection("system.profile", {capped: true, size: 1024}));