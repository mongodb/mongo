/**
 * Tests that delete queries that are not an exact match on shard key and target only a single shard
 * work without having to use the two phase protocol.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertExplainTargetsCorrectShard,
    splitAndMoveChunks
} from "jstests/sharding/libs/without_two_phase_util.js";

const st = new ShardingTest({
    shards: 2,

    // To test this feature we need to disable the two phase protocol as it will
    // broadcast any non targeted write.
    mongosOptions: {setParameter: {featureFlagUpdateOneWithoutShardKey: false}},
    configOptions: {setParameter: {featureFlagUpdateOneWithoutShardKey: false}},
    rsOptions: {setParameter: {featureFlagUpdateOneWithoutShardKey: false}},
});

// Setup the test by creating two chunks:
//   1. The first chunk has documents with "a" from [min, 2) and is on shard0.
//   2. The second chunk has documents with "a" from [2, max) and is on shard1.
const dbName = "test";
const coll = "sharded_coll";
const ns = dbName + "." + coll;
const db = st.getDB(dbName);
const docsToInsert = [{a: 1, b: 1}, {a: 2, b: 1}, {a: 3, b: 1}, {a: 999, b: 1}];
assert.commandWorked(st.s0.adminCommand({shardcollection: ns, key: {a: 1, b: 1}}));
db.sharded_coll.insert(docsToInsert);
splitAndMoveChunks(st,
                   {a: 2, b: 1} /* split point */,
                   {a: 1, b: 1} /* move chunk containing doc to shard0 */,
                   {a: 3, b: 1} /* move chunk containing doc to shard1 */);

// deleteOne query without the full shard key and only one shard targeted should succeed.
assert.eq(db.sharded_coll.count({a: 999}), 1);
let cmdObj = {delete: coll, deletes: [{q: {a: 999}, limit: 1}]};
assert.commandWorked(db.runCommand(cmdObj));
assert.eq(db.sharded_coll.count({a: 999}), 0);
assertExplainTargetsCorrectShard(db, cmdObj, st.shard1.shardName);

// deleteOne query without the full shard key and multiple shards targeted should fail.
cmdObj = {
    delete: coll,
    deletes: [{q: {a: {$gt: 0}}, limit: 1}]
};
assert.commandFailedWithCode(db.runCommand(cmdObj), ErrorCodes.ShardKeyNotFound);
assert.commandFailedWithCode(db.runCommand({explain: cmdObj}), ErrorCodes.ShardKeyNotFound);

// deleteOne query without the shard key should fail (as this would requiring targeting more than
// one shard).
cmdObj = {
    delete: coll,
    deletes: [{q: {nonShardKey: 12345}, limit: 1}]
};
assert.commandFailedWithCode(db.runCommand(cmdObj), ErrorCodes.ShardKeyNotFound);
assert.commandFailedWithCode(db.runCommand({explain: cmdObj}), ErrorCodes.ShardKeyNotFound);

// deleteMany query that targets only one shard should work correctly.
assert.eq(db.sharded_coll.count({a: 1}), 1);
cmdObj = {
    delete: coll,
    deletes: [{q: {a: 1}, limit: 0}]
};
assert.commandWorked(db.runCommand(cmdObj));
assert.eq(db.sharded_coll.count({a: 1}), 0);
assert.eq(db.sharded_coll.count({a: {$gt: 0}}), 2);  // Check that other documents were not deleted.
assertExplainTargetsCorrectShard(db, cmdObj, st.shard0.shardName);

st.stop();
