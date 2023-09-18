// @tags: [
//   assumes_superuser_permissions,
//   requires_non_retryable_commands,
//   requires_fcv_72,
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

// system.foo and system.users aren't in the allowlist so they can't be renamed to or from
assert.commandFailed(coll.renameCollection(systemFoo.getName()));
assert.commandFailed(systemFoo.renameCollection(coll.getName()));

assert.commandFailed(coll.renameCollection(systemUsers.getName()));
assert.commandFailed(systemUsers.renameCollection(coll.getName()));
