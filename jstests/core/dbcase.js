// Check db name duplication constraint SERVER-2111
// @tags: [
//   # The inject_tenant_prefix override in shard split mode might choose different
//   # prefixes for each sibling DB in this test.
//   shard_split_incompatible,
// ]

a = db.getSiblingDB("dbcasetest_dbnamea");
b = db.getSiblingDB("dbcasetest_dbnameA");

a.dropDatabase();
b.dropDatabase();

assert.commandWorked(a.foo.save({x: 1}));

res = b.foo.save({x: 1});
assert.writeError(res);

assert.neq(-1, db.getMongo().getDBNames().indexOf(a.getName()));
assert.eq(-1, db.getMongo().getDBNames().indexOf(b.getName()));
printjson(db.getMongo().getDBs().databases);

a.dropDatabase();
b.dropDatabase();

ai = db.getMongo().getDBNames().indexOf(a.getName());
bi = db.getMongo().getDBNames().indexOf(b.getName());
// One of these dbs may exist if there is a secondary active, but they must
// not both exist.
assert(ai == -1 || bi == -1);
printjson(db.getMongo().getDBs().databases);
