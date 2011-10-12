// dumprestore5.js

t = new ToolTest( "dumprestore5" );

t.startDB( "foo" );

db = t.db

db.addUser('user','password')

assert.eq(1, db.system.users.count(), "setup")
assert.eq(1, db.system.indexes.count(), "setup2")

t.runTool( "dump" , "--out" , t.ext );

db.dropDatabase()

assert.eq(0, db.system.users.count(), "didn't drop users")
assert.eq(0, db.system.indexes.count(), "didn't drop indexes")

t.runTool("restore", "--dir", t.ext)

assert.soon("db.system.users.findOne()", "no data after restore");
assert.eq(1, db.system.users.find({user:'user'}).count(), "didn't restore users")
assert.eq(1, db.system.indexes.count(), "didn't restore indexes")

db.removeUser('user')
db.addUser('user2', 'password2')

t.runTool("restore", "--dir", t.ext, "--drop")

assert.soon("1 == db.system.users.find({user:'user'}).count()", "didn't restore users 2")
assert.eq(0, db.system.users.find({user:'user2'}).count(), "didn't drop users")
assert.eq(1, db.system.indexes.count(), "didn't maintain indexes")

t.stop();

