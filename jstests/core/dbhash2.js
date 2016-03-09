
mydb = db.getSisterDB("config");

t = mydb.foo;
t.drop();

t.insert({x: 1});
res1 = mydb.runCommand("dbhash");
assert(res1.fromCache.indexOf("config.foo") == -1);

res2 = mydb.runCommand("dbhash");
assert(res2.fromCache.indexOf("config.foo") >= 0);
assert.eq(res1.collections.foo, res2.collections.foo);

t.insert({x: 2});
res3 = mydb.runCommand("dbhash");
assert(res3.fromCache.indexOf("config.foo") < 0);
assert.neq(res1.collections.foo, res3.collections.foo);

// Validate dbHash with an empty database does not trigger an fassert/invariant
assert.commandFailed(db.runCommand({"dbhash": ""}));
