// Multiversion scenarios for explain.

var explain, testDb, coll, options, st;

//
// Standalone
//

// Ensure that a 2.8 shell can explain .find() against a 2.6 mongod
var mongod26 = MongoRunner.runMongod({binVersion: "2.6"});

testDb = mongod26.getDB("explain-multiversion");
testDb.dropDatabase();
coll = testDb.standalone;
coll.drop();

coll.insert({_id: 1, a: 1});

// Make sure that we get the expected 2.6-style explain.
explain = coll.find().explain();
assert.eq(explain.cursor, "BasicCursor");
assert.eq(explain.n, 1);

// Also test the new .explain.find().finish() style.
explain = coll.explain().find().finish();
assert.eq(explain.cursor, "BasicCursor");
assert.eq(explain.n, 1);

MongoRunner.stopMongod(mongod26.port);

//
// Sharded cluster
//   --2.6 mongos
//   --2.6 config
//   --two shards, both version 2.6
//

options = {
    mongosOptions: {binVersion: "2.6"},
    configOptions: {binVersion: "2.6"},
    shardOptions: {binVersion: "2.6"}
}

st = new ShardingTest({shards: 2, other: options});

testDb = st.s.getDB("explain-multiversion");
testDb.dropDatabase();
coll = testDb.standalone;
coll.drop();

assert.commandWorked(testDb.adminCommand({enableSharding: testDb.getName()}));
st.ensurePrimaryShard(testDb.getName(), 'shard0001');
testDb.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}});

coll.insert({_id: 1, a: 1});

// Ensure that explain gives us 2.6-style sharded output.
explain = coll.find().explain();
assert("clusteredType" in explain);
assert("shards" in explain);
assert.eq(explain.n, 1);
assert.gte(explain.nscanned, 1);
assert.gte(explain.nscannedObjects, 1);

// Also test .explain().find().finish() syntax.
explain = coll.explain().find().finish();
assert("clusteredType" in explain);
assert("shards" in explain);
assert.eq(explain.n, 1);
assert.gte(explain.nscanned, 1);
assert.gte(explain.nscannedObjects, 1);

st.stop();

//
// Sharded cluster
//   --2.8 mongos
//   --2.8 config
//   --two shards, both 2.6
//

options = {
    mongosOptions: {binVersion: "2.8"},
    configOptions: {binVersion: "2.8"},
    shardOptions: {binVersion: "2.6"}
}

st = new ShardingTest({shards: 2, other: options});

testDb = st.s.getDB("explain-multiversion");
testDb.dropDatabase();
coll = testDb.standalone;
coll.drop();

assert.commandWorked(testDb.adminCommand({enableSharding: testDb.getName()}));
st.ensurePrimaryShard(testDb.getName(), 'shard0001');
testDb.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}});

coll.insert({_id: 1, a: 1});

// Ensure that explain gives us 2.6-style sharded output.
explain = coll.find().explain();
assert("clusteredType" in explain);
assert("shards" in explain);
assert.eq(explain.n, 1);
assert.gte(explain.nscanned, 1);
assert.gte(explain.nscannedObjects, 1);

// Also test .explain().find().finish() syntax.
explain = coll.explain().find().finish();
assert("clusteredType" in explain);
assert("shards" in explain);
assert.eq(explain.n, 1);
assert.gte(explain.nscanned, 1);
assert.gte(explain.nscannedObjects, 1);

st.stop();

//
// Sharded cluster
//   --2.8 mongos
//   --2.8 config
//   --two shards, one 2.6 and one 2.8
//

options = {
    mongosOptions: {binVersion: "2.8"},
    configOptions: {binVersion: "2.8"},
    shardOptions: {binVersion: ["2.6", "2.8"]}
}

st = new ShardingTest({shards: 2, other: options});

testDb = st.s.getDB("explain-multiversion");
testDb.dropDatabase();
coll = testDb.standalone;
coll.drop();

assert.commandWorked(testDb.adminCommand({enableSharding: testDb.getName()}));
st.ensurePrimaryShard(testDb.getName(), 'shard0001');
testDb.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}});

// Disable the balancer and pre-split in order to ensure chunks on both shards.
st.stopBalancer();
assert.commandWorked(testDb.adminCommand({split: coll.getFullName(), middle: {_id: 5}}));
assert.commandWorked(testDb.adminCommand({moveChunk: coll.getFullName(),
                                          find: {_id: 5},
                                          to: "shard0000"}));
coll.insert({_id: 1});
coll.insert({_id: 10});

// Explain should aggregate mixed-version output in a 2.6-style way.
explain = coll.find().explain();
assert("clusteredType" in explain);
assert("shards" in explain);
assert.eq(explain.n, 2);
assert.gte(explain.nscanned, 1);
assert.gte(explain.nscannedObjects, 1);

// Also test .explain().find().finish() syntax.
explain = coll.explain().find().finish();
assert("clusteredType" in explain);
assert("shards" in explain);
assert.eq(explain.n, 2);
assert.gte(explain.nscanned, 1);
assert.gte(explain.nscannedObjects, 1);

st.stop();
