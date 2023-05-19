// The test runs commands that are not allowed with security token: dbhash.
// @tags: [
//   not_allowed_with_security_token,
//   assumes_superuser_permissions,
// ]

let mydb = db.getSiblingDB("config");

let t = mydb.foo;
t.drop();

assert.commandWorked(t.insert({x: 1}));
let res1 = mydb.runCommand("dbhash");
let res2 = mydb.runCommand("dbhash");
assert.commandWorked(res1);
assert.commandWorked(res2);
assert.eq(res1.collections.foo, res2.collections.foo);

assert.commandWorked(t.insert({x: 2}));
let res3 = mydb.runCommand("dbhash");
assert.commandWorked(res3);
assert.neq(res1.collections.foo, res3.collections.foo);

// Validate dbHash with an empty database does not trigger an fassert/invariant
assert.commandFailed(db.runCommand({"dbhash": ""}));
