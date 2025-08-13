/**
 * Tests that commands that accept write concern correctly return write concern errors when run
 * through mongos on unsharded collections.
 *
 * @tags: [
 * assumes_balancer_off,
 * does_not_support_stepdowns,
 * multiversion_incompatible,
 * uses_transactions,
 * ]
 *
 */

import {
    checkWriteConcernBehaviorAdditionalCRUDOps,
    checkWriteConcernBehaviorForAllCommands
} from "jstests/libs/write_concern_all_commands.js";

var st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 3},
    configReplSetTestOptions: {settings: {electionTimeoutMillis: ReplSetTest.kForeverMillis}},
    other: {rsOptions: {settings: {electionTimeoutMillis: ReplSetTest.kForeverMillis}}}
});

assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {"level": "local"}}));

jsTest.log("Testing all commands on an unsharded collection.");
checkWriteConcernBehaviorForAllCommands(
    st.s, st, "sharded" /* clusterType */, null /* presetup */, false /* shardedCollection */);
checkWriteConcernBehaviorAdditionalCRUDOps(
    st.s, st, "sharded" /* clusterType */, null /* presetup */, false /* shardedCollection */);

st.stop();
