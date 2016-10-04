var mydb = db.getSiblingDB('auth1_db');
mydb.dropAllUsers();

pass = "a" + Math.random();
// print( "password [" + pass + "]" );

mydb.createUser({user: "eliot", pwd: pass, roles: jsTest.basicUserRoles});

assert(mydb.auth("eliot", pass), "auth failed");
assert(!mydb.auth("eliot", pass + "a"), "auth should have failed");

pass2 = "b" + Math.random();
mydb.changeUserPassword("eliot", pass2);

assert(!mydb.auth("eliot", pass), "failed to change password failed");
assert(mydb.auth("eliot", pass2), "new password didn't take");

assert(mydb.auth("eliot", pass2), "what?");
mydb.dropUser("eliot");
assert(!mydb.auth("eliot", pass2), "didn't drop user");

var a = mydb.getMongo().getDB("admin");
a.dropAllUsers();
pass = "c" + Math.random();
a.createUser({user: "super", pwd: pass, roles: jsTest.adminUserRoles});
assert(a.auth("super", pass), "auth failed");
assert(!a.auth("super", pass + "a"), "auth should have failed");

mydb.dropAllUsers();
pass = "a" + Math.random();

mydb.createUser({user: "eliot", pwd: pass, roles: jsTest.basicUserRoles});

assert.commandFailed(mydb.runCommand({authenticate: 1, user: "eliot", nonce: "foo", key: "bar"}));

// check sanity check SERVER-3003

var before = a.system.users.count({db: mydb.getName()});

assert.throws(function() {
    mydb.createUser({user: "", pwd: "abc", roles: jsTest.basicUserRoles});
}, [], "C1");
assert.throws(function() {
    mydb.createUser({user: "abc", pwd: "", roles: jsTest.basicUserRoles});
}, [], "C2");

var after = a.system.users.count({db: mydb.getName()});
assert(before > 0, "C3");
assert.eq(before, after, "C4");

// Clean up after ourselves so other tests using authentication don't get messed up.
mydb.dropAllUsers();
