// Test creation of compound indexes with special index types.

var coll = db.index_plugins;
coll.drop();

// Test building special index types on a single field.

assert.commandWorked(coll.ensureIndex({a: "hashed"}));
coll.dropIndexes();
assert.commandWorked(coll.ensureIndex({a: "2d"}));
coll.dropIndexes();
assert.commandWorked(coll.ensureIndex({a: "2dsphere"}));
coll.dropIndexes();
assert.commandWorked(coll.ensureIndex({a: "text"}));
coll.dropIndexes();

assert.commandFailed(coll.ensureIndex({a: "geoHaystack"}, {bucketSize: 1}));  // compound required

// Test compounding special index types with an ascending index.

assert.commandWorked(coll.ensureIndex({a: "2dsphere", b: 1}));
coll.dropIndexes();
assert.commandWorked(coll.ensureIndex({a: 1, b: "2dsphere"}));
coll.dropIndexes();

assert.commandWorked(coll.ensureIndex({a: "text", b: 1}));
coll.dropIndexes();
assert.commandWorked(coll.ensureIndex({a: 1, b: "text"}));
coll.dropIndexes();

assert.commandWorked(coll.ensureIndex({a: "2d", b: 1}));
coll.dropIndexes();
assert.commandFailed(coll.ensureIndex({a: 1, b: "2d"}));  // unsupported

assert.commandWorked(coll.ensureIndex({a: "geoHaystack", b: 1}, {bucketSize: 1}));
coll.dropIndexes();
assert.commandFailed(coll.ensureIndex({a: 1, b: "geoHaystack"}, {bucketSize: 1}));  // unsupported

assert.commandFailed(coll.ensureIndex({a: "hashed", b: 1}));  // unsupported
assert.commandFailed(coll.ensureIndex({a: 1, b: "hashed"}));  // unsupported

// Test compound index where multiple fields have same special index type.

assert.commandWorked(coll.ensureIndex({a: "2dsphere", b: "2dsphere"}));
coll.dropIndexes();
assert.commandWorked(coll.ensureIndex({a: "text", b: "text"}));
coll.dropIndexes();

assert.commandFailed(coll.ensureIndex({a: "2d", b: "2d"}));                  // unsupported
assert.commandFailed(coll.ensureIndex({a: "geoHaystack", b: "geoHaystack"},  // unsupported
                                      {bucketSize: 1}));

assert.commandFailed(coll.ensureIndex({a: "hashed", b: "hashed"}));  // unsupported

// Test compounding different special index types with each other.

assert.commandFailed(coll.ensureIndex({a: "2d", b: "hashed"}));         // unsupported
assert.commandFailed(coll.ensureIndex({a: "hashed", b: "2dsphere"}));   // unsupported
assert.commandFailed(coll.ensureIndex({a: "2dsphere", b: "text"}));     // unsupported
assert.commandFailed(coll.ensureIndex({a: "text", b: "geoHaystack"}));  // unsupported
assert.commandFailed(coll.ensureIndex({a: "geoHaystack", b: "2d"},      // unsupported
                                      {bucketSize: 1}));
