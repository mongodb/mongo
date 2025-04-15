/**
 * Tests that commands that accept write concern correctly return write concern errors when run
 * through mongos.
 *
 * @tags: [
 * assumes_balancer_off,
 * does_not_support_stepdowns,
 * multiversion_incompatible,
 * uses_transactions,
 * incompatible_aubsan,
 * tsan_incompatible,
 * requires_persistence,
 * ]
 *
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    checkWriteConcernBehaviorForAllCommands,
    precmdShardKey
} from "jstests/libs/write_concern_all_commands.js";

const overrideInternalWriteConcernTimeout = {
    setParameter: {
        'failpoint.overrideInternalWriteConcernTimeout':
            tojson({mode: 'alwaysOn', data: {wtimeoutMillis: 5000}})
    }
};
const stOtherOptions = {
    mongosOptions: overrideInternalWriteConcernTimeout,
    rsOptions: overrideInternalWriteConcernTimeout,
    configOptions: overrideInternalWriteConcernTimeout,
    enableBalancer: false
};

var st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 3}, other: stOtherOptions});
assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {"level": "local"}}));

jsTest.log("Testing all commands on a sharded collection where {_id: 1} is the shard key.");
const precmdShardKeyId = precmdShardKey.bind(null, "_id");

checkWriteConcernBehaviorForAllCommands(
    st.s, st, "sharded" /* clusterType */, precmdShardKeyId, true /* shardedCollection */);

st.stop();

st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 3}, other: stOtherOptions});
st.stopBalancer();

assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {"level": "local"}}));

jsTest.log("Testing all commands on a sharded collection where {a : 1} is the shard key.");

const precmdShardKeyA = precmdShardKey.bind(null, "a");

checkWriteConcernBehaviorForAllCommands(
    st.s, st, "sharded" /* clusterType */, precmdShardKeyA, true /* shardedCollection */);

st.stop();
