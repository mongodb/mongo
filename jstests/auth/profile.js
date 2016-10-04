// Check that username information gets recorded properly in profiler.
var conn = MongoRunner.runMongod();
var db1 = conn.getDB("profile-a");
var db2 = db1.getSisterDB("profile-b");
var username = "user";
db1.createUser({user: username, pwd: "password", roles: jsTest.basicUserRoles});
db2.createUser({user: username, pwd: "password", roles: jsTest.basicUserRoles});

function lastOp(db) {
    return db.system.profile.find().sort({$natural: -1}).next();
}

function principalName(user, db) {
    return user + "@" + db.getName();
}

db1.setProfilingLevel(0);
db1.system.profile.drop();
assert.eq(0, db1.system.profile.count());

db1.setProfilingLevel(2);

db1.foo.findOne();
var last = lastOp(db1);
assert.eq("", last.user);
assert.eq(0, last.allUsers.length);

db1.auth(username, "password");

db1.foo.findOne();
var last = lastOp(db1);
assert.eq(principalName(username, db1), last.user);
assert.eq(1, last.allUsers.length);
assert.eq(username, last.allUsers[0].user);
assert.eq(db1, last.allUsers[0].db);

db2.auth(username, "password");

db1.foo.findOne();
var last = lastOp(db1);
// Which user gets put in "user" and the ordering of users in "allUsers" is undefined.
assert((principalName(username, db1) == last.user) || (principalName(username, db2) == last.user));
assert.eq(2, last.allUsers.length);
assert.eq(username, last.allUsers[0].user);
assert.eq(username, last.allUsers[1].user);
assert((db1 == last.allUsers[0].db && db2 == last.allUsers[1].db) ||
       (db2 == last.allUsers[0].db && db1 == last.allUsers[1].db));

db1.setProfilingLevel(0);
db1.dropDatabase();
db2.dropDatabase();
