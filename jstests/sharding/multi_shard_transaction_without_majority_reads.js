/**
 * Test that multi-shard transactions will fail with a non-transient error when run against shards
 * with disabled majority read concern.
 *
 * @tags: [uses_transactions]
 */

(function() {
'use strict';

const st = new ShardingTest({shards: 2, rs: {nodes: 1, enableMajorityReadConcern: 'false'}});

assert.commandWorked(st.s0.adminCommand({enableSharding: 'TestDB'}));
st.ensurePrimaryShard('TestDB', st.shard0.shardName);
assert.commandWorked(st.s0.adminCommand({shardCollection: 'TestDB.TestColl', key: {_id: 1}}));

const coll = st.s0.getDB('TestDB').TestColl;
assert.commandWorked(coll.insert({_id: -1, x: 0}));
assert.commandWorked(coll.insert({_id: 1, x: 0}));
assert.commandWorked(st.s0.adminCommand({split: 'TestDB.TestColl', middle: {_id: 1}}));
assert.commandWorked(
    st.s0.adminCommand({moveChunk: 'TestDB.TestColl', find: {_id: 1}, to: st.shard1.shardName}));

assert.commandWorked(coll.update({_id: -1}, {$inc: {x: 1}}));
assert.commandWorked(coll.update({_id: 1}, {$inc: {x: 1}}));

const session = st.s0.startSession();
const sessionColl = session.getDatabase('TestDB').TestColl;

session.startTransaction();

assert.commandWorked(sessionColl.update({_id: -1}, {$inc: {x: 1}}));
assert.commandWorked(sessionColl.update({_id: 1}, {$inc: {x: 1}}));

assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                             ErrorCodes.ReadConcernMajorityNotEnabled);

st.stop();
})();
