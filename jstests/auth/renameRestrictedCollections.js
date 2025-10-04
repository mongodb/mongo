// SERVER-8623: Test that renameCollection can't be used to bypass auth checks on system
// namespaces
// @tags: [
//   requires_fcv_72,
// ]

const conn = MongoRunner.runMongod({auth: ""});

const adminDB = conn.getDB("admin");
const configDB = conn.getDB("config");
const localDB = conn.getDB("local");
const CodeUnauthorized = 13;

const backdoorUserDoc = {
    user: "backdoor",
    db: "admin",
    pwd: "hashed",
    roles: ["root"],
};

adminDB.createUser({user: "userAdmin", pwd: "password", roles: ["userAdminAnyDatabase"]});

assert(adminDB.auth("userAdmin", "password"));
adminDB.createUser({user: "readWriteAdmin", pwd: "password", roles: ["readWriteAnyDatabase"]});
adminDB.createUser({
    user: "readWriteAndUserAdmin",
    pwd: "password",
    roles: ["readWriteAnyDatabase", "userAdminAnyDatabase"],
});
adminDB.createUser({user: "root", pwd: "password", roles: ["root"]});
adminDB.createUser({user: "rootier", pwd: "password", roles: ["__system"]});
adminDB.logout();

jsTestLog("Test that a readWrite user can't rename system.profile to something they can read");
assert(adminDB.auth("readWriteAdmin", "password"));
let res = adminDB.system.profile.renameCollection("profile");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);

jsTestLog("Test that a readWrite user can't rename system.users to something they can read");
res = adminDB.system.users.renameCollection("users");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);
assert.eq(0, adminDB.users.count());

jsTestLog("Test that a readWrite user can't use renameCollection to override system.users");
adminDB.users.insert(backdoorUserDoc);
res = adminDB.users.renameCollection("system.users", true);
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);
adminDB.users.drop();

jsTestLog("Test that a userAdmin can't rename system.users without readWrite");
adminDB.logout();

assert(adminDB.auth("userAdmin", "password"));
res = adminDB.system.users.renameCollection("users");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);
assert.eq(5, adminDB.system.users.count());
adminDB.logout();

assert(adminDB.auth("readWriteAndUserAdmin", "password"));
assert.eq(0, adminDB.users.count());

jsTestLog("Test that even with userAdmin AND dbAdmin you CANNOT rename to/from system.users");
res = adminDB.system.users.renameCollection("users");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);
assert.eq(5, adminDB.system.users.count());

adminDB.users.drop();
adminDB.users.insert(backdoorUserDoc);
res = adminDB.users.renameCollection("system.users");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);

assert.eq(null, adminDB.system.users.findOne({user: backdoorUserDoc.user}));
assert.neq(null, adminDB.system.users.findOne({user: "userAdmin"}));
adminDB.logout();

assert(adminDB.auth("rootier", "password"));

// Test permissions against the configDB and localDB

// Start with test against inserting to and renaming collections in config and local
// as __system.
assert.commandWorked(configDB.test.insert({"a": 1}));
assert.commandWorked(configDB.test.renameCollection("test2"));

assert.commandWorked(localDB.test.insert({"a": 1}));
assert.commandWorked(localDB.test.renameCollection("test2"));
adminDB.logout();

// Test renaming collection in config with readWriteAnyDatabase
assert(adminDB.auth("readWriteAdmin", "password"));
res = configDB.test2.insert({"b": 2});
assert.writeError(res, 13, "not authorized on config to execute command");
res = configDB.test2.renameCollection("test");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);

// Test renaming collection in local with readWriteAnyDatabase
res = localDB.test2.insert({"b": 2});
assert.writeError(res, 13, "not authorized on config to execute command");
res = localDB.test2.renameCollection("test");
assert.eq(0, res.ok);
assert.eq(CodeUnauthorized, res.code);
adminDB.logout();

// Test renaming system.users collection with __system
assert(adminDB.auth("rootier", "password"));
jsTestLog("Test that with __system you CANNOT rename to/from system.users");
res = adminDB.system.users.renameCollection("users", true);
assert.eq(0, res.ok, tojson(res));
assert.eq(ErrorCodes.IllegalOperation, res.code);

// At this point, all the user documents are gone, so further activity may be unauthorized,
// depending on cluster configuration.  So, this is the end of the test.
MongoRunner.stopMongod(conn, {user: "userAdmin", pwd: "password"});
