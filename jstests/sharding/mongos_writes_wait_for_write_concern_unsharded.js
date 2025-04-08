/**
 * Tests that commands that accept write concern correctly return write concern errors when run
 * through mongos on unsharded collections.
 *
 * @tags: [
 * assumes_balancer_off,
 * does_not_support_stepdowns,
 * multiversion_incompatible,
 * uses_transactions,
 * tsan_incompatible,
 * requires_persistence,
 * ]
 *
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    checkWriteConcernBehaviorAdditionalCRUDOps,
    checkWriteConcernBehaviorForAllCommands
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

jsTest.log("Testing all commands on an unsharded collection.");
checkWriteConcernBehaviorForAllCommands(
    st.s, st, "sharded" /* clusterType */, null /* presetup */, false /* shardedCollection */);
checkWriteConcernBehaviorAdditionalCRUDOps(
    st.s, st, "sharded" /* clusterType */, null /* presetup */, false /* shardedCollection */);

st.stop();
