// @tags: [
//   assumes_superuser_permissions,
//   requires_non_retryable_commands,
// ]

// SERVER-12591: prevent renaming to arbitrary system collections.

var testdb =
    db.getSiblingDB("rename8");  // to avoid breaking other tests when we touch system.users
var coll = testdb.rename8;
var systemFoo = testdb.system.foo;
var systemUsers = testdb.system.users;

systemFoo.drop();
systemUsers.drop();
coll.drop();
coll.insert({});

// system.foo isn't in the whitelist so it can't be renamed to or from
assert.commandFailed(coll.renameCollection(systemFoo.getName()));
assert.commandFailed(systemFoo.renameCollection(coll.getName()));

// system.users is whitelisted so these should work
assert.commandWorked(coll.renameCollection(systemUsers.getName()));
assert.commandWorked(systemUsers.renameCollection(coll.getName()));
