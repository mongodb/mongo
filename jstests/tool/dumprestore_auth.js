// dumprestore_auth.js

t = new ToolTest("dumprestore_auth", { auth : "" });

c = t.startDB("foo");

adminDB = c.getDB().getSiblingDB('admin');
adminDB.addUser('admin', 'password', jsTest.adminUserRoles);
adminDB.auth('admin','password');

assert.eq(0 , c.count() , "setup1");
c.save({ a : 22 });
assert.eq(1 , c.count() , "setup2");

t.runTool("dump" , "--out" , t.ext, "--username", "admin", "--password", "password");

c.drop();
assert.eq(0 , c.count() , "after drop");

t.runTool("restore" , "--dir" , t.ext); // Should fail
assert.eq(0 , c.count() , "after restore without auth");

t.runTool("restore" , "--dir" , t.ext, "--username", "admin", "--password", "password");
assert.soon("c.findOne()" , "no data after sleep");
assert.eq(1 , c.count() , "after restore 2");
assert.eq(22 , c.findOne().a , "after restore 2");

t.stop();
