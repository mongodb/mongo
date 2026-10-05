// Check that a database name differing only in case from an existing database is rejected
// (SERVER-2111).
// @tags: [
//   # The simulate_atlas_proxy override in multi-tenant mode might choose different
//   # prefixes for each sibling DB in this test.
//   multiple_tenants_incompatible,
//   # Can't have 2 databases that just differ on case
//   assumes_no_implicit_collection_creation_on_get_collection,
// ]

const a = db.getSiblingDB("dbcasetest_dbnamea");
const b = db.getSiblingDB("dbcasetest_dbnameA");

assert.commandWorked(a.dropDatabase());
assert.commandWorked(b.dropDatabase());

assert.commandWorked(a.foo.insert({x: 1}));
assert.commandFailedWithCode(b.foo.insert({x: 1}), ErrorCodes.DatabaseDifferCase);

const dbNames = db.getMongo().getDBNames();
assert.contains(a.getName(), dbNames);
assert(!dbNames.includes(b.getName()), dbNames);

assert.commandWorked(a.dropDatabase());
assert.commandWorked(b.dropDatabase());

// One of these dbs may exist if there is a secondary active, but they must not both exist.
const after = db.getMongo().getDBNames();
assert(!after.includes(a.getName()) || !after.includes(b.getName()), after);
