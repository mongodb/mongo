/**
 * Tests that commands that accept write concern correctly return write concern errors when run
 * through mongos.
 *
 * @tags: [
 * assumes_balancer_off,
 * does_not_support_stepdowns,
 * multiversion_incompatible,
 * uses_transactions,
 * ]
 *
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    checkWriteConcernBehaviorAdditionalCRUDOps,
    precmdShardKey
} from "jstests/libs/write_concern_all_commands.js";

var st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 3}});

assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {"level": "local"}}));

jsTest.log(
    "Testing addl. CRUD commands on a sharded collection where {_id: 1} is the shard key. The writes will take the target without shard key path.");
const precmdShardKeyId = precmdShardKey.bind(null, "_id");

checkWriteConcernBehaviorAdditionalCRUDOps(st.s,
                                           st,
                                           "sharded" /* clusterType */,
                                           precmdShardKeyId,
                                           true /* shardedCollection */,
                                           true /* writeWithoutSk */);

jsTest.log(
    "Testing addl. CRUD commands on a sharded collection where {a : 1} is the shard key. The writes will take the target by shard key path.");

const precmdShardKeyA = precmdShardKey.bind(null, "a");

checkWriteConcernBehaviorAdditionalCRUDOps(st.s,
                                           st,
                                           "sharded" /* clusterType */,
                                           precmdShardKeyA,
                                           true /* shardedCollection */,
                                           false /* writeWithoutSk */);

jsTest.log(
    "Testing addl. CRUD commands on a sharded collection where {x : 1} is the shard key. The writes will take the target without shard key path.");

const precmdShardKeyX = precmdShardKey.bind(null, "x");

checkWriteConcernBehaviorAdditionalCRUDOps(st.s,
                                           st,
                                           "sharded" /* clusterType */,
                                           precmdShardKeyX,
                                           true /* shardedCollection */,
                                           true /* writeWithoutSk */);

st.stop();
