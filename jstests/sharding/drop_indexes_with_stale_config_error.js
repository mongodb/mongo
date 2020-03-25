//
// Tests that a StaleConfigError received from a shard on dropIndexes will allow the command to
// successfully complete upon the mongos' command retry.
//

(function() {
'use strict';

load("jstests/sharding/libs/shard_versioning_util.js");
load('jstests/sharding/libs/sharded_transactions_helpers.js');

const st = new ShardingTest({mongos: 2, shards: 2});
const dbName = jsTestName();
const collName = "coll";
const ns = dbName + "." + collName;
const mongos0Coll = st.s0.getDB(dbName)[collName];
const mongos1Coll = st.s1.getDB(dbName)[collName];

// Shard the collection and create an index
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 100}, to: st.shard1.shardName}));
flushRoutersAndRefreshShardMetadata(st, {ns});

assert.commandWorked(mongos0Coll.ensureIndex({x: 1}));

// Move chunk without refreshing the recipient so that the recipient shard throws a
// StaleShardVersion error upon receiving the drop index command.
ShardVersioningUtil.moveChunkNotRefreshRecipient(st.s1, ns, st.shard1, st.shard0, {x: 100});

assert.commandWorked(mongos0Coll.dropIndexes({x: 1}));

st.stop();
})();
