// SERVER-12591: prevent renaming to arbitrary system collections.

var testdb =
    db.getSiblingDB("rename8");  // to avoid breaking other tests when we touch system.users
var coll = testdb.rename8;
var systemNamespaces = testdb.system.namespaces;
var systemFoo = testdb.system.foo;
var systemUsers = testdb.system.users;

systemFoo.drop();
systemUsers.drop();
coll.drop();
coll.insert({});

// system.foo isn't in the whitelist so it can't be renamed to or from
assert.commandFailed(coll.renameCollection(systemFoo.getName()));
assert.commandFailed(systemFoo.renameCollection(coll.getName()));

// same with system.namespaces, even though it does exist
assert.commandFailed(coll.renameCollection(systemNamespaces.getName()));
assert.commandFailed(coll.renameCollection(systemNamespaces.getName(), /*dropTarget*/ true));
assert.commandFailed(systemNamespaces.renameCollection(coll.getName()));

// system.users is whitelisted so these should work
assert.commandWorked(coll.renameCollection(systemUsers.getName()));
assert.commandWorked(systemUsers.renameCollection(coll.getName()));
