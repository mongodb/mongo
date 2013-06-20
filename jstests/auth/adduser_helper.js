// Test the db.addUser() shell helper.

var passwordHash = function(username, password) {
    return hex_md5(username + ":mongo:" + password);
}

var conn = MongoRunner.runMongod({smallfiles: ""});

var db = conn.getDB('addUser');
db.dropDatabase();

jsTest.log("Testing creating backwards-compatible user objects using old form of db.addUser");
db.addUser('spencer', 'password');
assert.eq(1, db.system.users.count());
var userObj = db.system.users.findOne();
assert.eq('spencer', userObj['user']);
assert.eq(passwordHash('spencer', 'password'), userObj['pwd']);

// Test re-adding the same user fails
assert.throws(function() { db.addUser("spencer", "password2"); });

// test changing password
db.changeUserPassword('spencer', 'newpassword');
assert.eq(1, db.system.users.count());
userObj = db.system.users.findOne();
assert.eq('spencer', userObj['user']);
assert.eq(passwordHash('spencer', 'newpassword'), userObj['pwd']);


jsTest.log("Testing new form of addUser");

// Can't create old-style entries with new addUser helper.
assert.throws(function() {db.addUser({user:'noroles', pwd:'password'});});
// Should fail because user already exists
assert.throws(function() {db.addUser({user:'spencer', pwd:'password', roles:'read'});});

// Create valid extended form user
db.addUser({user:'andy', pwd:'password', roles:['read']});
assert.eq(2, db.system.users.count());
userObj = db.system.users.findOne({user:'andy'});
assert.eq('andy', userObj['user']);
assert.eq(passwordHash('andy', 'password'), userObj['pwd']);
assert.eq('read', userObj['roles'][0]);

// Create valid extended form external user
db.addUser({user:'andy', userSource:'$sasl', roles:['readWrite']});
assert.eq(3, db.system.users.count());
userObj = db.system.users.findOne({user:'andy', userSource:'$sasl'});
assert.eq('andy', userObj['user']);
assert.eq('$sasl', userObj['userSource']);
assert.eq('readWrite', userObj['roles'][0]);
assert(!userObj['pwd']);