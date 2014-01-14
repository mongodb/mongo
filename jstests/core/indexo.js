// Tests that an index cannot be created with dropDups=true on
// a capped collection.

var coll = db.jstests_indexo;
coll.drop();

// Can create a dropDups index on non-capped collection.
assert.writeOK(coll.ensureIndex({x: 1}, {dropDups: true}));
coll.drop();

// Cannot create a dropDups index on non-capped collection.
db.createCollection("jstests_indexy", {capped: true, size: 1024});
coll = db.jstests_indexy;
assert.writeError(coll.ensureIndex({x: 1}, {dropDups: true}));
coll.drop();
