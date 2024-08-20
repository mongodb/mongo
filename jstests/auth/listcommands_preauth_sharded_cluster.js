/*
 * Make sure that listCommands on sharded clusters doesn't require authentication.
 * @tags: [requires_sharding]
 */
import {runTest} from "jstests/auth/listcommands_preauth_base.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st =
    new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
runTest(st.s0);
st.stop();