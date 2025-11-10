/**
 * Reproduces a cross-shard $lookup where the foreign collection is on the primary shard
 * and the local collection on a different shard, which forces mongos to serialize a network
 * request for $lookup.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const testName = "lookup_foreign_local_different_shards";
const st = new ShardingTest({mongos: 1, shards: 4, config: 1});

const testDB = st.getDB(testName);
const dbAtShard0 = st.shard0.getDB(testName);
const foreignCollName = jsTestName() + "_foreign";
const localCollName = jsTestName();
assert(st.adminCommand({enableSharding: testName, primaryShard: st.shard0.shardName}));

// Create the foreign collection on the primary shard (shard0).
const foreignColl = dbAtShard0[foreignCollName];
foreignColl.drop();
const data = [
    {"t": new Date(), "shard": "shard1"},
    {"t": new Date("2020-01-01"), "shard": "shard1"},
    {"t": new Date(), "shard": "shard1"},
    {"t": new Date("2017-01-01"), "shard": "shard1"},
    {"t": new Date(), "shard": "shard1"},
    {"t": new Date("2016-01-01"), "shard": "shard1"},
];
assert.commandWorked(foreignColl.insertMany(data));

// Create the local collection on a different shard (shard1).
const localColl = testDB[localCollName];
localColl.drop();
assert.commandWorked(localColl.insert({"t": new Date(), join: "shard1"}));
assert.commandWorked(localColl.createIndex({join: 1}));
assert(st.s.adminCommand({shardCollection: localColl.getFullName(), key: {join: 1}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: localColl.getFullName(), find: {join: "shard1"}, to: st.shard1.shardName}),
);

// If rawData is attached multiple times to the network request, the following query will fail.
const results = localColl
    .aggregate([{$lookup: {from: foreignCollName, localField: "join", foreignField: "shard", as: "things"}}], {
        rawData: true,
    })
    .toArray();

assert.eq(1, results.length);
assert.eq(data.length, results[0].things.length);

st.stop();
