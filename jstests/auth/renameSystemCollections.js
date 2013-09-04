// SERVER-8623: Test that renameCollection can't be used to bypass auth checks on system namespaces
var conn = MongoRunner.runMongod({auth : ""});

var adminDB = conn.getDB("admin");
var testDB = conn.getDB("testdb");
var testDB2 = conn.getDB("testdb2");

var CodeUnauthorized = 13;

adminDB.addUser({user:'userAdmin',
                 pwd:'password',
                 roles:['userAdminAnyDatabase']});

adminDB.auth('userAdmin', 'password');
adminDB.addUser({user:'readWriteAdmin',
                 pwd:'password',
                 roles:['readWriteAnyDatabase']});
adminDB.addUser({user:'readWriteAndUserAdmin',
                 pwd:'password',
                 roles:['readWriteAnyDatabase', 'userAdminAnyDatabase']});
adminDB.logout();


jsTestLog("Test that a readWrite user can't rename system.profile to something they can read");
adminDB.auth('readWriteAdmin', 'password');
res = adminDB.system.profile.renameCollection("profile");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);


jsTestLog("Test that a readWrite user can't rename system.users to something they can read");
var res = adminDB.system.users.renameCollection("users");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);
assert.eq(0, adminDB.users.count());


jsTestLog("Test that a readWrite user can't use renameCollection to override system.users");
adminDB.users.insert({user:'backdoor',
                     pwd:'hashedpassword',
                     roles:'userAdmin'});
res = adminDB.users.renameCollection("system.users", true);
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);
adminDB.users.drop();

jsTestLog("Test that a userAdmin can't rename system.users without readWrite");
adminDB.logout();
adminDB.auth('userAdmin', 'password');
var res = adminDB.system.users.renameCollection("users");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);
assert.eq(3, adminDB.system.users.count());

adminDB.auth('readWriteAndUserAdmin', 'password');
assert.eq(0, adminDB.users.count());

jsTestLog("Test that with userAdmin AND dbAdmin you CAN rename to/from system.users");
var res = adminDB.system.users.renameCollection("users");
assert.eq(1, res.ok);
assert.eq(3, adminDB.users.count());

adminDB.users.drop();
adminDB.users.insert({user:'newUser',
                      pwd:'hashedPassword',
                      roles:['readWrite']});
var res = adminDB.users.renameCollection("system.users");
assert.eq(1, res.ok);
assert.neq(null, adminDB.system.users.findOne({user:'newUser'}));
assert.eq(null, adminDB.system.users.findOne({user:'userAdmin'}));
