//
// Tests valid cases for creation of 2dsphere index
//

var coll = db.getCollection("twodspherevalid");

// Valid index
coll.drop();
assert.eq(undefined, coll.ensureIndex({geo : "2dsphere", other : 1}));

// Valid index
coll.drop();
assert.eq(undefined, coll.ensureIndex({geo : "2dsphere", other : 1, geo2 : "2dsphere"}));

// Invalid index, using hash with 2dsphere
coll.drop();
assert.neq(undefined, coll.ensureIndex({geo : "2dsphere", other : "hash"}).err);

// Invalid index, using 2d with 2dsphere
coll.drop();
assert.neq(undefined, coll.ensureIndex({geo : "2dsphere", other : "2d"}).err);

jsTest.log("Success!");

// Ensure the empty collection is gone, so that small_oplog passes.
coll.drop();
