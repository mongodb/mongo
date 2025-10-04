// @tags: [
//   # The test runs commands that are not allowed with security token: checkShardingIndex.
//   not_allowed_with_signed_security_token,
//   requires_fastcount,
//   requires_non_retryable_writes,
//   requires_sharding,
//   no_selinux,
// ]

// -------------------------
//  CHECKSHARDINGINDEX TEST UTILS
// -------------------------

let f = db.jstests_shardingindex;
f.drop();

// -------------------------
// Case 1: all entries filled or empty should make a valid index
//

f.drop();
f.createIndex({x: 1, y: 1});
assert.eq(0, f.count(), "1. initial count should be zero");

let res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandWorked(res, "1a");

f.save({x: 1, y: 1});
assert.eq(1, f.count(), "1. count after initial insert should be 1");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandWorked(res, "1b");

// -------------------------
// Case 2: entry with null values would make an index suitable
//

f.drop();
f.createIndex({x: 1, y: 1});
assert.eq(0, f.count(), "2. initial count should be zero");

f.save({x: 1, y: 1});
f.save({x: null, y: 1});

res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandWorked(res, "2a " + tojson(res));

f.save({y: 2});
assert.eq(3, f.count(), "2. count after initial insert should be 3");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandWorked(res, "2b " + tojson(res));

// Check _id index
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {_id: 1}});
assert.commandWorked(res, "2c " + tojson(res));
assert(res.idskip, "2d " + tojson(res));

// -------------------------
// Case 3: entry with array values would make an index unsuitable
//

f.drop();
f.createIndex({x: 1, y: 1});
assert.eq(0, f.count(), "3. initial count should be zero");

f.save({x: 1, y: 1});
f.save({x: [1, 2], y: 2});

assert.eq(2, f.count(), "3. count after initial insert should be 2");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandFailed(res, "3a " + tojson(res));

f.remove({y: 2});
f.dropIndex({x: 1, y: 1});
f.createIndex({x: 1, y: 1});

assert.eq(1, f.count(), "3. count after removing array value should be 1");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandWorked(res, "3b " + tojson(res));

f.save({x: 2, y: [1, 2]});

assert.eq(2, f.count(), "3. count after adding array value should be 2");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandFailed(res, "3c " + tojson(res));

// -------------------------
// Case 4: Handles prefix shard key indexes.
//

f.drop();
f.createIndex({x: 1, y: 1, z: 1});
assert.eq(0, f.count(), "4. initial count should be zero");

f.save({x: 1, y: 1, z: 1});

assert.eq(1, f.count(), "4. count after initial insert should be 1");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1}});
assert.commandWorked(res, "4a " + tojson(res));

res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandWorked(res, "4b " + tojson(res));

f.save({x: [1, 2], y: 2, z: 2});

assert.eq(2, f.count(), "4. count after adding array value should be 2");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1}});
assert.commandFailed(res, "4c " + tojson(res));
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandFailed(res, "4d " + tojson(res));
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1, z: 1}});
assert.commandFailed(res, "4e " + tojson(res));

f.remove({y: 2});
f.dropIndex({x: 1, y: 1, z: 1});
f.createIndex({x: 1, y: 1, z: 1});

assert.eq(1, f.count(), "4. count after removing array value should be 1");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1, z: 1}});
assert.commandWorked(res, "4f " + tojson(res));

f.save({x: 3, y: [1, 2], z: 3});

assert.eq(2, f.count(), "4. count after adding array value on second key should be 2");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1}});
assert.commandFailed(res, "4g " + tojson(res));
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandFailed(res, "4h " + tojson(res));
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1, z: 1}});
assert.commandFailed(res, "4i " + tojson(res));

f.remove({x: 3});
// Necessary so that the index is no longer marked as multikey
f.dropIndex({x: 1, y: 1, z: 1});
f.createIndex({x: 1, y: 1, z: 1});

assert.eq(1, f.count(), "4. count after removing array value should be 1 again");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1, z: 1}});
assert.commandWorked(res, "4e " + tojson(res));

f.save({x: 4, y: 4, z: [1, 2]});

assert.eq(2, f.count(), "4. count after adding array value on third key should be 2");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1}});
assert.commandFailed(res, "4c " + tojson(res));
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandFailed(res, "4d " + tojson(res));
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1, z: 1}});
assert.commandFailed(res, "4e " + tojson(res));

// -------------------------
// Test error messages of checkShardingIndex failing:

// Shard key is not a prefix of index key:
f.drop();
f.createIndex({x: 1});
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {y: 1}});
assert.commandFailed(res);
assert(res.errmsg.includes("Shard key is not a prefix of index key."));

// Index key is partial:
f.drop();
f.createIndex({x: 1, y: 1}, {partialFilterExpression: {y: {$gt: 0}}});
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandFailed(res);
assert(res.errmsg.includes("Index key is partial."));

// Index key is sparse:
f.drop();
f.createIndex({x: 1, y: 1}, {sparse: true});
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandFailed(res);
assert(res.errmsg.includes("Index key is sparse."));

// Index key is multikey:
f.drop();
f.createIndex({x: 1, y: 1});
f.save({y: [1, 2, 3, 4, 5]});
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandFailed(res);
assert(res.errmsg.includes("Index key is multikey."));

// Index key has a non-simple collation:
f.drop();
f.createIndex({x: 1, y: 1}, {collation: {locale: "en"}});
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandFailed(res);
assert(res.errmsg.includes("Index has a non-simple collation."));

// Index key is sparse and index has non-simple collation:
f.drop();
f.createIndex({x: 1, y: 1}, {sparse: true, collation: {locale: "en"}});
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandFailed(res);
assert(res.errmsg.includes("Index key is sparse.") && res.errmsg.includes("Index has a non-simple collation."));

// Multiple incompatible indexes: Index key is multikey and is partial:
f.drop();
f.createIndex({x: 1, y: 1}, {name: "index_1_part", partialFilterExpression: {x: {$gt: 0}}});
f.createIndex({x: 1, y: 1}, {name: "index_2"});
f.save({y: [1, 2, 3, 4, 5]});
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandFailed(res);
assert(res.errmsg.includes("Index key is multikey.") && res.errmsg.includes("Index key is partial."));

// Multiple incompatible indexes: Index key is partial and sparse:
f.drop();
f.createIndex({x: 1, y: 1}, {name: "index_1_part", partialFilterExpression: {x: {$gt: 0}}});
f.createIndex({x: 1, y: 1}, {name: "index_2_sparse", sparse: true});
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.commandFailed(res);
assert(res.errmsg.includes("Index key is partial.") && res.errmsg.includes("Index key is sparse."));

print("PASSED");
