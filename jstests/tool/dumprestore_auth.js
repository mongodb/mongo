// dumprestore_auth.js

t = new ToolTest("dumprestore_auth", { auth : "" });

c = t.startDB("foo");
var dbName = c.getDB().toString();
print("DB is ",dbName);

adminDB = c.getDB().getSiblingDB('admin');
adminDB.createUser({user: 'admin', pwd: 'password', roles: ['root']});
adminDB.auth('admin','password');
adminDB.createUser({user: 'backup', pwd: 'password', roles: ['backup']});
adminDB.createUser({user: 'restore', pwd: 'password', roles: ['restore']});

// Add user defined roles & users with those roles
var testUserAdmin = c.getDB().getSiblingDB(dbName);
var backupActions = ["find","listCollections"];
testUserAdmin.createRole({role: "backupFoo",
   privileges: [{resource: {db: dbName, collection: "foo"}, actions:backupActions},
                {resource: {db: dbName, collection: "system.indexes"},
                 actions: backupActions},
                {resource: {db: dbName, collection: "" },
                 actions: backupActions},
                {resource: {db: dbName, collection: "system.namespaces"},
                 actions: backupActions}],
   roles: []});
testUserAdmin.createUser({user: 'backupFoo', pwd: 'password', roles: ['backupFoo']});

var restoreActions = ["collMod", "createCollection","createIndex","dropCollection","insert"];
var restoreActionsFind = restoreActions;
restoreActionsFind.push("find");
testUserAdmin.createRole({role: "restoreChester",
       privileges: [{resource: {db: dbName, collection: "chester"}, actions: restoreActions},
                {resource: {db: dbName, collection: "system.indexes"},
                 actions: restoreActions},
                {resource: {db: dbName, collection: "system.namespaces"},
                 actions: restoreActionsFind}],
       roles: []});
testUserAdmin.createRole({role: "restoreFoo",
       privileges: [{resource: {db: dbName, collection: "foo"}, actions:restoreActions},
                {resource: {db: dbName, collection: "system.indexes"},
                 actions: restoreActions},
                {resource: {db: dbName, collection: "system.namespaces"},
                 actions: restoreActionsFind}],
       roles: []});
testUserAdmin.createUser({user: 'restoreChester', pwd: 'password', roles: ['restoreChester']});
testUserAdmin.createUser({user: 'restoreFoo', pwd: 'password', roles: ['restoreFoo']});

var sysUsers = adminDB.system.users.count();
assert.eq(0 , c.count() , "setup1");
c.save({ a : 22 });
assert.eq(1 , c.count() , "setup2");

assert.commandWorked(c.runCommand("collMod", {usePowerOf2Sizes: false}));
assert.eq(0, c.getDB().system.namespaces.findOne(
{name: c.getFullName()}).options.flags, "find namespaces 1");

t.runTool("dump" , "--out" , t.ext, "--username", "backup", "--password", "password");

c.drop();
assert.eq(0 , c.count() , "after drop");

// Restore should fail without user & pass
t.runTool("restore" , "--dir" , t.ext);
assert.eq(0 , c.count() , "after restore without auth");

// Restore should pass with authorized user
t.runTool("restore" , "--dir" , t.ext, "--username", "restore", "--password", "password");
assert.soon("c.findOne()" , "no data after sleep");
assert.eq(1 , c.count() , "after restore 2");
assert.eq(22 , c.findOne().a , "after restore 2");
assert.eq(0, c.getDB().system.namespaces.findOne(
{name: c.getFullName()}).options.flags, "find namespaces 2");
assert.eq(sysUsers, adminDB.system.users.count());

// Ddump & restore DB/colection with user defined roles
t.runTool("dump" , "--out" , t.ext, "--username", "backupFoo", "--password", "password",
          "--db", dbName, "--collection", "foo");
c.drop();
assert.eq(0 , c.count() , "after drop");

// Restore with wrong user
t.runTool("restore" , "--username", "restoreChester", "--password", "password",
          "--db", dbName, "--collection", "foo", t.ext+dbName+"/foo.bson");
assert.eq(0 , c.count() , "after restore with wrong user");

// Restore with proper user
t.runTool("restore" , "--username", "restoreFoo", "--password", "password",
          "--db", dbName, "--collection", "foo", t.ext+dbName+"/foo.bson");
assert.soon("c.findOne()" , "no data after sleep");
assert.eq(1 , c.count() , "after restore 3");
assert.eq(22 , c.findOne().a , "after restore 3");
assert.eq(0, c.getDB().system.namespaces.findOne(
{name: c.getFullName()}).options.flags, "find namespaces 3");
assert.eq(sysUsers, adminDB.system.users.count());

t.stop();
