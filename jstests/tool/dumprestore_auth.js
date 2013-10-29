// dumprestore_auth.js

t = new ToolTest("dumprestore_auth", { auth : "" });

c = t.startDB("foo");

adminDB = c.getDB().getSiblingDB('admin');
adminDB.addUser({user: 'admin', pwd: 'password', roles: ['root']});
adminDB.auth('admin','password');
adminDB.addUser({user: 'backup', pwd: 'password', roles: ['backup']});
adminDB.addUser({user: 'restore', pwd: 'password', roles: ['restore']});

assert.eq(0 , c.count() , "setup1");
c.save({ a : 22 });
assert.eq(1 , c.count() , "setup2");
assert.commandWorked(c.runCommand("collMod", {usePowerOf2Sizes: true}));
assert.eq(1, c.getDB().system.namespaces.findOne({name: c.getFullName()}).options.flags);

t.runTool("dump" , "--out" , t.ext, "--username", "backup", "--password", "password");

c.drop();
assert.eq(0 , c.count() , "after drop");

t.runTool("restore" , "--dir" , t.ext); // Should fail
assert.eq(0 , c.count() , "after restore without auth");

t.runTool("restore" , "--dir" , t.ext, "--username", "restore", "--password", "password");
assert.soon("c.findOne()" , "no data after sleep");
assert.eq(1 , c.count() , "after restore 2");
assert.eq(22 , c.findOne().a , "after restore 2");
assert.eq(1, c.getDB().system.namespaces.findOne({name: c.getFullName()}).options.flags);
assert.eq(3, adminDB.system.users.count());

t.stop();
