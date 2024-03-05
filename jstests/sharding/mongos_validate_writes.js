//
// Tests that mongos validating writes when stale does not DOS config servers
//
// Note that this is *unsafe* with broadcast removes and updates
//
// @tags: [
//   # Test doesn't start enough mongods to have num_mongos routers
//   temp_disabled_embedded_router,
// ]
import {
    WriteWithoutShardKeyTestUtil
} from "jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js";

var st = new ShardingTest({shards: 2, mongos: 3, other: {shardOptions: {verbose: 2}}});

var mongos = st.s0;
var staleMongosA = st.s1;
var staleMongosB = st.s2;

var admin = mongos.getDB("admin");
const kDbName = "foo";
assert.commandWorked(
    admin.runCommand({enableSharding: kDbName, primaryShard: st.shard1.shardName}));
var coll = mongos.getCollection(kDbName + ".bar");
var staleCollA = staleMongosA.getCollection(coll + "");
var staleCollB = staleMongosB.getCollection(coll + "");

coll.createIndex({a: 1});

// Shard the collection on {a: 1} and move one chunk to another shard. Updates need to be across
// two shards to trigger an error, otherwise they are versioned and will succeed after raising a
// StaleConfig error.
st.shardColl(coll, {a: 1}, {a: 0}, {a: 1}, coll.getDB(), true);

// Let the stale mongos see the collection state
staleCollA.findOne();
staleCollB.findOne();

// Change the collection sharding state
coll.drop();
coll.createIndex({b: 1});
st.shardColl(coll, {b: 1}, {b: 0}, {b: 1}, coll.getDB(), true);

// Make sure that we can successfully insert, even though we have stale state
assert.commandWorked(staleCollA.insert({b: "b"}));

// Change the collection sharding state
coll.drop();
coll.createIndex({c: 1});
st.shardColl(coll, {c: 1}, {c: 0}, {c: 1}, coll.getDB(), true);

// Make sure we can successfully upsert, even though we have stale state
assert.commandWorked(staleCollA.update({c: "c"}, {c: "c"}, true));

if (!WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(coll.getDB())) {
    // When updateOneWithoutShardKey feature flag is disabled, make sure we unsuccessfully upsert
    // with old info.
    assert.commandFailedWithCode(staleCollB.update({b: "b"}, {b: "b"}, true),
                                 ErrorCodes.ShardKeyNotFound);
}

// Change the collection sharding state
coll.drop();
coll.createIndex({d: 1});
st.shardColl(coll, {d: 1}, {d: 0}, {d: 1}, coll.getDB(), true);

// Make sure we can successfully update, even though we have stale state
assert.commandWorked(coll.insert({d: "d"}));

assert.commandWorked(staleCollA.update({d: "d"}, {$set: {x: "x"}}, false, false));
assert.eq(staleCollA.findOne().x, "x");

if (!WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(coll.getDB())) {
    // When updateOneWithoutShardKey feature flag is disabled, make sure we unsuccessfully update
    // with old info.
    assert.commandFailedWithCode(staleCollB.update({c: "c"}, {$set: {x: "y"}}, false, false),
                                 ErrorCodes.InvalidOptions);
}

assert.eq(staleCollB.findOne().x, "x");

// Change the collection sharding state
coll.drop();
coll.createIndex({e: 1});
// Deletes need to be across two shards to trigger an error.
st.shardColl(coll, {e: 1}, {e: 0}, {e: 1}, coll.getDB(), true);

// Make sure we can successfully remove, even though we have stale state
assert.commandWorked(coll.insert({e: "e"}));

assert.commandWorked(staleCollA.remove({e: "e"}, true));
assert.eq(null, staleCollA.findOne());

if (!WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(coll.getDB())) {
    // When updateOneWithoutShardKey feature flag is disabled, make sure we unsuccessfully delete
    // with old info.
    assert.commandFailedWithCode(staleCollB.remove({d: "d"}, true), ErrorCodes.ShardKeyNotFound);
}

st.stop();