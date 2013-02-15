var conn = MongoRunner.runMongod({auth : ""});

var adminDb = conn.getDB("admin");
var testDb = conn.getDB("testdb");

adminDb.addUser({user:'admin',
                 pwd:'password',
                 roles:['userAdminAnyDatabase','dbAdminAnyDatabase', 'readWriteAnyDatabase']});

adminDb.auth('admin','password');
testDb.addUser({user:'readUser',pwd:'password',roles:['read']});
testDb.setProfilingLevel(2);
adminDb.logout();
testDb.auth('readUser','password');
assert.throws(function() { testDb.system.profile.findOne(); });