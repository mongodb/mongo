// SERVER-8623: Test that renameCollection can't be used to bypass auth checks on system namespaces
var conn = MongoRunner.runMongod({auth : ""});

var adminDB = conn.getDB("admin");
var testDB = conn.getDB("testdb");
var testDB2 = conn.getDB("testdb2");

var CodeUnauthorized = 13;

testDB.addUser({user:'spencer',
                pwd:'password',
                roles:['readWrite']});

adminDB.addUser({user:'userAdmin',
                 pwd:'password',
                 roles:['userAdminAnyDatabase']});

var userAdminConn = new Mongo(conn.host);
userAdminConn.getDB('admin').auth('userAdmin', 'password');
userAdminConn.getDB('admin').addUser({user:'readWriteAdmin',
                                      pwd:'password',
                                      roles:['readWriteAnyDatabase']});


// Test that a readWrite user can't rename system.profile to something they can read.
testDB.auth('spencer', 'password');
res = testDB.system.profile.renameCollection("profile");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);


// Test that a readWrite user can't rename system.users to something they can read.
var res = testDB.system.users.renameCollection("users");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);
assert.eq(0, testDB.users.count());


// Test that a readWrite user can't use renameCollection to override system.users
testDB.users.insert({user:'backdoor',
                     pwd:'hashedpassword',
                     roles:'userAdmin'});
res = testDB.users.renameCollection("system.users", true);
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);
assert.eq(null, userAdminConn.getDB('testdb').system.users.findOne({user:'backdoor'}));


// Test that a readWrite user can't create system.users using renameCollection
adminDB.auth('readWriteAdmin', 'password');
testDB2.users.insert({user:'backdoor',
                      pwd:'hashedpassword',
                      roles:'userAdmin'});
res = testDB2.users.renameCollection("system.users");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);
assert.eq(0, userAdminConn.getDB('testdb2').system.users.count());


// Test that you can't rename system.users across databases
testDB2.users.drop();
var res = adminDB.runCommand({renameCollection:'testdb.system.users', to:'testdb2.users'});
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);
assert.eq(0, testDB2.users.count());


// Test that a userAdmin can't rename system.users without readWrite
testDB.users.drop();
var res = userAdminConn.getDB('testdb').system.users.renameCollection("users");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);
assert.eq(0, testDB.users.count());


// Test that with userAdmin AND dbAdmin you CAN rename to/from system.users
adminDB.auth('userAdmin', 'password');
var res = testDB.system.users.renameCollection("users");
assert.eq(1, res.ok);
assert.eq(1, testDB.users.count());

testDB.users.drop();
testDB.users.insert({user:'newUser',
                     pwd:'hashedPassword',
                     roles:['readWrite']});
var res = testDB.users.renameCollection("system.users");
assert.eq(1, res.ok);
assert.neq(null, testDB.system.users.findOne({user:'newUser'}));
assert.eq(null, testDB.system.users.findOne({user:'spencer'}));
