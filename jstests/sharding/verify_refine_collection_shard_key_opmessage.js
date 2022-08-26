/**
 * Test that a special {op: 'n'} oplog event is created during refineCollectionShardKey command.
 *
 * @tags: [requires_fcv_61]
 */
(function() {
"use strict";

const st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 2}});

const mongos = st.s0;
const primaryShard = st.shard0.shardName;
const kDbName = 'refineShardKey';
const kCollName = 'coll';
const kNsName = kDbName + '.' + kCollName;

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, primaryShard);
assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: {_id: 1}}));
assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, akey: 1}));
assert.commandWorked(
    mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, akey: 1}}));

const o2expected = {
    refineCollectionShardKey: "refineShardKey.coll",
    shardKey: {_id: 1, akey: 1},
    oldShardKey: {_id: 1}
};

const oplog = st.rs0.getPrimary().getCollection("local.oplog.rs");
const logEntry = oplog.findOne({ns: kNsName, op: 'n', "o2.refineCollectionShardKey": kNsName});
assert(logEntry != null);
assert.eq(bsonWoCompare(logEntry.o2, o2expected), 0, logEntry);

st.stop();
})();
