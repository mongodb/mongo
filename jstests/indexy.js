// Tests that an index cannot be created with dropDups=true on
// a capped collection.

var coll = db.jstests_indexy;
coll.drop();

// Can create a dropDups index on non-capped collection.
var response = coll.ensureIndex({x: 1}, {dropDups: true});
assert(response == null);
coll.drop();

// Cannot create a dropDups index on non-capped collection.
db.createCollection("jstests_indexy", {capped: true, size: 1024});
coll = db.jstests_indexy;
response = coll.ensureIndex({x: 1}, {dropDups: true});
assert(response != null);
coll.drop();
