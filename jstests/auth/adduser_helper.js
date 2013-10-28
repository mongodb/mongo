// Test the db.addUser() shell helper.

var passwordHash = function(username, password) {
    return hex_md5(username + ":mongo:" + password);
}

var conn = MongoRunner.runMongod({smallfiles: ""});

var db = conn.getDB('addUser');
var admin = conn.getDB('admin');
db.dropDatabase();
admin.dropDatabase();

// Test that the deprecated (username,password,readonly) form of addUser still works
db.addUser('dbReadWrite', 'x');
var userObj = db.getUser('dbReadWrite');
assert.eq(1, userObj.roles.length);
assert.eq("dbOwner", userObj.roles[0].role);
assert.eq(db.getName(), userObj.roles[0].db);

db.addUser('dbReadOnly', 'x', true);
userObj = db.getUser('dbReadOnly');
assert.eq(1, userObj.roles.length);
assert.eq("read", userObj.roles[0].role);
assert.eq(db.getName(), userObj.roles[0].db);

admin.addUser('adminReadWrite', 'x');
userObj = admin.getUser('adminReadWrite');
assert.eq(1, userObj.roles.length);
assert.eq("root", userObj.roles[0].role);
assert.eq("admin", userObj.roles[0].db);

admin.addUser('adminReadOnly', 'x', true);
userObj = admin.getUser('adminReadOnly');
assert.eq(1, userObj.roles.length);
assert.eq("readAnyDatabase", userObj.roles[0].role);
assert.eq("admin", userObj.roles[0].db);

admin.dropDatabase();

// Create valid V2 format user
db.addUser({user:'andy', pwd:'password', roles:['read']});
assert.eq(1, admin.system.users.count());
userObj = admin.system.users.findOne({user:'andy'});
assert.eq('andy', userObj['user']);
assert.eq(passwordHash('andy', 'password'), userObj['credentials']['MONGODB-CR']);

// test changing password
db.changeUserPassword('andy', 'newpassword');
assert.eq(1, admin.system.users.count());
userObj = admin.system.users.findOne();
assert.eq('andy', userObj['user']);
assert.eq(passwordHash('andy', 'newpassword'), userObj['credentials']['MONGODB-CR']);

// Should fail because user already exists
assert.throws(function() {db.addUser({user:'andy', pwd:'password', roles:['read']});});

// Create valid extended form external user
db.getSiblingDB("$external").addUser({user:'spencer', roles:['readWrite']});
assert.eq(2, admin.system.users.count());
userObj = admin.system.users.findOne({user:'spencer', db:'$external'});
assert.eq('spencer', userObj['user']);
assert.eq('$external', userObj['db']);
assert.eq(true, userObj['credentials']['external']);
