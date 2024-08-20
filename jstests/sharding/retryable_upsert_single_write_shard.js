/**
 * Tests that single-write-shard commit is not used for updates that cause WCOS errors thereby
 * preserving the safety of retryable writes.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";
import {ShardingStateTest} from "jstests/sharding/libs/sharding_state_test.js";

const st = new ShardingTest({shards: 2, mongos: 1, rs: {nodes: 3}});
const mongos = st.s;

let db = mongos.getDB(jsTestName());
let coll = db.coll;
const collName = coll.getName();
coll.drop();

CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {x: 1}, [
    {min: {x: MinKey}, max: {x: 0}, shard: st.shard0.shardName},
    {min: {x: 0}, max: {x: MaxKey}, shard: st.shard1.shardName},
]);

assert.commandWorked(
    db.adminCommand({moveChunk: coll.getFullName(), find: {x: -100}, to: st.shard0.shardName}));
assert.commandWorked(
    db.adminCommand({moveChunk: coll.getFullName(), find: {x: 100}, to: st.shard1.shardName}));

// This retryable update command causes a WCOS error if there is no match found and inserts the
// document on different shard in a distributed transaction. If a match is found, the document is
// updated without triggering WCOS error.
const updateCmd = {
    update: collName,
    updates: [
        {q: {x: -50}, u: {$set: {y: -51}, $setOnInsert: {x: 50}}, upsert: true},
    ],
    ordered: false,
    lsid: {id: UUID()},
    txnNumber: NumberLong(5)
};

assert.commandWorked(db.runCommand(updateCmd));

// Insert a document matching the query for retryable write that got executed.
assert.commandWorked(coll.insertOne({x: -50}));

// Failover shard0 to cause it to forget the in memory txn state from being a txn participant.
ShardingStateTest.failoverToMember(st.rs0, st.rs0.getSecondary());

// Retry the earlier retryable upsert.
db = st.s.getDB(jsTestName());
coll = db.coll;

// Insert a document matching the update query.
// Add retries on retryable error as the connections in ShardingTest may be stale and can fail with
// NotWritablePrimary
retryOnRetryableError(() => coll.insertOne({x: -50}), 10, 30);

// A retry of the update should fail.
assert.commandFailedWithCode(db.runCommand(updateCmd), ErrorCodes.IncompleteTransactionHistory);
assert.neq(null, coll.findOne({x: -50}));

st.stop();
