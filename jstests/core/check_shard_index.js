// -------------------------
//  CHECKSHARDINGINDEX TEST UTILS
// -------------------------

f = db.jstests_shardingindex;
f.drop();

// -------------------------
// Case 1: all entries filled or empty should make a valid index
//

f.drop();
f.ensureIndex({x: 1, y: 1});
assert.eq(0, f.count(), "1. initial count should be zero");

res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.eq(true, res.ok, "1a");

f.save({x: 1, y: 1});
assert.eq(1, f.count(), "1. count after initial insert should be 1");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.eq(true, res.ok, "1b");

// -------------------------
// Case 2: entry with null values would make an index unsuitable
//

f.drop();
f.ensureIndex({x: 1, y: 1});
assert.eq(0, f.count(), "2. initial count should be zero");

f.save({x: 1, y: 1});
f.save({x: null, y: 1});

res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.eq(true, res.ok, "2a " + tojson(res));

f.save({y: 2});
assert.eq(3, f.count(), "2. count after initial insert should be 3");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.eq(false, res.ok, "2b " + tojson(res));

// Check _id index
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {_id: 1}});
assert.eq(true, res.ok, "2c " + tojson(res));
assert(res.idskip, "2d " + tojson(res));

// -------------------------
// Case 3: entry with array values would make an index unsuitable
//

f.drop();
f.ensureIndex({x: 1, y: 1});
assert.eq(0, f.count(), "3. initial count should be zero");

f.save({x: 1, y: 1});
f.save({x: [1, 2], y: 2});

assert.eq(2, f.count(), "3. count after initial insert should be 2");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.eq(false, res.ok, "3a " + tojson(res));

f.remove({y: 2});
f.reIndex();

assert.eq(1, f.count(), "3. count after removing array value should be 1");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.eq(true, res.ok, "3b " + tojson(res));

f.save({x: 2, y: [1, 2]});

assert.eq(2, f.count(), "3. count after adding array value should be 2");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.eq(false, res.ok, "3c " + tojson(res));

// -------------------------
// Case 4: Handles prefix shard key indexes.
//

f.drop();
f.ensureIndex({x: 1, y: 1, z: 1});
assert.eq(0, f.count(), "4. initial count should be zero");

f.save({x: 1, y: 1, z: 1});

assert.eq(1, f.count(), "4. count after initial insert should be 1");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1}});
assert.eq(true, res.ok, "4a " + tojson(res));

res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.eq(true, res.ok, "4b " + tojson(res));

f.save({x: [1, 2], y: 2, z: 2});

assert.eq(2, f.count(), "4. count after adding array value should be 2");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1}});
assert.eq(false, res.ok, "4c " + tojson(res));
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.eq(false, res.ok, "4d " + tojson(res));
res = db.runCommand(
    {checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1, z: 1}});
assert.eq(false, res.ok, "4e " + tojson(res));

f.remove({y: 2});
f.reIndex();

assert.eq(1, f.count(), "4. count after removing array value should be 1");
res = db.runCommand(
    {checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1, z: 1}});
assert.eq(true, res.ok, "4f " + tojson(res));

f.save({x: 3, y: [1, 2], z: 3});

assert.eq(2, f.count(), "4. count after adding array value on second key should be 2");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1}});
assert.eq(false, res.ok, "4g " + tojson(res));
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.eq(false, res.ok, "4h " + tojson(res));
res = db.runCommand(
    {checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1, z: 1}});
assert.eq(false, res.ok, "4i " + tojson(res));

f.remove({x: 3});
f.reIndex();  // Necessary so that the index is no longer marked as multikey

assert.eq(1, f.count(), "4. count after removing array value should be 1 again");
res = db.runCommand(
    {checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1, z: 1}});
assert.eq(true, res.ok, "4e " + tojson(res));

f.save({x: 4, y: 4, z: [1, 2]});

assert.eq(2, f.count(), "4. count after adding array value on third key should be 2");
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1}});
assert.eq(false, res.ok, "4c " + tojson(res));
res = db.runCommand({checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1}});
assert.eq(false, res.ok, "4d " + tojson(res));
res = db.runCommand(
    {checkShardingIndex: "test.jstests_shardingindex", keyPattern: {x: 1, y: 1, z: 1}});
assert.eq(false, res.ok, "4e " + tojson(res));

print("PASSED");
