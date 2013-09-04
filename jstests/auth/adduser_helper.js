// Test the db.addUser() shell helper.

var passwordHash = function(username, password) {
    return hex_md5(username + ":mongo:" + password);
}

var conn = MongoRunner.runMongod({smallfiles: ""});

var db = conn.getDB('addUser');
var admin = conn.getDB('admin');
db.dropDatabase();
admin.dropDatabase();

// Can't use old-form of addUser helper to make v0 users
assert.throws(function() {db.addUser('spencer', 'password'); });
// Can't create old-style entries with new addUser helper.
assert.throws(function() {db.addUser({user:'noroles', pwd:'password'});});

// Create valid V2 format user
db.addUser({name:'andy', pwd:'password', roles:['read']});
assert.eq(1, admin.system.users.count());
userObj = admin.system.users.findOne({name:'andy'});
assert.eq('andy', userObj['name']);
assert.eq(passwordHash('andy', 'password'), userObj['credentials']['MONGODB-CR']);

// test changing password
db.changeUserPassword('andy', 'newpassword');
assert.eq(1, admin.system.users.count());
userObj = admin.system.users.findOne();
assert.eq('andy', userObj['name']);
assert.eq(passwordHash('andy', 'newpassword'), userObj['credentials']['MONGODB-CR']);

// Should fail because user already exists
assert.throws(function() {db.addUser({user:'andy', pwd:'password', roles:['read']});});

// Create valid extended form external user
db.getSiblingDB("$external").addUser({user:'spencer', roles:['readWrite']});
assert.eq(2, admin.system.users.count());
userObj = admin.system.users.findOne({name:'spencer', source:'$external'});
assert.eq('spencer', userObj['name']);
assert.eq('$external', userObj['source']);
assert(!userObj['credentials']);


// Create valid V2 format user using new helper format
db.addUser('bob', 'password', ['read']);
assert.eq(3, admin.system.users.count());
userObj = admin.system.users.findOne({name:'bob'});
assert.eq('bob', userObj['name']);
assert.eq(passwordHash('bob', 'password'), userObj['credentials']['MONGODB-CR']);
