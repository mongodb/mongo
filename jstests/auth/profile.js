// Check that username information gets recorded properly in profiler.
// @tags: [requires_profiling]

(function() {
'use strict';

const conn = MongoRunner.runMongod();
const db = conn.getDB("profile");
const username = "user";
db.createUser({user: username, pwd: "password", roles: jsTest.basicUserRoles});

function lastOp(db) {
    return db.system.profile.find().sort({$natural: -1}).next();
}

function principalName(user, db) {
    return user + "@" + db.getName();
}

db.setProfilingLevel(0);
db.system.profile.drop();
assert.eq(0, db.system.profile.count());

db.setProfilingLevel(2);

db.foo.findOne();
const last1 = lastOp(db);
assert.eq("", last1.user);
assert.eq(0, last1.allUsers.length);

assert(db.auth(username, "password"));
db.foo.findOne();
const last2 = lastOp(db);
assert.eq(principalName(username, db), last2.user);
assert.eq(1, last2.allUsers.length);
assert.eq(username, last2.allUsers[0].user);
assert.eq(db, last2.allUsers[0].db);
db.logout();

db.setProfilingLevel(0);
db.dropDatabase();
MongoRunner.stopMongod(conn);
})();
