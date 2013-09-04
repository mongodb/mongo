// Check that username information gets recorded properly in profiler.
var conn = startMongodTest();
var db1 = conn.getDB("profile-a");
var db2 = db1.getSisterDB("profile-b");
var username = "user";
db1.addUser(username, "password", jsTest.basicUserRoles);
db2.addUser(username, "password", jsTest.basicUserRoles);


function lastOp(db) {
    return db.system.profile.find().sort( { $natural:-1 } ).next();
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
assert.eq(username, last.allUsers[0].name);
assert.eq(db1, last.allUsers[0].source);

db2.auth(username, "password");

db1.foo.findOne();
var last = lastOp(db1);
// Which user gets put in "user" and the ordering of users in "allUsers" is undefined.
assert((principalName(username, db1) == last.user) || (principalName(username, db2) == last.user));
assert.eq(2, last.allUsers.length);
assert.eq(username, last.allUsers[0].name);
assert.eq(username, last.allUsers[1].name);
assert((db1 == last.allUsers[0].source && db2 == last.allUsers[1].source) ||
       (db2 == last.allUsers[0].source && db1 == last.allUsers[1].source));

db1.setProfilingLevel(0);
db1.dropDatabase();
db2.dropDatabase();
