// @tags: [
//     # dbhash command is not available on embedded
//     incompatible_with_embedded,
// ]

mydb = db.getSisterDB("config");

t = mydb.foo;
t.drop();

assert.commandWorked(t.insert({x: 1}));
res1 = mydb.runCommand("dbhash");
res2 = mydb.runCommand("dbhash");
assert.commandWorked(res1);
assert.commandWorked(res2);
assert.eq(res1.collections.foo, res2.collections.foo);

assert.commandWorked(t.insert({x: 2}));
res3 = mydb.runCommand("dbhash");
assert.commandWorked(res3);
assert.neq(res1.collections.foo, res3.collections.foo);

// Validate dbHash with an empty database does not trigger an fassert/invariant
assert.commandFailed(db.runCommand({"dbhash": ""}));
