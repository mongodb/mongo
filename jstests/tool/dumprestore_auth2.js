// dumprestore_auth2.js
// Tests that mongodump and mongorestore properly handle access control information

t = new ToolTest( "dumprestore_auth2" );

t.startDB( "foo" );

db = t.db.getSiblingDB("admin")

db.addUser({user: 'user',pwd: 'password', roles: jsTest.basicUserRoles});
db.addRole({role: 'role', roles: [], privileges:[]});

assert.eq(1, db.system.users.count(), "setup")
assert.eq(2, db.system.indexes.count({ns: "admin.system.users"}), "setup2")
assert.eq(1, db.system.roles.count(), "setup3")
assert.eq(2, db.system.indexes.count({ns: "admin.system.roles"}), "setup4")
assert.eq(1, db.system.version.count());
var versionDoc = db.system.version.findOne();

t.runTool( "dump" , "--out" , t.ext );

db.dropDatabase()

assert.eq(0, db.system.users.count(), "didn't drop users")
assert.eq(0, db.system.roles.count(), "didn't drop roles")
assert.eq(0, db.system.version.count(), "didn't drop version");
assert.eq(0, db.system.indexes.count(), "didn't drop indexes")

t.runTool("restore", "--dir", t.ext)

assert.soon("db.system.users.findOne()", "no data after restore");
assert.eq(1, db.system.users.find({user:'user'}).count(), "didn't restore users")
assert.eq(2, db.system.indexes.count({ns: "admin.system.users"}), "didn't restore user indexes")
assert.eq(1, db.system.roles.find({role:'role'}).count(), "didn't restore roles")
assert.eq(2, db.system.indexes.count({ns: "admin.system.roles"}), "didn't restore role indexes")
assert.eq(1, db.system.version.count(), "didn't restore version");
assert.docEq(versionDoc, db.system.version.findOne(), "version doc wasn't restored properly");

db.dropUser('user')
db.addUser({user: 'user2', pwd: 'password2', roles: jsTest.basicUserRoles});
db.dropRole('role')
db.addRole({role: 'role2', roles: [], privileges:[]});

t.runTool("restore", "--dir", t.ext, "--drop")

assert.soon("1 == db.system.users.find({user:'user'}).count()", "didn't restore users 2")
assert.eq(0, db.system.users.find({user:'user2'}).count(), "didn't drop users")
// assert.eq(0, db.system.roles.find({role:'role2'}).count(), "didn't drop roles") // SERVER-11461
assert.eq(1, db.system.roles.find({role:'role'}).count(), "didn't restore roles")
assert.eq(2, db.system.indexes.count({ns: "admin.system.users"}), "didn't maintain user indexes")
assert.eq(2, db.system.indexes.count({ns: "admin.system.roles"}), "didn't maintain role indexes")
assert.eq(1, db.system.version.count(), "didn't restore version");
assert.docEq(versionDoc, db.system.version.findOne(), "version doc wasn't restored properly");

t.stop();

