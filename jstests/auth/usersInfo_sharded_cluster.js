/*
 * Test behavior and edge cases in usersInfo on sharded clusters.
 * @tags: [requires_sharding]
 */
import {runTest} from "jstests/auth/usersInfo_base.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, mongos: 1, config: 1});
runTest(st.s0);
st.stop();