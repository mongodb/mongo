// Check db name duplication constraint SERVER-2111

a = db.getSisterDB("dbcasetest_dbnamea");
b = db.getSisterDB("dbcasetest_dbnameA");

a.dropDatabase();
b.dropDatabase();

assert.writeOK(a.foo.save({x: 1}));

res = b.foo.save({x: 1});
assert.writeError(res);

assert.neq(-1, db.getMerizo().getDBNames().indexOf(a.getName()));
assert.eq(-1, db.getMerizo().getDBNames().indexOf(b.getName()));
printjson(db.getMerizo().getDBs().databases);

a.dropDatabase();
b.dropDatabase();

ai = db.getMerizo().getDBNames().indexOf(a.getName());
bi = db.getMerizo().getDBNames().indexOf(b.getName());
// One of these dbs may exist if there is a slave active, but they must
// not both exist.
assert(ai == -1 || bi == -1);
printjson(db.getMerizo().getDBs().databases);
