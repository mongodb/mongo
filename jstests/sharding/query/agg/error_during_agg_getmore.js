// This test was designed to reproduce SERVER-31475. It issues sharded aggregations with an error
// returned from one shard, and a delayed response from another shard.
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, useBridge: true});

const mongosDB = st.s0.getDB(jsTestName());
const mongosColl = mongosDB[jsTestName()];

assert.commandWorked(mongosDB.dropDatabase());

// Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.shard0.shardName}));

// Shard the test collection on _id.
assert.commandWorked(mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

// Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
assert.commandWorked(mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

// Move the [0, MaxKey] chunk to st.shard1.shardName.
assert.commandWorked(
    mongosDB.adminCommand({moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.shard1.shardName}),
);

// Write a document to each chunk.
assert.commandWorked(mongosColl.insert({_id: -1}));
assert.commandWorked(mongosColl.insert({_id: 1}));

// Delay messages between shard 1 and the mongos, long enough that shard 1's responses will
// likely arrive after the response from shard 0, but not so long that the background cluster
// client cleanup job will have been given a chance to run.
const delayMillis = 100;
st.rs1.getPrimary().delayMessagesFrom(st.s, delayMillis);

const nTrials = 10;
for (let i = 1; i < 10; ++i) {
    // This will trigger an error on shard 0, but not shard 1. We set up a delay from shard 1,
    // so the response should get back after the error has been returned to the client. We use a
    // batch size of 0 to ensure the error happens during a getMore.
    assert.throws(() =>
        mongosColl
            .aggregate([{$project: {_id: 0, x: {$divide: [2, {$add: ["$_id", 1]}]}}}], {cursor: {batchSize: 0}})
            .itcount(),
    );
}

st.stop();
